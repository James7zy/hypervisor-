/* SPDX-License-Identifier: TBD */
/*
 * Virtual timer setup for the guest.
 *
 * CNTHCTL_EL2.EL1PCTEN (bit 0) and EL1PCEN (bit 1) let the guest read
 * CNTPCT_EL0 and access the physical timer registers without trapping to EL2
 * as EC=0x18. CNTVOFF_EL2=0 gives the guest the host timebase (single VM, no
 * migration — see spec §5.3).
 */
#include <types.h>
#include <printk.h>
#include <arch/sysreg.h>
#include <vtimer.h>

void vtimer_init(void)
{
    /* CNTVOFF_EL2 is per-vCPU since M11 B2 (vcpu_arch load/resume program
     * it from the shadow); only the EL1 access trap control is global. */
    SYSREG_WRITE(CNTHCTL_EL2, 0x3ULL);   /* EL1PCTEN | EL1PCEN */
    asm volatile("isb");

    printk("[hv] vtimer: CNTHCTL_EL2=0x3 (CNTVOFF per-vCPU, B2)\n");
}
