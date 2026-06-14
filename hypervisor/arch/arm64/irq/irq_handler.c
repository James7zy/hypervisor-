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
#include <vm.h>
#include <board.h>
#include <vgic.h>
#include <gic_v3.h>

void el2_irq_handler(void)
{
    u32 intid = gic_ack_irq();

    if (intid == BOARD_VTIMER_IRQ) {
        vgic_inject_hw(&g_vm.vcpu, BOARD_VTIMER_IRQ, BOARD_VTIMER_IRQ, 0xA0);
        gic_priority_drop(intid);   /* EOIR1 only — leave Active (ADR-0001) */
    } else {
        /* Unexpected (incl. spurious 1023): drop AND deactivate. */
        gic_priority_drop(intid);
        gic_deactivate(intid);
    }
}
