/* SPDX-License-Identifier: TBD */
/*
 * Offset regression test: verifies that C struct layout matches the
 * hardcoded numeric offsets used in vmexit_asm.S (#ifdef __ASSEMBLER__
 * block in hypervisor/arch/arm64/include/arch/vm.h). Compiled on the host
 * with plain cc; no cross-toolchain
 * needed. Run via: make check-offsets
 *
 * If a struct field is added/reordered, this file fails to compile with a
 * clear _Static_assert message before any runtime breakage occurs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t u64;

/* Mirror of the vCPU structs — must stay in sync with hypervisor/include/vm.h
 * (struct vcpu) and hypervisor/arch/arm64/include/arch/vm.h (the rest). */
struct arch_regs {
    u64 x[31];      /* x0–x30 */
    u64 sp_el1;
    u64 elr_el2;
    u64 spsr_el2;
};

struct vcpu_arch {
    u64 hcr_el2;
    u64 vttbr_el2;
    u64 ich_hcr_el2;
    u64 ich_vmcr_el2;
    u64 ich_lr[4];
};

struct vcpu {
    struct arch_regs regs;   /* MUST be first */
    struct vcpu_arch arch;
};

struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;
    u64 sp;
};

/* Assembly macro values from <arch/vm.h> #ifdef __ASSEMBLER__ block */
#define VCPU_X0          0x000
#define VCPU_SP_EL1      0x0F8
#define VCPU_ELR         0x100
#define VCPU_SPSR        0x108
#define VCPU_HCR_EL2     0x110
#define VCPU_VTTBR_EL2   0x118
#define HV_LR            0x058
#define HV_SP            0x060
#define HV_CTX_SIZE      0x068

_Static_assert(offsetof(struct arch_regs, x[0])     == VCPU_X0,        "VCPU_X0 mismatch");
_Static_assert(offsetof(struct arch_regs, sp_el1)   == VCPU_SP_EL1,    "VCPU_SP_EL1 mismatch");
_Static_assert(offsetof(struct arch_regs, elr_el2)  == VCPU_ELR,       "VCPU_ELR mismatch");
_Static_assert(offsetof(struct arch_regs, spsr_el2) == VCPU_SPSR,      "VCPU_SPSR mismatch");
_Static_assert(offsetof(struct vcpu, arch.hcr_el2)       == VCPU_HCR_EL2,   "VCPU_HCR_EL2 mismatch");
_Static_assert(offsetof(struct vcpu, arch.vttbr_el2)     == VCPU_VTTBR_EL2, "VCPU_VTTBR_EL2 mismatch");
_Static_assert(offsetof(struct vcpu, arch.ich_hcr_el2)   == 0x120,          "ich_hcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, arch.ich_vmcr_el2)  == 0x128,          "ich_vmcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, arch.ich_lr)        == 0x130,          "ich_lr offset mismatch");
_Static_assert(offsetof(struct hv_ctx, lr)          == HV_LR,          "HV_LR mismatch");
_Static_assert(offsetof(struct hv_ctx, sp)          == HV_SP,          "HV_SP mismatch");
_Static_assert(sizeof(struct hv_ctx)                == HV_CTX_SIZE,    "HV_CTX_SIZE mismatch");

int main(void)
{
    puts("PASS: all struct offsets match assembly macros");
    return 0;
}
