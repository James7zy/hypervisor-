/* SPDX-License-Identifier: TBD */
#include <vcpu_state.h>

/*
 * Slice-A transition table: one switch per event, legal edges only (see
 * <vcpu_state.h>). Anything else is a caller bug: reject, don't guess.
 */
int vcpu_state_step(enum vcpu_state *s, enum vcpu_event e)
{
    switch (e) {
    case VCPU_EV_CPU_ON:
        if (*s == VCPU_OFF) {
            *s = VCPU_ON_PENDING;
            return 0;
        }
        break;
    case VCPU_EV_PICK:
        if (*s == VCPU_ON_PENDING) {
            *s = VCPU_RUNNABLE;
            return 0;
        }
        break;
    case VCPU_EV_LOAD:
        if (*s == VCPU_RUNNABLE) {
            *s = VCPU_RUNNING;
            return 0;
        }
        break;
    case VCPU_EV_BLOCK:
        if (*s == VCPU_RUNNING) {
            *s = VCPU_BLOCKED;
            return 0;
        }
        break;
    case VCPU_EV_WAKE:
        if (*s == VCPU_BLOCKED) {
            *s = VCPU_RUNNABLE;
            return 0;
        }
        break;
    case VCPU_EV_OFF:
        /* SYSTEM_OFF sweeps every non-OFF vCPU, ON_PENDING included. */
        if (*s != VCPU_OFF) {
            *s = VCPU_OFF;
            return 0;
        }
        break;
    }
    return -1;
}
