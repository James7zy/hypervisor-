/* SPDX-License-Identifier: TBD */
/*
 * EL2 physical-IRQ handler.
 *
 * Called from el1_irq_handler_asm after the guest frame is saved. The only
 * expected interrupt in M2.5 is the EL1 virtual-timer PPI (INTID 27): it is
 * hardware-forwarded into the guest's vGIC and only priority-dropped here —
 * never deactivated — so the level-sensitive line cannot re-pend and storm the
 * guest before it runs (ADR-0001). The guest's deactivate of the virtual IRQ
 * releases the physical one via the ICH_LR HW linkage.
 */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <board.h>
#include <percpu.h>
#include <vgic.h>
#include <gic_v3.h>
#include <uart.h>
#include <vuart.h>
#include "vgic_debug.h"
#include "vgic_sgi.h"

/* ICH_VMCR_EL2.VENG1 (bit 1): the guest's virtual Group-1 enable. If clear, the
 * guest cannot take a virtual Group-1 IRQ yet (its vGIC interface is not up). */
static inline bool guest_veng1(void)
{
    u64 vmcr;
    asm volatile("mrs %0, S3_4_C12_C11_7" : "=r"(vmcr));   /* ICH_VMCR_EL2 */
    return (vmcr & (1ULL << 1)) != 0ULL;
}

void el2_irq_handler(void)
{
    u32 intid = gic_ack_irq();

    if (intid == BOARD_KICK_SGI) {
        /* Cross-core IPI: drain this pCPU's pending SGI bitmap, inject each as
         * a virtual SGI, then priority-drop AND deactivate the kick (no LR
         * linkage gates it). */
        vgic_sgi_drain(current_vcpu_id());
        gic_priority_drop(intid);
        gic_deactivate(intid);
    } else if (intid == BOARD_VTIMER_IRQ) {
        /* vtimer PPI is per-CPU: inject into THIS core's current vCPU. */
        if (guest_veng1()) {
            /* Guest's vGIC interface is up: hardware-forward (HW=1) and leave
             * Active — the guest's deactivate releases the physical via the LR
             * linkage (ADR-0001). This is the steady-state path (always true on
             * cpu0). */
            vgic_dbg("inject HW PPI=%u (timer, ICH_LR0)\n", intid);
            vgic_inject_hw(current_vcpu(), BOARD_VTIMER_IRQ, BOARD_VTIMER_IRQ, 0xA0);
            gic_priority_drop(intid);   /* EOIR1 only — leave Active (ADR-0001) */
        } else {
            /*
             * Guest's vGIC interface is NOT up yet (early SMP secondary bring-up:
             * the guest arms its vtimer before enabling VENG1). A HW=1 forward
             * would wedge Active (the guest can't take/deactivate it) and the
             * core would never tick again. Instead: software-inject (HW=0) so
             * the vIRQ sits Pending in the LR for when the guest enables its
             * interface; deactivate the physical; and MASK the physical PPI at
             * this cpu's redistributor to stop the storm. The guest's later
             * write to its virtual PPI-27 enable re-enables the physical PPI
             * (vgicr_write_sgi → gic_ppi_set_enable). */
            vgic_inject_sw(current_vcpu(), BOARD_VTIMER_IRQ, 0xA0);
            gic_priority_drop(intid);
            gic_deactivate(intid);
            gic_ppi_set_enable(current_vcpu_id(), BOARD_VTIMER_IRQ, false);
        }
    } else if (intid == BOARD_PL011_IRQ) {
        /*
         * PL011 RX: EL2 owns the physical UART exclusively (M5 slice 2). Drain
         * the physical FIFO here and hand each byte to the vuart model, which
         * buffers it and re-injects a SOFTWARE SPI into the guest's vGIC
         * (vuart_rx -> vgic_inject_spi) if its virtual IMSC is unmasked.
         * Unlike the old passthrough design, the physical SPI is fully
         * deactivated below: there is no guest driver left to read DR
         * directly, so nothing needs the line held Active to avoid a storm --
         * that rationale only applied when the guest read hardware itself.
         *
         * Check vuart_rx_has_room() BEFORE uart_getc(): a physical DR read is
         * destructive (pops the hardware FIFO), so once the virtual ring is
         * full this loop must stop WITHOUT consuming the next physical byte --
         * QEMU's pl011 model backpressures its chardev on the physical FIFO
         * having room, so an un-popped byte simply waits for a later RX IRQ
         * once the guest drains the virtual ring, instead of being lost.
         */
        while (vuart_rx_has_room(&vm[0])) {
            int c = uart_getc();
            if (c < 0)
                break;
            vuart_rx(&vm[0], (u8)c);
        }
        gic_priority_drop(intid);
        gic_deactivate(intid);
    } else {
        /* Unexpected (incl. spurious 1023): drop AND deactivate. */
        gic_priority_drop(intid);
        gic_deactivate(intid);
    }
}
