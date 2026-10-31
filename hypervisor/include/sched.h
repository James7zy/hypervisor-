/* SPDX-License-Identifier: TBD */
#ifndef HV_SCHED_H
#define HV_SCHED_H

#include <types.h>
#include <sched_q.h> /* SCHED_Q_MAX for the capacity bind */
#include <vm.h>       /* NR_VMS/VCPUS_PER_VM */

/*
 * Scheduler glue (M11 slice B1). Per-pCPU round-robin over statically
 * affined vCPUs: sched_pick dequeues the head, sched_yield_cur re-enqueues
 * the current one — RR by composition. Mutual exclusion: one spinlock per
 * queue (CPU_ON enqueues cross-pCPU while the owner dequeues); both call
 * sites run with interrupts masked, satisfying spinlock.h's precondition.
 * The queue primitives (<sched_q.h>) stay lockless and host-testable.
 */
_Static_assert(NR_VMS * VCPUS_PER_VM <= SCHED_Q_MAX,
               "per-pCPU queue capacity must cover the config's vCPU count");

struct vcpu;

void          sched_init(void);             /* build + fill NR_CPUS queues */
struct vcpu  *sched_pick(u32 pcpu);         /* rotate out the next; NULL if idle */
void          sched_yield_cur(void);        /* re-enqueue the current vCPU */
void          sched_enqueue(struct vcpu *v);/* CPU_ON / wake (idempotent) */
void          sched_wake(struct vcpu *v);  /* BLOCKED only: WAKE + enqueue + kick */
bool          sched_queue_empty(u32 pcpu); /* locked probe (WFI "alone?" leg) */
/* Lock-free advisory probe — ONLY inside the unmasked idle window (see
 * sched.c: the locked probe there self-deadlocks on hardware). */
bool          sched_idle_may_sleep(u32 pcpu);
/* The scheduler run loop both pCPUs drop into after their bring-up (M11 B2
 * hoist; noreturn). */
void          sched_loop(u32 pcpu) __attribute__((noreturn));

#endif /* HV_SCHED_H */
