/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_ARM64_EL2_MMU_H
#define HV_ARCH_ARM64_EL2_MMU_H

/*
 * EL2's own Stage-1 MMU (M11 prerequisite A, slice 1 — see
 * docs/superpowers/specs/2026-10-22-m11-pre-a-el2-hardening-design.md).
 * Identity-mapped: VA == PA for everything EL2 touches directly (its own
 * image, GICD/GICR, PL011). Guest RAM and Stage-2 state are NOT mapped here
 * — Stage-2 table walks use physical addresses directly and never go
 * through this translation.
 */

/* Build the shared identity-map L1 table. Boot CPU only, once, before any
 * CPU calls el2_mmu_enable(). */
void el2_mmu_init(void);

/* Program this PE's MAIR_EL2/TCR_EL2/TTBR0_EL2/SCTLR_EL2 and turn the MMU
 * on. Every pCPU (boot and PSCI-woken secondary) calls this once, after
 * el2_mmu_init() has built the shared table. */
void el2_mmu_enable(void);

#endif /* HV_ARCH_ARM64_EL2_MMU_H */
