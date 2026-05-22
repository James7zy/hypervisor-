/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <asm/sysreg.h>

u64 read_currentel(void)
{
    return SYSREG_READ(CurrentEL);
}

void cpu_wfi(void)
{
    __asm__ volatile("wfi");
}

void cpu_relax(void)
{
    __asm__ volatile("yield");
}
