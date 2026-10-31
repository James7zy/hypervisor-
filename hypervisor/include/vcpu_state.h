/* SPDX-License-Identifier: TBD */
#ifndef HV_VCPU_STATE_H
#define HV_VCPU_STATE_H

#include <types.h>

/*
 * Per-vCPU run state (M11 slice A; ADR-0015: generic, lives in struct vcpu).
 * PSCI-visible mapping: ON = RUNNABLE ∪ RUNNING ∪ BLOCKED (suspend is still
 * "on"); ON_PENDING is the window between CPU_ON authorization and the target
 * pCPU's first pick; SYSTEM_OFF sweeps every vCPU of the VM to OFF (incl.
 * ON_PENDING — gate-① 2026-10-31 adjudication).
 */
enum vcpu_state {
    VCPU_OFF = 0,       /* zero-init: vm_init authors all vCPUs to OFF */
    VCPU_ON_PENDING,    /* CPU_ON authorized (regs authored), not yet picked */
    VCPU_RUNNABLE,      /* pickable */
    VCPU_RUNNING,       /* loaded on a pCPU (or in its exit path) */
    VCPU_BLOCKED,       /* CPU_SUSPEND retention (in-place wake, slice A) */
};

enum vcpu_event {
    VCPU_EV_CPU_ON,     /* PSCI CPU_ON authorization (OFF → ON_PENDING) */
    VCPU_EV_PICK,       /* target pCPU picked the vCPU (ON_PENDING → RUNNABLE) */
    VCPU_EV_LOAD,       /* loaded onto virtualization hardware (→ RUNNING) */
    VCPU_EV_BLOCK,      /* CPU_SUSPEND entered (RUNNING → BLOCKED) */
    VCPU_EV_WAKE,       /* wake event while blocked (BLOCKED → RUNNABLE) */
    VCPU_EV_OFF,        /* CPU_OFF / SYSTEM_OFF sweep (→ OFF) */
    VCPU_EV_YIELD,      /* cooperative WFI yield (RUNNING → RUNNABLE, M11 B1;
                           B2's block/wake may absorb this edge — ADR-0018
                           records the pending re-ruling) */
};

/*
 * Apply event `e` to state `*s` in place. Returns 0 on a legal transition,
 * -1 (state untouched) on an illegal one. Pure logic: host-testable,
 * arch-neutral (types.h only).
 */
int vcpu_state_step(enum vcpu_state *s, enum vcpu_event e);

#endif /* HV_VCPU_STATE_H */
