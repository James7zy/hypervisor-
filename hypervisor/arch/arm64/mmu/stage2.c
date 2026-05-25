/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include "stage2.h"

/*
 * VTCR_EL2: T0SZ=25 (39-bit IPA), SL0=1 (L1 start), IRGN0/ORGN0=1 (WB RA-WA),
 * SH0=3 (Inner Shareable), TG0=0 (4KB granule), PS=2 (40-bit PA).
 */
#define VTCR_T0SZ   (25ULL << 0)
#define VTCR_SL0    (1ULL  << 6)
#define VTCR_IRGN0  (1ULL  << 8)
#define VTCR_ORGN0  (1ULL  << 10)
#define VTCR_SH0    (3ULL  << 12)
#define VTCR_TG0    (0ULL  << 14)
#define VTCR_PS     (2ULL  << 16)
#define VTCR_EL2_VALUE \
    (VTCR_T0SZ | VTCR_SL0 | VTCR_IRGN0 | VTCR_ORGN0 | VTCR_SH0 | VTCR_TG0 | VTCR_PS)

/* Stage-2 Level-1 block descriptor fields */
#define S2_BLOCK        0x1ULL
#define S2_MEMATTR_DEV  (0x1ULL << 2)   /* Device-nGnRE: MemAttr[3:0]=0001 */
#define S2_MEMATTR_NORM (0xFULL << 2)   /* Normal WB inner+outer: MemAttr=1111 */
#define S2_S2AP_RW      (0x3ULL << 6)   /* R/W EL0+EL1 */
#define S2_SH_OSH       (0x2ULL << 8)   /* Outer Shareable */
#define S2_SH_ISH       (0x3ULL << 8)   /* Inner Shareable */
#define S2_AF           (1ULL   << 10)  /* Access Flag */
#define S2_XN           (1ULL   << 54)  /* Execute-never */

static u64 l1_table[512];   /* 4 KB, BSS */

void stage2_init(struct vcpu *vcpu, u32 vmid)
{
    /* IPA 0x00000000–0x3FFFFFFF → PA 0x00000000: Device (covers UART @ 0x09000000) */
    l1_table[0] = 0x00000000UL |
                  S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;

    /* IPA 0x40000000–0x7FFFFFFF → PA 0x40000000: Normal WB (covers all DRAM) */
    l1_table[1] = 0x40000000UL |
                  S2_BLOCK | S2_MEMATTR_NORM | S2_S2AP_RW | S2_SH_ISH | S2_AF;

    vcpu->vttbr_el2 = ((u64)vmid << 48) | (u64)(uintptr_t)l1_table;
}

void stage2_activate(const struct vcpu *vcpu)
{
    asm volatile(
        "msr vtcr_el2,  %0\n"
        "msr vttbr_el2, %1\n"
        "isb\n"
        :
        : "r"((u64)VTCR_EL2_VALUE), "r"(vcpu->vttbr_el2)
        : "memory"
    );
}
