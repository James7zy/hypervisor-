/* SPDX-License-Identifier: TBD */
#ifndef HV_SCHED_Q_H
#define HV_SCHED_Q_H

#include <types.h>

/*
 * Per-pCPU runqueue primitives (M11 slice B1). Pure logic, NO locks — the
 * cross-pCPU mutual exclusion lives in the sched.c glue layer (one spinlock
 * per queue); this module stays host-testable (types.h only, opaque vcpu).
 * Round-robin is dequeue + re-enqueue, composed by sched.c.
 */
#define SCHED_Q_MAX 8   /* per-pCPU capacity; sched.h binds it to the config */

struct vcpu;            /* opaque — the queue never dereferences it */

struct sched_q {
    struct vcpu *slots[SCHED_Q_MAX];
    u8           head;    /* next dequeue index */
    u8           count;
};

void          sched_q_init(struct sched_q *q);
/* Enqueue `v`; 0 if enqueued or ALREADY queued (idempotent — guards a yield
 * racing a wake), -1 if full. */
int           sched_q_enqueue(struct sched_q *q, struct vcpu *v);
/* Pop the head; NULL when empty. */
struct vcpu  *sched_q_dequeue(struct sched_q *q);
bool          sched_q_empty(const struct sched_q *q);

#endif /* HV_SCHED_Q_H */
