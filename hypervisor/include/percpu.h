/* SPDX-License-Identifier: TBD */
#ifndef HV_PERCPU_H
#define HV_PERCPU_H

/*
 * Per-CPU foundation (M3.5 Slice 1).
 *
 * TPIDR_EL2 is the single source of truth for "which vCPU is current on this
 * physical core". Each pCPU writes &percpu[id] into TPIDR_EL2; the exception
 * -entry asm reads the current vCPU through it (mrs tpidr_el2 ; ldr [#offset])
 * instead of taking the address of a single global VM. This is the
 * controlled offset coupling that supersedes the "first field of first field"
 * trick (see ADR-0003); the _Static_assert below makes a drift a build error.
 *
 * NR_CPUS is a compile-time constant: the M3.5 build assumes physical =
 * virtual = 2, statically 1:1 pinned, no scheduler.
 */

#define NR_CPUS 2

/*
 * Byte offset of cur_vcpu within struct percpu, consumed by the asm entry
 * paths as `ldr xN, [tpidr_el2, #PERCPU_CUR_VCPU]`. cur_vcpu MUST be the first
 * field; the _Static_assert below pins this to 0.
 */
#define PERCPU_CUR_VCPU 0

/*
 * sizeof(struct percpu), used by secondary_entry asm to index percpu[id]
 * (base + id * PERCPU_SIZE). Pinned by a _Static_assert below.
 */
#define PERCPU_SIZE 16

#ifndef __ASSEMBLER__
#include <types.h>

struct vcpu;   /* forward decl — avoids a vm.h<->percpu.h include cycle */

struct percpu {
    struct vcpu *cur_vcpu;   /* MUST be first — asm reads at PERCPU_CUR_VCPU */
    u32          cpu_id;
    volatile u32 online;
};

_Static_assert(__builtin_offsetof(struct percpu, cur_vcpu) == PERCPU_CUR_VCPU,
               "asm reads cur_vcpu at PERCPU_CUR_VCPU; keep it the first field");
_Static_assert(sizeof(struct percpu) == PERCPU_SIZE,
               "secondary_entry asm indexes percpu[id] using PERCPU_SIZE");

extern struct percpu percpu[NR_CPUS];

/* Read TPIDR_EL2 → current pCPU's percpu slot → its current vCPU. */
static inline struct vcpu *current_vcpu(void)
{
    struct percpu *pc;
    __asm__ volatile("mrs %0, tpidr_el2" : "=r"(pc));
    return pc->cur_vcpu;
}

/* This pCPU's id (== vCPU index under static 1:1 pinning). */
static inline u32 current_vcpu_id(void)
{
    struct percpu *pc;
    __asm__ volatile("mrs %0, tpidr_el2" : "=r"(pc));
    return pc->cpu_id;
}
#endif /* !__ASSEMBLER__ */

#endif /* HV_PERCPU_H */
