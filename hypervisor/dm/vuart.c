/* SPDX-License-Identifier: TBD */
/*
 * PL011 "vuart": trap-and-emulate console for VM0's guest (M5 slice 2).
 *
 * EL2 now owns the physical PL011 exclusively (see uart_pl011.c / print.c);
 * the guest's IPA window at BOARD_UART_BASE is punched out of Stage-2
 * (stage2.c) so every guest access traps here instead of touching hardware.
 * TX is forwarded to the physical UART via console_putc (serialized with
 * printk under the same lock); RX is filled by EL2's IRQ handler draining the
 * physical FIFO and calling vuart_rx() (irq_handler.c, BOARD_PL011_IRQ branch).
 *
 * Registered ONCE globally on the M3.1 MMIO bus; the handler resolves the
 * faulting VM per-access via current_vcpu()->owner, matching vgic_v3_mmio.c's
 * idiom. Single VM, single vCPU-0 injection target (console focus / VM1
 * arrive in slice 3).
 */
#include <types.h>
#include <printk.h>
#include <board.h>
#include <percpu.h>
#include <vm.h>
#include <vm_config.h>   /* struct vm_config (pcpu_base) */
#include "../arch/arm64/vmexit/mmio.h"
#include "../arch/arm64/irq/vgic.h"
#include "../arch/arm64/irq/vgic_sgi.h"
#include "vuart.h"

/* ── PL011 register offsets this model implements ── */
#define VUART_DR     0x000U
#define VUART_IBRD   0x024U
#define VUART_FBRD   0x028U
#define VUART_LCR_H  0x02CU
#define VUART_CR     0x030U
#define VUART_IFLS   0x034U
#define VUART_IMSC   0x038U
#define VUART_RIS    0x03CU
#define VUART_MIS    0x040U
#define VUART_ICR    0x044U
#define VUART_FR     0x018U

#define VUART_FR_RXFE  (1U << 4)
#define VUART_FR_TXFF  (1U << 5)

/* UARTRIS/UARTIMSC/UARTICR bit positions (ARM PL011 TRM): receive (RXIS,
 * bit 4) and receive-timeout (RTIS, bit 6). This model raises both together
 * on any RX push -- Linux's pl011 driver treats either as "data available"
 * without distinguishing the trigger, so collapsing them is safe. */
#define VUART_RIS_RXIS  (1U << 4)
#define VUART_RIS_RTIS  (1U << 6)
#define VUART_RIS_RX_MASK (VUART_RIS_RXIS | VUART_RIS_RTIS)

/* AMBA PrimeCell identification block at 0xFE0..0xFFC (PeriphID0-3, then
 * PCellID0-3, 4 bytes apart). Linux's amba bus driver reads these BEFORE
 * binding the pl011 driver -- wrong values here mean ttyAMA0 never probes. */
static const u8 vuart_amba_id[8] = {
    0x11, 0x10, 0x14, 0x00, 0x0D, 0xF0, 0x05, 0xB1,
};

/* M5 slice 3: RX console focus. VM0 by default (matches pre-slice-3
 * single-VM behaviour). See the declaration in vuart.h. */
u32 console_focus = 0;

static bool vuart_rx_empty(const struct vuart *u)
{
    return u->rx_head == u->rx_tail;
}

static bool vuart_rx_full(const struct vuart *u)
{
    return ((u->rx_head + 1U) % VUART_RX_FIFO) == u->rx_tail;
}

static u8 vuart_rx_pop(struct vuart *u)
{
    u8 c = u->rx_buf[u->rx_tail];
    u->rx_tail = (u->rx_tail + 1U) % VUART_RX_FIFO;
    return c;
}

static u32 vuart_read(struct vuart *u, u64 off)
{
    if (off >= 0xFE0U && off <= 0xFFCU && ((off & 0x3U) == 0U)) {
        u32 idx = (u32)((off - 0xFE0U) >> 2);
        return (idx < 8U) ? (u32)vuart_amba_id[idx] : 0U;
    }

    switch (off) {
    case VUART_DR: {
        if (vuart_rx_empty(u))
            return 0U;
        u8 c = vuart_rx_pop(u);
        if (vuart_rx_empty(u))
            u->ris &= ~VUART_RIS_RX_MASK;
        return (u32)c;
    }
    case VUART_FR:
        return vuart_rx_empty(u) ? VUART_FR_RXFE : 0U;   /* TXFF never set */
    case VUART_RIS:
        return u->ris;
    case VUART_MIS:
        return u->ris & u->imsc;
    case VUART_IBRD:  return u->ibrd;
    case VUART_FBRD:  return u->fbrd;
    case VUART_LCR_H: return u->lcr_h;
    case VUART_CR:    return u->cr;
    case VUART_IFLS:  return u->ifls;
    case VUART_IMSC:  return u->imsc;
    default:
        return 0U;   /* RAZ */
    }
}

