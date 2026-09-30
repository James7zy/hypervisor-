/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_VM_H
#define HV_ARCH_VM_H

/*
 * arm64 part of the vCPU (ADR-0015). The generic struct vcpu (<vm.h>) embeds
 * struct arch_regs first and struct vcpu_arch second, so the asm-visible
 * fields keep fixed offsets from the start of struct vcpu. The C layout and
 * the asm offset macros live together here (ADR-0003, as amended by
 * ADR-0015) and are pinned by tests/check_offsets*.c.
 */

#ifndef __ASSEMBLER__
#include <types.h>
#include <spinlock.h>

struct arch_regs {
    u64 x[31];      /* x0–x30   offset 0x000 */
    u64 sp_el1;     /*           offset 0x0F8 */
    u64 elr_el2;    /*           offset 0x100 */
    u64 spsr_el2;   /*           offset 0x108 */
};

struct vcpu_arch {
    u64 hcr_el2;             /* vcpu offset 0x110 */
    u64 vttbr_el2;           /* vcpu offset 0x118 */
    u64 ich_hcr_el2;         /* vcpu offset 0x120 */
    u64 ich_vmcr_el2;        /* vcpu offset 0x128 */
    u64 ich_lr[4];           /* vcpu offset 0x130 (LR0..LR3, 0x130..0x14F) */
    /* Fields below are NOT read by the exception-entry asm — append only. */
    /* M5 slice 3 fix: set by remote vgic_inject_spi() before kicking the
     * owning pCPU, test-and-cleared by vgic_reload_spi_lr() on that pCPU.
     * Distinguishes "this kick-SGI carries a freshly-shadowed PL011 SPI" from
     * "this kick-SGI is an ordinary cross-core IPI/park-check with nothing
     * new in ich_lr[1]" -- without it, vgic_reload_spi_lr() would blindly
     * replay a stale shadow (already consumed by the guest) back into the
     * live ICH_LR1_EL2 on every unrelated kick. Mirrors sgi_pending[]'s role
     * for the SGI/IPI case (vgic_sgi.c) but is per-vcpu, separate state --
     * does not interact with sgi_pending[]. */
    bool spi_shadow_pending;
    /* Serializes LR1 payload/pending/live completion; never reset at runtime. */
    struct spinlock spi_lock;
};

/* Hypervisor callee-saved context, saved by vcpu_arch_run and restored by
 * hv_restore (vmexit_asm.S), one slot per pCPU. */
struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;   /* offset 0x058 */
    u64 sp;   /* offset 0x060 */
};

extern void hv_restore(void);   /* vmexit_asm.S; does not return */
#endif /* !__ASSEMBLER__ */

#ifdef __ASSEMBLER__
/* Offsets from the start of struct vcpu (arch_regs is its first member). */
#define VCPU_X0         0x000
#define VCPU_SP_EL1     0x0F8
#define VCPU_ELR        0x100
#define VCPU_SPSR       0x108
#define VCPU_HCR_EL2    0x110
#define VCPU_VTTBR_EL2  0x118
#define HV_LR           0x058
#define HV_SP           0x060
#define HV_CTX_SIZE     0x068
#endif /* __ASSEMBLER__ */

#endif /* HV_ARCH_VM_H */
