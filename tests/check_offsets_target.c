/* SPDX-License-Identifier: TBD */
/*
 * Cross-compiled struct offset check.
 * Compiled by the AArch64 cross-toolchain against the real vm.h, so
 * _Static_assert fires at 'make' time if the C struct layout diverges
 * from the assembly macros in <arch/vm.h>'s __ASSEMBLER__ block.
 *
 * Companion to tests/check_offsets.c (host cc; catches macro arithmetic).
 * This file catches field-insertion drift because it uses the live structs.
 * Run via: make check-offsets-target  (included in: make test)
 */
#include <stddef.h>
#include <vm.h>

/* Expected values — must mirror the #ifdef __ASSEMBLER__ block in <arch/vm.h> */
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
/* Generic-tail field (M11 slice A): NOT read by exception-entry asm, but
 * pinned here so a field insertion that pushes it (or a migration of it into
 * the asm-visible prefix) fails the build instead of silently drifting. */
_Static_assert(offsetof(struct vcpu, state)              == 0x1E4,          "vcpu.state offset mismatch");
_Static_assert(offsetof(struct vcpu, launched)            == 0x1E8,          "vcpu.launched offset mismatch");
/* EL1 bulk set (M11 B2 T4): generic-tail adjacency, whole-block pin. */
_Static_assert(offsetof(struct vcpu, arch.el1)             == 0x178,          "el1 block offset mismatch");
_Static_assert(sizeof(((struct vcpu *)0)->arch.el1)        == 0x60,           "el1 block size mismatch");
/* Per-vCPU vtimer shadow (M11 B2) — generic-tail adjacency of vcpu_arch. */
_Static_assert(offsetof(struct vcpu, arch.cntvoff)         == 0x160,          "cntvoff offset mismatch");
_Static_assert(offsetof(struct vcpu, arch.cntv_ctl)        == 0x168,          "cntv_ctl offset mismatch");
_Static_assert(offsetof(struct vcpu, arch.cntv_cval)       == 0x170,          "cntv_cval offset mismatch");
_Static_assert(offsetof(struct hv_ctx, lr)          == HV_LR,          "HV_LR mismatch");
_Static_assert(offsetof(struct hv_ctx, sp)          == HV_SP,          "HV_SP mismatch");
_Static_assert(sizeof(struct hv_ctx)                == HV_CTX_SIZE,    "HV_CTX_SIZE mismatch");