static void vuart_write(struct vm *m, struct vuart *u, u64 off, u32 val)
{
    switch (off) {
    case VUART_DR:
        /* Guest TX: one character, forwarded to the real UART under the
         * printk lock so EL2 and guest output never interleave mid-char. No
         * FIFO/blocking modelled -- console_putc blocks on the physical FR
         * internally, which is enough for this single-VM slice. */
        console_putc((char)(u8)val);
        break;
    case VUART_IMSC:
        u->imsc = val;
        break;
    case VUART_ICR:
        u->ris &= ~val;
        break;
    case VUART_IBRD:  u->ibrd  = val; break;
    case VUART_FBRD:  u->fbrd  = val; break;
    case VUART_LCR_H: u->lcr_h = val; break;
    case VUART_CR:    u->cr    = val; break;
    case VUART_IFLS:  u->ifls  = val; break;
    default:
        break;   /* WI, incl. DMACR */
    }
    (void)m;
}

static int vuart_mmio_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    struct vm *m = current_vcpu()->owner;
    struct vuart *u = &m->vuart;

    if (acc->is_write)
        vuart_write(m, u, acc->offset, (u32)acc->data);
    else
        acc->data = vuart_read(u, acc->offset);

    return 0;
}

void vuart_bus_init(void)
{
    int r = mmio_bus_register(BOARD_UART_BASE, 0x1000ULL, vuart_mmio_handler, NULL);
    if (r != 0)
        printk("[hv] vuart: bus full, registration failed\n");
    else
        printk("[hv] vuart: PL011 0x%lx/0x1000 registered\n",
               (unsigned long)BOARD_UART_BASE);
}

bool vuart_rx_has_room(const struct vm *m)
{
    return !vuart_rx_full(&m->vuart);
}

void vuart_rx(struct vm *m, u8 ch)
{
    struct vuart *u = &m->vuart;

    if (!vuart_rx_full(u)) {
        u->rx_buf[u->rx_head] = ch;
        u->rx_head = (u->rx_head + 1U) % VUART_RX_FIFO;
    }
    /* else: silently drop. Callers are expected to check vuart_rx_has_room()
     * before pulling a byte off the physical UART, so this only fires if the
     * ring filled between that check and this call -- not expected on the
     * current single-drain-loop caller, but kept as a safe fallback rather
     * than corrupting the ring. */

    u->ris |= VUART_RIS_RX_MASK;
    if (u->imsc & VUART_RIS_RX_MASK) {
        /*
         * M5 slice 3: the physical PL011 IRQ (and hence this whole call) is
         * always serviced on the pCPU that owns the physical UART (CPU0);
         * with console focus now routable to VM1, the target vCPU0 may live
         * on a DIFFERENT pCPU than the one running this code.
         * vgic_inject_spi writes the LIVE ICH_LR1_EL2 of whichever vCPU is
         * actually loaded on the CALLING core -- correct only when the
         * target vCPU IS the caller's own current vCPU (VM0's steady state,
         * unchanged). For any other VM, only the shadow ich_lr[1] write is
         * safe here; the owning pCPU must reload it itself once kicked into
         * EL2 (vgic_reload_spi_lr, called from el2_irq_handler's kick-SGI
         * branch), the same cross-core pattern already used for SGI/IPI.
         */
        u32 target_pcpu = m->config->pcpu_base;   /* vCPU0 always owns console injection */
        if (target_pcpu == current_vcpu_id()) {
            vgic_inject_spi(&m->vcpu[0], BOARD_PL011_IRQ);
        } else {
            vgic_set_spi_shadow(&m->vcpu[0], BOARD_PL011_IRQ);
            asm volatile("dsb ish" ::: "memory");
            vgic_kick_pcpu(target_pcpu);
        }
    }
}
