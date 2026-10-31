/* SPDX-License-Identifier: TBD */
#include <sched_q.h>

/* Linear ring: enqueue at (head + count) % MAX, dequeue advances head.
 * Idempotency needs a membership scan — queues hold at most a handful of
 * entries, and the alternative (a per-vcpu queued flag) would give the
 * queue a vcpu-field dependency it must not have (host-testable opaque). */
void sched_q_init(struct sched_q *q)
{
    q->head  = 0;
    q->count = 0;
}

int sched_q_enqueue(struct sched_q *q, struct vcpu *v)
{
    for (u8 i = 0; i < q->count; i++) {
        if (q->slots[(u8)(q->head + i) % SCHED_Q_MAX] == v)
            return 0;                       /* already queued */
    }
    if (q->count >= SCHED_Q_MAX)
        return -1;
    q->slots[(u8)(q->head + q->count) % SCHED_Q_MAX] = v;
    q->count++;
    return 0;
}

struct vcpu *sched_q_dequeue(struct sched_q *q)
{
    if (q->count == 0)
        return NULL;
    struct vcpu *v = q->slots[q->head];
    q->head = (u8)(q->head + 1) % SCHED_Q_MAX;
    q->count--;
    return v;
}

bool sched_q_empty(const struct sched_q *q)
{
    return q->count == 0;
}
