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
#include <arch/sysreg.h>
#include <gic_v3.h>
#include <vgic.h>
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

    /*
     * Enter guest (EL1) on this core. This vCPU's register state (elr/x0/spsr)
     * was authored by the guest-driven PSCI CPU_ON handler (psci_cpu_on_guest)
     * before this pCPU was powered on. Its Stage-2 vttbr was authored earlier
     * still and independently of PSCI: stage2_init() (called once per VM from
     * vm_init(), at hypervisor boot) fills in vttbr_el2 for every vCPU slot of
     * the VM up front, since all vCPUs of a VM share one Stage-2 table/VMID.
     * HCR is the one PSCI CPU_ON actually mirrors, copying vcpu[0]'s hcr_el2
     * into this vCPU's slot. We only set up the per-vCPU virtual GIC interface
     * here.
     */
    struct vm   *m = &vm[id / VCPUS_PER_VM];
    struct vcpu *v = &m->vcpu[id % VCPUS_PER_VM];
    vgic_init(v);   /* per-vCPU virtual interface: enabled, blank LRs/VMCR */

    /* Virtual MPIDR for this vCPU: Aff0 = VM-local vCPU index. */
    SYSREG_WRITE(VMPIDR_EL2, (u64)v->vcpu_idx);
    asm volatile("isb");

    /* This core's current vCPU (asm entry path reads it via TPIDR_EL2). */
    percpu[id].cur_vcpu = v;

    /* Publish online AFTER the vCPU is fully prepared: the PSCI handler waits
     * on this before returning SUCCESS to the guest. */
    asm volatile("dmb ish" ::: "memory");
    percpu[id].cpu_id = id;
    percpu[id].online = 1;
    asm volatile("dsb ish" ::: "memory");

    printk("[hv] pCPU%u online, entering guest\n", (unsigned)id);

    /* Required order: Stage-2 activate BEFORE vGIC restore, then run (vcpu_run
     * loads HCR_EL2 from v->hcr_el2 and erets to EL1). */
    stage2_activate(v);
    vgic_restore(v);

    for (;;)
        vcpu_run(v);
}
