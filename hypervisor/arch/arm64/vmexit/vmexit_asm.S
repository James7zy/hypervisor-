/* SPDX-License-Identifier: TBD */
/*
 * vmexit_asm.S — EL2↔EL1 context switch.
 *
 * vcpu_run(struct vcpu*)  — save HV callee-saved, load guest regs, eret to EL1
 * el1_sync_handler        — save guest regs, call handle_exit()
 * hv_restore()            — restore HV callee-saved, ret to vm_run()
 *
 * g_hv_ctx is defined here (BSS) — only accessed from this file.
 *
 * M5 slice 3: g_hv_ctx is now PER-pCPU (g_hv_ctx[NR_CPUS]), not a single
 * shared slot. With NR_VMS>1 more than one pCPU can independently be inside
 * its own vcpu_run/hv_restore pair at the same time (e.g. VM0's vCPU0 on
 * pCPU0 and VM1's vCPU0 on pCPU2 both issuing the debug HC_GUEST_DONE HVC
 * around the same wall-clock time) -- a single shared g_hv_ctx let one
 * pCPU's hv_restore() load ANOTHER pCPU's saved sp/lr, corrupting its C
 * stack. This was latent since M1 (nothing before M5 slice 3 ever had two
 * pCPUs concurrently past vcpu_run) and is caught by the new dual-VM svm4
 * test scenario. Slot index = this pCPU's id, derived from TPIDR_EL2 (=
 * &percpu[id]) exactly like the C-side current_vcpu_id() helper.
 */

#include <vm.h>       /* VCPU_* and HV_* macros (active only in __ASSEMBLER__ block) */
#include <percpu.h>   /* PERCPU_CUR_VCPU, NR_CPUS, PERCPU_SIZE */

    .section .bss
    .align 3
    .globl g_hv_ctx
g_hv_ctx:
    .zero HV_CTX_SIZE * NR_CPUS

/*
 * x9 <- &g_hv_ctx[this pCPU's id]. Clobbers x9, x10, x11 ONLY (never x0-x8,
 * so callers can compute this before or after loading x0..x8 with anything
 * live). id = (TPIDR_EL2 - &percpu[0]) / PERCPU_SIZE, matching the C-side
 * current_vcpu_id() derivation exactly.
 *
 * Cross-reference: current_vcpu_id() in hypervisor/include/percpu.h derives
 * "pCPU id for the currently executing core" independently, by reading the
 * stored percpu[id].cpu_id field instead of doing this pointer-difference
 * arithmetic. A _Static_assert can't span this asm macro's arithmetic vs that
 * C helper (unlike PERCPU_CUR_VCPU/PERCPU_SIZE below, which DO have
 * _Static_assert guards in percpu.h), so there is nothing enforcing these two
 * derivations agree except this comment: if you change either one, check the
 * other still computes the same pCPU id for the same core.
 */
    .macro hv_ctx_slot
    mrs     x9, tpidr_el2               /* &percpu[id] */
    adrp    x10, percpu
    add     x10, x10, :lo12:percpu
    sub     x9, x9, x10                 /* id * PERCPU_SIZE */
    mov     x11, #(PERCPU_SIZE)
    udiv    x9, x9, x11                 /* id */
    mov     x11, #(HV_CTX_SIZE)
    mul     x9, x9, x11                 /* id * HV_CTX_SIZE */
    adrp    x10, g_hv_ctx
    add     x10, x10, :lo12:g_hv_ctx
    add     x9, x9, x10                 /* &g_hv_ctx[id] */
    .endm

/* ------------------------------------------------------------------ */
/*  vcpu_run(struct vcpu *vcpu)   x0 = vcpu*                          */
/* ------------------------------------------------------------------ */
    .section .text
    .globl vcpu_run
vcpu_run:
    /* 1. Save hypervisor callee-saved registers. hv_ctx_slot only touches
     * x9-x11, leaving x0 (vcpu*) intact. */
    hv_ctx_slot
    stp     x19, x20, [x9, #0x00]
    stp     x21, x22, [x9, #0x10]
    stp     x23, x24, [x9, #0x20]
    stp     x25, x26, [x9, #0x30]
    stp     x27, x28, [x9, #0x40]
    str     x29,      [x9, #0x50]
    str     x30,      [x9, #HV_LR]
    mov     x10, sp
    str     x10,      [x9, #HV_SP]

    /* 2. Configure HCR_EL2 for this vCPU */
    ldr     x1, [x0, #VCPU_HCR_EL2]
    msr     hcr_el2, x1
    isb

    /* 3. Load guest system registers */
    ldr     x1, [x0, #VCPU_ELR]
    msr     elr_el2, x1
    ldr     x1, [x0, #VCPU_SPSR]
    msr     spsr_el2, x1
    ldr     x1, [x0, #VCPU_SP_EL1]
    msr     sp_el1, x1

    /* 4. Load guest GPRs (x0 last to preserve pointer) */
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
    ldr     x0,       [x0, #0x00]   /* x0 last */

    /*
     * Do NOT unmask EL2 IRQs before `eret`. With HCR_EL2.IMO=1 a physical IRQ
     * taken while the guest runs at EL1 is delivered to EL2 regardless of the
     * guest's PSTATE.I — it is gated only by EL2's PSTATE.I, which does not
     * apply once we are at EL1. So the timer PPI is still taken (via the
     * Lower-EL IRQ vector +0x480). Unmasking here instead opens a
     * one-instruction window in which the IRQ is taken AT EL2, where vectors.S
     * routes the current-EL IRQ entry to panic_vector. (See irq_handler_asm.S.)
     */
    eret

/* ------------------------------------------------------------------ */
/*  el1_sync_handler — Lower EL AArch64 synchronous exception         */
/* ------------------------------------------------------------------ */
    .globl el1_sync_handler
el1_sync_handler:
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

    /* handle_exit(struct vcpu_regs *regs, u64 esr) */
    mrs     x1, esr_el2
    bl      handle_exit

    /* Eret-back path: restore and return to guest (current vCPU via TPIDR_EL2) */
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
    eret

/* ------------------------------------------------------------------ */
/*  hv_restore() — restore hypervisor context; does not return        */
/* ------------------------------------------------------------------ */
    .globl hv_restore
hv_restore:
    hv_ctx_slot                        /* x9 = &g_hv_ctx[this pCPU's id] */
    ldr     x1,       [x9, #HV_SP]
    mov     sp, x1
    ldp     x19, x20, [x9, #0x00]
    ldp     x21, x22, [x9, #0x10]
    ldp     x23, x24, [x9, #0x20]
    ldp     x25, x26, [x9, #0x30]
    ldp     x27, x28, [x9, #0x40]
    ldr     x29,      [x9, #0x50]
    ldr     x30,      [x9, #HV_LR]
    ret
