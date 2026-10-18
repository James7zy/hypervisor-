/* SPDX-License-Identifier: TBD */
#ifndef HV_CPU_H
#define HV_CPU_H

#include <types.h>

/*
 * Physical-CPU hooks every arch implements (ADR-0015). No weak defaults: a
 * missing implementation fails the link.
 */

/* One-time arch bring-up on the boot pCPU, after the console is up
 * (privilege-level check, interrupt controller, timer). */
void cpu_arch_init(void);

/* Stop this pCPU for good (e.g. halt in a low-power wait loop). */
void cpu_arch_halt(void) __attribute__((noreturn));

/* Power on physical CPU `pcpu` at the hypervisor's secondary entry, which
 * then runs secondary_main(pcpu). Returns 0 on success, else an
 * arch-specific firmware status. */
s64 cpu_arch_power_on(u32 pcpu);

#endif /* HV_CPU_H */
