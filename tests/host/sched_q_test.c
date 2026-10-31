/* SPDX-License-Identifier: TBD */
#include <sched_q.h>
#include "host_test.h"

/* Opaque stand-ins: the queue must never dereference a vcpu. */
#define V(x) ((struct vcpu *)(uintptr_t)(x))

void test_sched_q(void)
{
    struct sched_q q;

    /* init: empty */
    sched_q_init(&q);
    TEST_ASSERT(sched_q_empty(&q));
    TEST_ASSERT(sched_q_dequeue(&q) == NULL);

    /* FIFO order over three entries */
    TEST_ASSERT(sched_q_enqueue(&q, V(0x10)) == 0);
    TEST_ASSERT(sched_q_enqueue(&q, V(0x20)) == 0);
    TEST_ASSERT(sched_q_enqueue(&q, V(0x30)) == 0);
    TEST_ASSERT(!sched_q_empty(&q));
    TEST_ASSERT(sched_q_dequeue(&q) == V(0x10));
    TEST_ASSERT(sched_q_dequeue(&q) == V(0x20));
    TEST_ASSERT(sched_q_dequeue(&q) == V(0x30));
    TEST_ASSERT(sched_q_empty(&q));

    /* Idempotent enqueue: re-queuing a queued vCPU is a no-op (0), and the
     * dequeue order of the others is untouched. */
    TEST_ASSERT(sched_q_enqueue(&q, V(0x10)) == 0);
    TEST_ASSERT(sched_q_enqueue(&q, V(0x20)) == 0);
    TEST_ASSERT(sched_q_enqueue(&q, V(0x10)) == 0);
    TEST_ASSERT(sched_q_dequeue(&q) == V(0x10));
    TEST_ASSERT(sched_q_dequeue(&q) == V(0x20));
    TEST_ASSERT(sched_q_empty(&q));

    /* Ring wrap: run past SCHED_Q_MAX enqueues/dequeues */
    sched_q_init(&q);
    for (int r = 0; r < 3; r++) {          /* three full wrap cycles */
        for (int i = 0; i < SCHED_Q_MAX; i++)
            TEST_ASSERT(sched_q_enqueue(&q, V(0x100 + i)) == 0);
        TEST_ASSERT(sched_q_enqueue(&q, V(0x999)) == -1);  /* full */
        for (int i = 0; i < SCHED_Q_MAX; i++)
            TEST_ASSERT(sched_q_dequeue(&q) == V(0x100 + i));
        TEST_ASSERT(sched_q_empty(&q));
    }
}
