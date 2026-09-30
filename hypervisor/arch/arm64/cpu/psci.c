/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <arch/psci.h>

s64 psci_cpu_on(u64 target_mpidr, u64 entry, u64 ctx_id)
{
    register u64 x0 __asm__("x0") = PSCI_CPU_ON_64;
    register u64 x1 __asm__("x1") = target_mpidr;
    register u64 x2 __asm__("x2") = entry;
    register u64 x3 __asm__("x3") = ctx_id;

    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (s64)x0;
}
