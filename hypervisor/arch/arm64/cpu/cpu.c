/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <arch/sysreg.h>
#include <arch/psci.h>
#include <cpu.h>
#include <printk.h>
#include <board_info.h>
#include <gic_v3.h>
#include <vtimer.h>

/* secondary_entry (head.S): EL2 PA a secondary core is powered on at. */
extern char secondary_entry[];

static u64 read_currentel(void)
{
    return SYSREG_READ(CurrentEL);
}

void cpu_arch_init(void)
{
    u64 el = read_currentel();
    printk("[hv] Hello from EL2 on %s, CurrentEL=0x%lx\n", board_name, el);

    gic_init();
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
