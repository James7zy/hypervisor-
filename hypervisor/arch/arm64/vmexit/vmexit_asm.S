/* SPDX-License-Identifier: TBD */
/*
 * vmexit_asm.S — EL2↔EL1 context switch.
 *
 * vcpu_run(struct vcpu*)  — save HV callee-saved, load guest regs, eret to EL1
 * el1_sync_handler        — save guest regs, call handle_exit()
 * hv_restore()            — restore HV callee-saved, ret to vm_run()
 *
 * g_hv_ctx is defined here (BSS) — only accessed from this file.
 */

#include <vm.h>   /* VCPU_* and HV_* macros (active only in __ASSEMBLER__ block) */

    .section .bss
    .align 3
    .globl g_hv_ctx
g_hv_ctx:
    .zero HV_CTX_SIZE       /* 0x68 = 104 bytes */

/* ------------------------------------------------------------------ */
/*  vcpu_run(struct vcpu *vcpu)   x0 = vcpu*                          */
/* ------------------------------------------------------------------ */
    .section .text
    .globl vcpu_run
vcpu_run:
    /* 1. Save hypervisor callee-saved registers */
    adr_l   x9, g_hv_ctx
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

    eret

/* ------------------------------------------------------------------ */
/*  el1_sync_handler — Lower EL AArch64 synchronous exception         */
/* ------------------------------------------------------------------ */
    .globl el1_sync_handler
el1_sync_handler:
    /* Save x0 on EL2 stack to free it as pointer */
    str     x0, [sp, #-16]!

    /* Load &g_vm.vcpu.regs (first field of first field = same address as g_vm) */
    adr_l   x0, g_vm

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

    /* Eret-back path (future milestones): restore and return to guest */
    adr_l   x0, g_vm
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
    adr_l   x0, g_hv_ctx
    ldr     x1,       [x0, #HV_SP]
    mov     sp, x1
    ldp     x19, x20, [x0, #0x00]
    ldp     x21, x22, [x0, #0x10]
    ldp     x23, x24, [x0, #0x20]
    ldp     x25, x26, [x0, #0x30]
    ldp     x27, x28, [x0, #0x40]
    ldr     x29,      [x0, #0x50]
    ldr     x30,      [x0, #HV_LR]
    ret
