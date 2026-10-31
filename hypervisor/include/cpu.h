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

/* One wfi, then return — the caller owns the interrupt window (M11 B2):
 * the scheduler loop unmasks (cpu_arch_irq_enable), re-checks its queue,
 * sleeps here, and remasks. Portability: RISC-V wfi; x86 hlt (IF already
 * enabled by the caller's window). */
void cpu_arch_idle(void);
/* EL2 IRQ window controls (M11 B2): the idle path's unmask/mask pair.
 * Callers must hold NO locks across the enabled window. */
void cpu_arch_irq_enable(void);
void cpu_arch_irq_disable(void);

/* Ask pCPU `pcpu` to re-check its runnable work (asynchronous; the target
 * observes it as an interrupt and re-runs its pick loop). Portability:
 * x86 = IPI, RISC-V = software-interrupt IPI. */
void cpu_arch_kick(u32 pcpu);

#endif /* HV_CPU_H */
