/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_PERCPU_H
#define HV_ARCH_PERCPU_H

/* Included by <percpu.h> after struct percpu is defined; not on its own. */

/* TPIDR_EL2 holds &percpu[this pCPU] (set in head.S / vm_init). */
static inline struct percpu *cpu_arch_this_percpu(void)
{
    struct percpu *pc;
    __asm__ volatile("mrs %0, tpidr_el2" : "=r"(pc));
    return pc;
}

#endif /* HV_ARCH_PERCPU_H */
