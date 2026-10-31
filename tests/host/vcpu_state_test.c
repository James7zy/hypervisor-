/* SPDX-License-Identifier: TBD */
#include <vcpu_state.h>
#include "host_test.h"

/* Legal edges of the slice-A state machine (everything else must reject). */
static void check_edge(enum vcpu_state from, enum vcpu_event ev,
                       enum vcpu_state to)
{
    enum vcpu_state s = from;
    TEST_ASSERT(vcpu_state_step(&s, ev) == 0);
    TEST_ASSERT(s == to);
}

void test_vcpu_state(void)
{
    /* Every legal edge, exactly as pinned in the plan's transition table. */
    check_edge(VCPU_OFF, VCPU_EV_CPU_ON, VCPU_ON_PENDING);
    check_edge(VCPU_ON_PENDING, VCPU_EV_PICK, VCPU_RUNNABLE);
    check_edge(VCPU_RUNNABLE, VCPU_EV_LOAD, VCPU_RUNNING);
    check_edge(VCPU_RUNNING, VCPU_EV_BLOCK, VCPU_BLOCKED);
    check_edge(VCPU_BLOCKED, VCPU_EV_WAKE, VCPU_RUNNABLE);
    check_edge(VCPU_RUNNING, VCPU_EV_OFF, VCPU_OFF);
    /* M11 B1: cooperative WFI yield */
    check_edge(VCPU_RUNNING, VCPU_EV_YIELD, VCPU_RUNNABLE);
    check_edge(VCPU_RUNNABLE, VCPU_EV_OFF, VCPU_OFF);
    check_edge(VCPU_BLOCKED, VCPU_EV_OFF, VCPU_OFF);
    check_edge(VCPU_ON_PENDING, VCPU_EV_OFF, VCPU_OFF); /* SYSTEM_OFF sweep */

    /* The full matrix minus the legal edges above must reject and leave the
     * state untouched. */
    static const enum vcpu_state states[] = {
        VCPU_OFF, VCPU_ON_PENDING, VCPU_RUNNABLE, VCPU_RUNNING, VCPU_BLOCKED,
    };
    static const enum vcpu_event events[] = {
        VCPU_EV_CPU_ON, VCPU_EV_PICK, VCPU_EV_LOAD,
        VCPU_EV_BLOCK, VCPU_EV_WAKE, VCPU_EV_OFF, VCPU_EV_YIELD,
    };
    static const int legal[5][7] = {
        /*            CPU_ON PICK LOAD BLOCK WAKE OFF YIELD */
        /* OFF       */ { 1, 0, 0, 0, 0, 0, 0 },
        /* ON_PENDING*/ { 0, 1, 0, 0, 0, 1, 0 },
        /* RUNNABLE  */ { 0, 0, 1, 0, 0, 1, 0 },
        /* RUNNING   */ { 0, 0, 0, 1, 0, 1, 1 },
        /* BLOCKED   */ { 0, 0, 0, 0, 1, 1, 0 },
    };
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 7; j++) {
            if (legal[i][j])
                continue;
            enum vcpu_state s = states[i];
            TEST_ASSERT(vcpu_state_step(&s, events[j]) == -1);
            TEST_ASSERT(s == states[i]);   /* illegal: untouched */
        }
    }
}
