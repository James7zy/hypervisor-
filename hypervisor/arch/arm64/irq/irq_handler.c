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
        vgic_dbg("inject SPI=%u (PL011 RX, ICH_LR1)\n", intid);
        /*
         * PL011 RX (ttyAMA0 passthrough). Software-inject the SPI into the
         * guest vGIC so its UART ISR runs and reads the RX byte from the
         * passed-through DR. Drop AND deactivate the physical SPI: the byte is
         * already latched in the PL011 FIFO, and QEMU only re-asserts the line
         * when there is fresh RX data, so this cannot storm. (Unlike the
         * vtimer, there is no HW-forward LR linkage to gate re-pend.)
         */
        vgic_inject_spi(&vm[0].vcpu[0], BOARD_PL011_IRQ);
        gic_priority_drop(intid);   /* leave Active so the level line cannot
                                     * re-pend and storm before the guest's ISR
                                     * reads DR (mirrors the vtimer, ADR-0001) */
    } else {
        /* Unexpected (incl. spurious 1023): drop AND deactivate. */
        gic_priority_drop(intid);
        gic_deactivate(intid);
    }
}
