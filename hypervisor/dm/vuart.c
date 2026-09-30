/* SPDX-License-Identifier: TBD */
/*
 * PL011 "vuart": per-VM trap-and-emulate console.
 *
 * EL2 now owns the physical PL011 exclusively (see drivers/uart/pl011.c / print.c);
 * the guest's window at config->vuart_base is punched out of Stage-2
 * (stage2.c) so every guest access traps here instead of touching hardware.
 * TX is forwarded to the physical UART via console_putc (serialized with
 * printk under the same lock); RX is filled by EL2's IRQ handler draining the
 * physical FIFO and calling vuart_rx() (console_rx_drain, dm/console.c).
 *
 * Registered ONCE globally on the M3.1 MMIO bus; the handler resolves the
 * faulting VM per-access via current_vcpu()->owner, matching vgic_v3_mmio.c's
 * idiom. Each VM's vCPU0 is its console interrupt target; vGIC owns delivery.
 */
#include <types.h>
#include <printk.h>
#include <percpu.h>
#include <vm.h>
#include <mmio.h>
#include "vm_config.h"   /* vuart_base / vuart_irq */
#include "vuart.h"
#include "console.h"   /* console_focus */

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

static void vuart_write(struct vuart *u, u64 off, u32 val)
{
    switch (off) {
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
}

static int vuart_mmio_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    struct vm *m = current_vcpu()->owner;
    struct vuart *u = &m->vuart;

    /* TX can wait on hardware and takes the print lock, never the device lock. */
    if (acc->is_write && acc->offset == VUART_DR) {
        console_putc((char)(u8)acc->data);
        return 0;
    }
    spin_lock(&u->lock);
    if (acc->is_write)
        vuart_write(u, acc->offset, (u32)acc->data);
    else
        acc->data = vuart_read(u, acc->offset);
    spin_unlock(&u->lock);

    return 0;
}

void vuart_bus_init(void)
{
    /* One global registration serves every VM: the handler resolves the VM
     * per access, and all VMs share one guest address map (ADR-0014). */
    uintptr_t base = vm[0].config->vuart_base;
    for (u32 i = 1; i < (u32)NR_VMS; i++) {
        if (vm[i].config->vuart_base != base) {
            printk("[hv] BUG: vuart: VM%u base 0x%lx != VM0 base 0x%lx\n",
                   (unsigned)i, (unsigned long)vm[i].config->vuart_base,
                   (unsigned long)base);
            return;
        }
    }

    int r = mmio_bus_register(base, 0x1000ULL, vuart_mmio_handler, NULL);
    if (r != 0)
        printk("[hv] vuart: bus full, registration failed\n");
    else
        printk("[hv] vuart: PL011 0x%lx/0x1000 registered\n",
               (unsigned long)base);
}

bool vuart_rx_has_room(struct vm *m)
{
    spin_lock(&m->vuart.lock);
    bool room = !vuart_rx_full(&m->vuart);
    spin_unlock(&m->vuart.lock);
    return room;
}

void vuart_rx(struct vm *m, u8 ch)
{
    struct vuart *u = &m->vuart;

    spin_lock(&u->lock);
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
    bool notify = (u->imsc & VUART_RIS_RX_MASK) != 0;
    spin_unlock(&u->lock);
    /* Mask decision linearizes at the snapshot; no retroactive unmask delivery. */
    if (notify)
        vcpu_arch_inject_irq(&m->vcpu[0], m->config->vuart_irq);
}
