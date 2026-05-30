/* SPDX-License-Identifier: TBD */
/*
 * Cross-compiled struct offset check.
 * Compiled by the AArch64 cross-toolchain against the real vm.h, so
 * _Static_assert fires at 'make' time if the C struct layout diverges
 * from the assembly macros in vm.h's __ASSEMBLER__ block.
 *
 * Companion to tests/check_offsets.c (host cc; catches macro arithmetic).
 * This file catches field-insertion drift because it uses the live structs.
 * Run via: make check-offsets-target  (included in: make test)
 */
#include <stddef.h>
#include <vm.h>

/* Expected values — must mirror the #ifdef __ASSEMBLER__ block in vm.h */
#define VCPU_X0          0x000
#define VCPU_SP_EL1      0x0F8
#define VCPU_ELR         0x100
#define VCPU_SPSR        0x108
#define VCPU_HCR_EL2     0x110
#define VCPU_VTTBR_EL2   0x118
#define HV_LR            0x058
#define HV_SP            0x060
#define HV_CTX_SIZE      0x068

_Static_assert(offsetof(struct vcpu_regs, x[0])     == VCPU_X0,        "VCPU_X0 mismatch");
_Static_assert(offsetof(struct vcpu_regs, sp_el1)   == VCPU_SP_EL1,    "VCPU_SP_EL1 mismatch");
_Static_assert(offsetof(struct vcpu_regs, elr_el2)  == VCPU_ELR,       "VCPU_ELR mismatch");
_Static_assert(offsetof(struct vcpu_regs, spsr_el2) == VCPU_SPSR,      "VCPU_SPSR mismatch");
_Static_assert(offsetof(struct vcpu, hcr_el2)       == VCPU_HCR_EL2,   "VCPU_HCR_EL2 mismatch");
_Static_assert(offsetof(struct vcpu, vttbr_el2)     == VCPU_VTTBR_EL2, "VCPU_VTTBR_EL2 mismatch");
_Static_assert(offsetof(struct hv_ctx, lr)          == HV_LR,          "HV_LR mismatch");
_Static_assert(offsetof(struct hv_ctx, sp)          == HV_SP,          "HV_SP mismatch");
_Static_assert(sizeof(struct hv_ctx)                == HV_CTX_SIZE,    "HV_CTX_SIZE mismatch");
