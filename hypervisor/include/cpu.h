/* SPDX-License-Identifier: TBD */
#ifndef HV_CPU_H
#define HV_CPU_H

#include <types.h>

/*
 * Physical-CPU hooks every arch implements (ADR-0015). No weak defaults: a
 * missing implementation fails the link.
 */

/* Stop this pCPU for good (arm64: wfi forever). */
void cpu_arch_halt(void) __attribute__((noreturn));

/* Power on physical CPU `pcpu` at the hypervisor's secondary entry, which
 * then runs secondary_main(pcpu). Returns 0 on success, else an
 * arch-specific firmware status (arm64: PSCI return code). */
s64 cpu_arch_power_on(u32 pcpu);

#endif /* HV_CPU_H */
