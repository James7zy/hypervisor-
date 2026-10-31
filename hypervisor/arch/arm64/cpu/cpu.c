/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <arch/sysreg.h>
#include <arch/psci.h>
#include <cpu.h>
#include <printk.h>
#include <board_info.h>
#include <gic_v3.h>
#include <vtimer.h>
#include <vgic.h>               /* vgic_probe (M11 B2) */
#include "../mmu/el2_mmu.h"

/* secondary_entry (head.S): EL2 PA a secondary core is powered on at. */
extern char secondary_entry[];

void cpu_arch_init(void)
{
    /* EL2 Stage-1 MMU, identity map, before anything else touches memory
     * (M11 prerequisite A slice 1): a page-table mistake must show up here,
     * immediately, not get mixed up with later state. */
    el2_mmu_init();
    el2_mmu_enable();

    /*
     * MDCR_EL2: TDE(8) traps guest debug exceptions, TPM(6)/TPMCR(5) trap
     * guest PMU register access to EL2 (ADR-0017). No virtual debug or PMU
     * support yet: register accesses land in handle_sysreg_trap's default
     * (logged, read-as-zero, writes dropped); a real debug exception halts
     * with the unexpected-exit log line.
     */
    SYSREG_WRITE(MDCR_EL2, (1ULL << 8) | (1ULL << 6) | (1ULL << 5));

    printk("[hv] Hello from EL2 on %s, CurrentEL=0x%lx\n", board_name,
           SYSREG_READ(CurrentEL));

    gic_init();
    vgic_probe();   /* ICH_VTR: validate the LR-count contract (M11 B2 T6) */
    vtimer_init();
}

void cpu_arch_halt(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

/* Physical PSCI CPU_ON; ctx = pCPU id, and the target affinity equals the
 * pCPU id on QEMU virt GICv3 (<16 cores). */
s64 cpu_arch_power_on(u32 pcpu)
{
    return psci_cpu_on((u64)pcpu, (u64)(uintptr_t)secondary_entry, (u64)pcpu);
}

void cpu_arch_idle(void)
{
    /* Nap with IRQs ON (M11 B2): a pending physical IRQ — the blocked
     * owner's timer PPI, a kick — must actually be TAKEN so the current-EL
     * IRQ vector dispatches el2_irq_handler, whose inject path sched_wakes
     * the blocked vCPU. The pre-B2 masked wfi left the IRQ pending forever
     * (wfi completes, nothing consumes it, the loop busy-spins). The
     * unmasked window is exactly this function; the caller runs masked. */
    __asm__ volatile("msr daifclr, #2\n\twfi\n\tmsr daifset, #2"
                     ::: "memory");
}

void cpu_arch_kick(u32 pcpu)
{
    gic_kick_pcpu(pcpu);
}
