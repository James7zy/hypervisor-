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
#include <cpu.h>
#include <sched.h>
#include <arch/sysreg.h>
#include <gic_v3.h>
#include <vgic.h>
#include "../mmu/el2_mmu.h"

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
    /* EL2 Stage-1 MMU: the shared identity-map table was already built by
     * the boot CPU's el2_mmu_init(); this core only programs its own
     * per-PE MAIR/TCR/TTBR0/SCTLR before touching any MMIO. */
    el2_mmu_enable();

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
    /* CNTVOFF_EL2: per-vCPU since M11 B2 — the scheduler loop's
     * load/resume programs it; nothing global to do here. */
    SYSREG_WRITE(CNTHCTL_EL2, 0x3ULL);
    asm volatile("isb");

    /*
     * M11 slice A run loop. The pCPU's vCPU comes from the affinity table
     * (vcpu_for_pcpu; NULL = idle pCPU). cur_vcpu is set BEFORE the
     * authorization wait: defense in depth for any path that reads
     * current_vcpu() from EL2 C on this core (the kick branch is reachable
     * from a Lower-EL exit and, since M11 B2, from the idle window's
     * current-EL vector — the seeding below is what holds in both).
     */
    struct vcpu *v = vcpu_for_pcpu(id);
    percpu[id].cur_vcpu = v;

    /* Publish online AFTER this core's EL2 per-CPU init is complete. */
    __asm__ volatile("dmb ish" ::: "memory");
    percpu[id].cpu_id = id;
    percpu[id].online = 1;
    __asm__ volatile("dsb ish" ::: "memory");

    printk("[hv] pCPU%u online, entering guest\n", (unsigned)id);
    /* ^ String kept byte-identical: tests/run_vspi_test.sh greps it to poll
     * boot completion (gate-1 R2). For vcpu[1]-affined pCPUs it now prints
     * at hypervisor boot rather than at the guest's PSCI CPU_ON. With B1's
     * dynamic queues the idle case (no affined vCPU ever) is just an empty
     * pick — the loop below parks naturally. */

    /* The scheduler loop (M11 B2 hoist) — see common/sched/sched.c. */
    sched_loop(id);
}
