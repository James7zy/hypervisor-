/* SPDX-License-Identifier: TBD */
/*
 * irq_handler_asm.S — EL2 physical-IRQ entry (Lower EL AArch64 IRQ, +0x480).
 *
 * Mirrors el1_sync_handler (vmexit_asm.S): saves the guest register frame to
 * the current vCPU's regs (reached via TPIDR_EL2 → percpu[id].cur_vcpu),
 * calls el2_irq_handler(), then restores and erets back to the
 * guest. Differs only in (a) the C callee and (b) the trailing
 * `msr daifclr, #2` that re-unmasks EL2 IRQs before the eret, so a still-pending
 * physical interrupt is taken again rather than lost.
 */

#include <vm.h>       /* VCPU_* offset macros (active in __ASSEMBLER__) */
#include <percpu.h>   /* PERCPU_CUR_VCPU — current vCPU via TPIDR_EL2 */

    .section .text
    .globl el1_irq_handler_asm
el1_irq_handler_asm:
    /* Save x0 on EL2 stack to free it as pointer */
    str     x0, [sp, #-16]!

    /* Load this pCPU's current vCPU regs via TPIDR_EL2 (&percpu[id]->cur_vcpu;
     * cur_vcpu is the first field, so it points at vcpu->regs). */
    mrs     x0, tpidr_el2
    ldr     x0, [x0, #PERCPU_CUR_VCPU]

    /* Save x1–x30 (genuine guest values, untouched since exception fired) */
    str     x1,       [x0, #0x08]
    stp     x2,  x3,  [x0, #0x10]
    stp     x4,  x5,  [x0, #0x20]
    stp     x6,  x7,  [x0, #0x30]
    stp     x8,  x9,  [x0, #0x40]
    stp     x10, x11, [x0, #0x50]
    stp     x12, x13, [x0, #0x60]
    stp     x14, x15, [x0, #0x70]
    stp     x16, x17, [x0, #0x80]
    stp     x18, x19, [x0, #0x90]
    stp     x20, x21, [x0, #0xA0]
    stp     x22, x23, [x0, #0xB0]
    stp     x24, x25, [x0, #0xC0]
    stp     x26, x27, [x0, #0xD0]
    stp     x28, x29, [x0, #0xE0]
    str     x30,      [x0, #0xF0]

    /* Retrieve guest x0 from stack and save it */
    ldr     x1, [sp], #16
    str     x1, [x0, #0x00]

    /* Save guest system registers */
    mrs     x1, sp_el1
    str     x1, [x0, #VCPU_SP_EL1]
    mrs     x1, elr_el2
    str     x1, [x0, #VCPU_ELR]
    mrs     x1, spsr_el2
    str     x1, [x0, #VCPU_SPSR]

    bl      el2_irq_handler

    /* Restore guest state and return (current vCPU via TPIDR_EL2) */
    mrs     x0, tpidr_el2
    ldr     x0, [x0, #PERCPU_CUR_VCPU]
    ldr     x1, [x0, #VCPU_ELR]
    msr     elr_el2, x1
    ldr     x1, [x0, #VCPU_SPSR]
    msr     spsr_el2, x1
    ldr     x1, [x0, #VCPU_SP_EL1]
    msr     sp_el1, x1
    ldr     x1,       [x0, #0x08]
    ldp     x2,  x3,  [x0, #0x10]
    ldp     x4,  x5,  [x0, #0x20]
    ldp     x6,  x7,  [x0, #0x30]
    ldp     x8,  x9,  [x0, #0x40]
    ldp     x10, x11, [x0, #0x50]
    ldp     x12, x13, [x0, #0x60]
    ldp     x14, x15, [x0, #0x70]
    ldp     x16, x17, [x0, #0x80]
    ldp     x18, x19, [x0, #0x90]
    ldp     x20, x21, [x0, #0xA0]
    ldp     x22, x23, [x0, #0xB0]
    ldp     x24, x25, [x0, #0xC0]
    ldp     x26, x27, [x0, #0xD0]
    ldp     x28, x29, [x0, #0xE0]
    ldr     x30,      [x0, #0xF0]
    ldr     x0,       [x0, #0x00]
    /*
     * Do NOT unmask EL2 IRQs here. EL2 has no current-EL IRQ handler (vectors.S
     * routes "Current EL SP_ELx / IRQ" to panic_vector). A pending physical IRQ
     * unmasked in the one-instruction window before `eret` is taken AT EL2 and
     * panics. The `eret` restores the guest's SPSR; with HCR_EL2.IMO=1 the next
     * physical IRQ is taken from the guest via the Lower-EL vector (+0x480),
     * which re-enters this handler. A level-sensitive line (e.g. the vtimer,
     * left Active per ADR-0001) stays pending and is serviced on that path.
     */
    eret
