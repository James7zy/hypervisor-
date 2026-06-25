/* SPDX-License-Identifier: TBD */
/*
 * secondary_main — per-CPU EL2 init for a PSCI-woken secondary core
 * (M3.5 Slice 2). Reached from secondary_entry (head.S) with SP_EL2,
 * VBAR_EL2 and TPIDR_EL2 already set for this core.
 *
 * This brings the secondary's EL2 to the same baseline the boot core set up
 * in gic_init()/vtimer_init(), but only for the per-CPU pieces: the GIC
 * redistributor and CPU interface, and the virtual-timer offset. The global
 * GICD config was already done once by CPU0.
 *
 * Slice 2 stops after publishing `online` and parks in WFI; Slice 3 makes it
 * enter the guest.
 */
#include <types.h>
#include <printk.h>
#include <board.h>
#include <percpu.h>
#include <vm.h>
#include <asm/sysreg.h>
#include <gic_v3.h>
#include "../irq/vgic.h"
#include "../mmu/stage2.h"

static inline void mmio_write32(unsigned long addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(unsigned long addr)
{
    return *(volatile u32 *)addr;
}

static inline void mmio_write8(unsigned long addr, u8 val)
{
    *(volatile u8 *)addr = val;
}

/*
 * This core's GICR RD_base. QEMU virt lays redistributors out contiguously,
 * 0x20000 (128 KiB) apart, starting at BOARD_GIC_RDIST_BASE. With static 1:1
 * pinning cpu id == redistributor index.
 */
#define GICR_STRIDE 0x20000UL

void secondary_main(u32 id)
{
    const unsigned long rd  = BOARD_GIC_RDIST_BASE + (unsigned long)id * GICR_STRIDE;
    const unsigned long sgi = rd + GICR_SGI_OFFSET;
    const u32 ppi  = BOARD_VTIMER_IRQ;   /* 27 */
    const u32 kick = BOARD_KICK_SGI;     /* cross-core kick SGI */

    /* 1. EL2 physical CPU interface for this core. */
    SYSREG_WRITE(ICC_SRE_EL2, 0xFULL);          /* SRE|DFB|DIB|Enable */
    asm volatile("isb");

    /* 2. Wake this core's redistributor: clear ProcessorSleep, wait
     *    ChildrenAsleep == 0. */
    mmio_write32(rd + GICR_WAKER,
                 mmio_read32(rd + GICR_WAKER) & ~GICR_WAKER_PROC_SLEEP);
    while (mmio_read32(rd + GICR_WAKER) & GICR_WAKER_CHILD_ASLEEP)
        ;

    /* 3. Enable the per-CPU INTIDs in this GICR's SGI/PPI frame: the vtimer
     *    PPI (27) and the cross-core kick-SGI. Group 1 NS, priority 0xA0. */
    mmio_write32(sgi + GICR_IGROUPR0,
                 mmio_read32(sgi + GICR_IGROUPR0) | (1U << ppi) | (1U << kick));
    mmio_write8(sgi + GICR_IPRIORITYR + ppi,  0xA0);
    mmio_write8(sgi + GICR_IPRIORITYR + kick, 0xA0);
    mmio_write32(sgi + GICR_ISENABLER0, (1U << ppi) | (1U << kick));

    /* 4. CPU interface enable: allow all priorities, Group 1, split EOI. */
    SYSREG_WRITE(ICC_PMR_EL1, 0xFFULL);
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1ULL);
    SYSREG_WRITE(ICC_CTLR_EL1,
                 SYSREG_READ(ICC_CTLR_EL1) | ICC_CTLR_EL1_EOIMODE);
    asm volatile("isb");

    /* 5. Virtual timer: same host timebase as CPU0 (CNTVOFF_EL2 = 0), let the
     *    guest read CNTPCT / use the physical timer regs without trapping. */
    SYSREG_WRITE(CNTVOFF_EL2, 0ULL);
    SYSREG_WRITE(CNTHCTL_EL2, 0x3ULL);
    asm volatile("isb");

    /* 6. Publish online. The barrier orders cpu_id before the online flag so
     *    CPU0's handshake sees a consistent slot. */
    percpu[id].cpu_id = id;
    asm volatile("dmb ish" ::: "memory");
    percpu[id].online = 1;
    asm volatile("dsb ish" ::: "memory");

    printk("[hv] pCPU%u online\n", (unsigned)id);

    /*
     * Slice 3 SMOKE TEST: enter guest (EL1) on this core to prove the per-CPU
     * guest-entry path works. vCPU1 shares vCPU0's Stage-2 table / VMID / HCR.
     * It is seeded with a tiny "wfe; b ." stub in guest RAM (TEMPORARY — Slice
     * 4 replaces this with the register state the guest's PSCI CPU_ON requests).
     */
    struct vcpu *v = &g_vm.vcpu[id];

    /* Share vCPU0's Stage-2 translation (same table, VMID, IPA space) and HCR. */
    v->vttbr_el2 = g_vm.vcpu[0].vttbr_el2;
    v->hcr_el2   = g_vm.vcpu[0].hcr_el2;
    vgic_init(v);   /* per-vCPU virtual interface: enabled, blank LRs/VMCR */

    /* Smoke stub: "wfe; b ." at a scratch page high in guest RAM, far past the
     * kernel Image / DTB / initrd. EL2 writes via the backing PA; the guest
     * enters at the corresponding IPA (PA - RAM_PA + RAM_IPA). */
    {
        const unsigned long stub_pa  = BOARD_LINUX_RAM_PA + 0x0F000000UL;
        const u64           stub_ipa = BOARD_LINUX_RAM_IPA + 0x0F000000UL;
        ((volatile u32 *)stub_pa)[0] = 0xD503205FU;   /* wfe       */
        ((volatile u32 *)stub_pa)[1] = 0x14000000U;   /* b .       */
        asm volatile("dsb ish; isb");

        v->regs.elr_el2  = stub_ipa;
        v->regs.spsr_el2 = 0x3C5ULL;   /* EL1h, DAIF masked (same as vCPU0) */
        v->regs.x[0] = 0;
        v->regs.x[1] = 0;
        v->regs.x[2] = 0;
        v->regs.x[3] = 0;
    }

    /* Virtual MPIDR for this vCPU: Aff0 = vCPU index (vCPU0->0, vCPU1->1). */
    SYSREG_WRITE(VMPIDR_EL2, (u64)id);
    asm volatile("isb");

    /* This core's current vCPU (asm entry path reads it via TPIDR_EL2). */
    percpu[id].cur_vcpu = v;

    /* Required order: Stage-2 activate BEFORE vGIC restore, then run (vcpu_run
     * loads HCR_EL2 from v->hcr_el2 and erets to EL1). */
    stage2_activate(v);
    vgic_restore(v);

    printk("[hv] pCPU%u entering guest (smoke stub)\n", (unsigned)id);
    for (;;)
        vcpu_run(v);
}
