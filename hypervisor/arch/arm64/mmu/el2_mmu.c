/* SPDX-License-Identifier: TBD */
/*
 * EL2 Stage-1 MMU, slice 1: identity map, MMU on, caches still off
 * (SCTLR_EL2.M=1, C=0, I=0). See
 * docs/superpowers/specs/2026-10-22-m11-pre-a-el2-hardening-design.md for
 * the full design and the register values' provenance (cross-checked
 * against bao-hypervisor and xvisor, two independent real-hardware-booting
 * AArch64 EL2 implementations).
 */
#include <types.h>

/*
 * MAIR_EL2: index 0 = Device-nGnRE, index 1 = Normal WB, Read-Allocate,
 * Write-Allocate (inner+outer).
 */
#define MAIR_ATTR_DEV_nGnRE   0x04ULL
#define MAIR_ATTR_NORMAL_WB   0xFFULL
#define MAIR_EL2_VALUE        (MAIR_ATTR_DEV_nGnRE | (MAIR_ATTR_NORMAL_WB << 8))

/*
 * TCR_EL2, non-VHE (HCR_EL2.E2H==0) format. RES1 at bit23 AND bit31 — this
 * is NOT the same RES1 as VTCR_EL2 (stage2.c), which only has bit31. T0SZ=25
 * matches stage2.c's VTCR_T0SZ (39-bit input address, same address-space
 * assumption throughout this codebase). PS=40-bit matches stage2.c's
 * VTCR_PS.
 */
#define TCR_RES1        ((1ULL << 23) | (1ULL << 31))
#define TCR_T0SZ        (25ULL << 0)
#define TCR_IRGN0_WBWA  (1ULL  << 8)
#define TCR_ORGN0_WBWA  (1ULL  << 10)
#define TCR_SH0_ISH     (3ULL  << 12)
#define TCR_TG0_4K      (0ULL  << 14)
#define TCR_PS_40BIT    (2ULL  << 16)
#define TCR_EL2_VALUE \
    (TCR_RES1 | TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_ISH | \
     TCR_TG0_4K | TCR_PS_40BIT)

/* SCTLR_EL2 bits this slice touches (slice 2 adds C and I). */
#define SCTLR_EL2_M     (1ULL << 0)

/*
 * Stage-1 VMSAv8-64 L1 block descriptor fields, EL2 translation regime
 * (single privilege level, no EL0 underneath). AP is a single bit at
 * bit[7]; leaving it unset means read-write (matches bao's
 * PTE_AP_RW_PRIV == 0). XN is a single bit at bit[54] (no separate
 * UXN/PXN split at this regime).
 */
#define S1_BLOCK        0x1ULL
#define S1_ATTRIDX(i)   ((u64)(i) << 2)
#define S1_AF           (1ULL << 10)
#define S1_SH_ISH       (3ULL << 8)
#define S1_SH_OSH       (2ULL << 8)
#define S1_XN           (1ULL << 54)

/* 512 x 1 GB L1 block entries. One shared table — all pCPUs use the same
 * identity map, so there is no per-CPU variant to build. */
static u64 l1_table[512] __attribute__((aligned(4096)));

void el2_mmu_init(void)
{
    /*
     * L1[0]: 0x00000000-0x3FFFFFFF, Device-nGnRE, non-executable, identity.
     * Covers BOARD_GIC_DIST_BASE (0x08000000), BOARD_GIC_RDIST_BASE
     * (0x080A0000) and BOARD_UART_BASE (0x09000000) — the only addresses in
     * this block EL2 ever touches directly.
     */
    l1_table[0] = 0x00000000ULL | S1_BLOCK | S1_ATTRIDX(0) | S1_AF |
                  S1_SH_OSH | S1_XN;

    /*
     * L1[1]: 0x40000000-0x7FFFFFFF, Normal WB, executable, identity. Covers
     * the hv image (loaded at 0x40080000) — nothing else lives in this 1 GB
     * block (board.h: guest RAM backing starts at BOARD_LINUX_RAM_PA =
     * 0x80000000, the next 1 GB block). No W^X in this slice (ADR-0017
     * defers permission tightening): the whole block is RW and executable.
     */
    l1_table[1] = 0x40000000ULL | S1_BLOCK | S1_ATTRIDX(1) | S1_AF |
                  S1_SH_ISH;

    /* Every other L1 entry (guest RAM, unused PA space) stays 0: invalid —
     * EL2 never addresses it through its own Stage-1 translation. */
}

void el2_mmu_enable(void)
{
    __asm__ volatile(
        "msr mair_el2, %0\n"
        "msr tcr_el2,  %1\n"
        "msr ttbr0_el2, %2\n"
        "isb\n"
        "tlbi alle2\n"
        "dsb nsh\n"
        "isb\n"
        :
        : "r"((u64)MAIR_EL2_VALUE), "r"((u64)TCR_EL2_VALUE),
          "r"((u64)(uintptr_t)l1_table)
        : "memory"
    );

    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    sctlr |= SCTLR_EL2_M;
    /*
     * Identity map: the next instruction (and everything after) sits at the
     * same VA it was already executing at, so enabling M here does not
     * require switching address spaces or branching anywhere — execution
     * just continues translated instead of untranslated.
     */
    __asm__ volatile(
        "msr sctlr_el2, %0\n"
        "isb\n"
        :: "r"(sctlr) : "memory"
    );
}
