/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include <board.h>
#include <printk.h>
#include "vm_config.h"   /* struct vm_config (vmid / ram_pa) */
#include "stage2.h"

/*
 * VTCR_EL2: T0SZ=25 (39-bit IPA), SL0=1 (L1 start), IRGN0/ORGN0=1 (WB RA-WA),
 * SH0=3 (Inner Shareable), TG0=0 (4KB granule), PS=2 (40-bit PA).
 * Bit 31 is RES1 per ARM DDI0487 VTCR_EL2 definition.
 */
#define VTCR_RES1   (1ULL  << 31)
#define VTCR_T0SZ   (25ULL << 0)
#define VTCR_SL0    (1ULL  << 6)
#define VTCR_IRGN0  (1ULL  << 8)
#define VTCR_ORGN0  (1ULL  << 10)
#define VTCR_SH0    (3ULL  << 12)
#define VTCR_TG0    (0ULL  << 14)
#define VTCR_PS     (2ULL  << 16)
#define VTCR_EL2_VALUE \
    (VTCR_RES1 | VTCR_T0SZ | VTCR_SL0 | VTCR_IRGN0 | VTCR_ORGN0 | VTCR_SH0 | VTCR_TG0 | VTCR_PS)

/* Stage-2 Level-1 block descriptor fields */
#define S2_BLOCK        0x1ULL
#define S2_MEMATTR_DEV  (0x1ULL << 2)   /* Device-nGnRE: MemAttr[3:0]=0001 */
#define S2_MEMATTR_NORM (0xFULL << 2)   /* Normal WB inner+outer: MemAttr=1111 */
#define S2_S2AP_RW      (0x3ULL << 6)   /* R/W EL0+EL1 */
#define S2_SH_OSH       (0x2ULL << 8)   /* Outer Shareable */
#define S2_SH_ISH       (0x3ULL << 8)   /* Inner Shareable */
#define S2_AF           (1ULL   << 10)  /* Access Flag */
#define S2_XN           (1ULL   << 54)  /* Execute-never */
#define S2_TABLE        0x3ULL          /* Table descriptor: bits[1:0]=0b11 (vs block 0b01) */

/* Must be 4 KB-aligned: VTTBR_EL2[11:0] are reserved and must be zero.
 * One table per VM, keyed by vm->id. */
static u64 l1_table[NR_VMS][512] __attribute__((aligned(4096)));

/*
 * L2 table backing l1_table[vm][0] (IPA 0x00000000–0x3FFFFFFF). 512 × 2 MB
 * blocks, identity Device-nGnRE, EXCEPT the one entry covering GICD
 * (0x08000000) + GICR (0x080A0000): left invalid so guest accesses fault →
 * vgic_v3_mmio shadow.
 */
static u64 l2_dev[NR_VMS][512] __attribute__((aligned(4096)));

void stage2_init(struct vm *m)
{
    u64 *l1 = l1_table[m->id];
    u64 *l2 = l2_dev[m->id];
    u64  ram_pa = (u64)m->config->ram_pa;

    /*
     * IPA 0x00000000–0x3FFFFFFF: split the old 1 GB Device block into an L2
     * table so GICD/GICR (and now PL011) can be punched out. Each L2 entry is
     * 2 MB; fill all as identity Device-nGnRE, then invalidate the entries
     * holding GICD+GICR and PL011 → guest access faults → MMIO trap →
     * vgic_v3_mmio / vuart shadow emulation (ADR-0012; M5 slice 2 for UART).
     */
    for (u32 i = 0; i < 512U; i++)
        l2[i] = ((u64)i << 21) |
                S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;

    u32 gic_l2_idx = (u32)(BOARD_GIC_DIST_BASE >> 21);
    if ((u32)(BOARD_GIC_RDIST_BASE >> 21) != gic_l2_idx)
        printk("[hv] stage2: WARN GICD/GICR span >1 L2 entry "
               "(D=%u R=%u); punch-hole only covers D's entry\n",
               gic_l2_idx, (u32)(BOARD_GIC_RDIST_BASE >> 21));
    l2[gic_l2_idx] = 0;   /* invalid → fault */

    /*
     * PL011 (0x09000000, 0x09000000 >> 21 = 72): EL2 now owns the physical
     * UART exclusively (M5 slice 2). The guest's DR/FR/etc. accesses must
     * fault into the vuart trap-and-emulate model instead of reaching
     * hardware directly. guest/qemu_virt.dts declares only pl011@9000000 in
     * this 2 MB window (checked against the DTS before this change) -- no
     * other device shares it.
     */
    u32 uart_l2_idx = (u32)(BOARD_UART_BASE >> 21);
    l2[uart_l2_idx] = 0;   /* invalid → fault → MMIO trap → vuart */

    l1[0] = (u64)(uintptr_t)l2 | S2_TABLE;

    /*
     * IPA 0x40000000–0x7FFFFFFF → PA ram_pa: Normal WB (guest RAM).
     * Non-identity for the Linux guest: ram_pa is a dedicated region that
     * does not overlap the hv image at 0x40080000. An L1 block is 1 GB, so its
     * output address MUST be 1 GB-aligned (low 30 bits zero); ram_pa
     * (BOARD_LINUX_RAM_PA = 0x80000000) is. The mask is an assertion of that
     * invariant, not a rounding step — if ram_pa were not 1 GB-aligned the
     * masked-away low bits would silently mis-map the guest.
     */
    l1[1] = (ram_pa & 0xFFFFC0000000UL) |
            S2_BLOCK | S2_MEMATTR_NORM | S2_S2AP_RW | S2_SH_ISH | S2_AF;

    /* Every vCPU of the VM shares the same Stage-2 table / VMID. */
    u64 vttbr = ((u64)m->config->vmid << 48) | (u64)(uintptr_t)l1;
    for (u32 i = 0; i < VCPUS_PER_VM; i++)
        m->vcpu[i].vttbr_el2 = vttbr;
}

void stage2_activate(const struct vcpu *vcpu)
{
    /* dsb register 
     * Part D → Chapter D8 → D8.2 Translation process → D8.2.6 Translation table walk properties → 
     * “Ordering of memory accesses from translation table walks”
     * */
    asm volatile(
        "dsb ish\n"
        "msr vtcr_el2,  %0\n"
        "msr vttbr_el2, %1\n"
        "isb\n"
        :
        : "r"((u64)VTCR_EL2_VALUE), "r"(vcpu->vttbr_el2)
        : "memory"
    );
}

