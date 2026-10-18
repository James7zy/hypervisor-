/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_PERCPU_H
#define HV_ARCH_PERCPU_H

/* Included by <percpu.h> after struct percpu is defined; not on its own. */

/* Stub: one global slot. A real port keeps &percpu[id] per pCPU (GS base). */
extern struct percpu *x86_this_percpu;

static inline struct percpu *cpu_arch_this_percpu(void)
{
    return x86_this_percpu;
}

static inline void cpu_arch_set_this_percpu(struct percpu *pc)
{
    x86_this_percpu = pc;
}

#endif /* HV_ARCH_PERCPU_H */
