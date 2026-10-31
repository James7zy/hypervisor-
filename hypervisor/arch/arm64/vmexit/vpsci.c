/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vm_config.h>   /* struct vm_config (vcpu_pcpu[]) */
#include <percpu.h>
#include <arch/psci.h>
#include "vpsci.h"
#include <gic_v3.h>
#include <cpu.h>
#include <sched.h>

/*
 * Guest-driven CPU_ON (M3.5 Slice 4; per-vCPU semantics since M11 slice A).
 * The guest's Linux issues PSCI CPU_ON to bring up its vCPU1; that HVC traps
 * here. We map the requested affinity to a vCPU index, author that vCPU's
 * boot state from the guest's args, publish ON_PENDING, and KICK the target
 * pCPU — which is already physically online (the hypervisor brought up every
 * pCPU at boot) and either running this VM's other vCPU or idling in its
 * authorization wait. Returns SUCCESS immediately (KVM precedent: CPU_ON
 * promises the target will run, not that it already does; the caller's own
 * secondary-holding protocol does the synchronizing).
 */
static u64 psci_cpu_on_guest(struct arch_regs *regs)
{
    struct vm *m = current_vcpu()->owner;

    u64 target_aff = regs->x[1];   /* MPIDR affinity of the target vCPU      */
    u64 entry      = regs->x[2];   /* guest IPA entry point                  */
    u64 ctx_id     = regs->x[3];   /* opaque context id, returned in x0      */

    /* VMPIDR_EL2 gives vCPU N affinity Aff0=N within its VM. */
    u64 aff = target_aff & 0xFFULL;   /* Aff0 */
    if ((target_aff & ~0xFFULL) != 0ULL || aff >= (u64)VCPUS_PER_VM)
        return PSCI_RET_INVALID_PARAMETERS;

    u32 idx  = (u32)aff;                    /* VM-local vCPU index */
    struct vcpu *v = &m->vcpu[idx];
    if (v->state != VCPU_OFF)
        return PSCI_RET_ALREADY_ON;         /* percpu[].online is now the
                                               physical fact, not this check */

    /*
     * Author the secondary's arm64 boot state (Documentation/arm64/booting.rst
     * secondary path): EL1h, DAIF masked, x0 = context_id (NOT the DTB), PC =
     * entry. CPU_ON gives the target a warm-reset register file — every other
     * GPR and sp are zero, nothing may leak from the CPU_OFF that parked it.
     * The live per-pCPU EL1 CPU state (SCTLR_EL1, TLBs) is reset on the
     * TARGET pCPU by vcpu_arch_load; from here (the caller's pCPU) we can
     * only author the saved register file. Shares vCPU0's Stage-2 / VMID /
     * HCR (Stage-2 vttbr was already set on every vCPU by stage2_init).
     */
    v->regs.elr_el2  = entry;
    v->regs.spsr_el2 = 0x3C5ULL;                 /* EL1h, DAIF masked         */
    v->launched      = false;  /* next entry is a fresh boot, not a resume */
    v->regs.sp_el1   = 0;   /* undefined at secondary entry; Linux sets its own */
    v->regs.x[0]     = ctx_id;
    for (u32 i = 1; i < 31U; i++)
        v->regs.x[i] = 0;
    v->arch.hcr_el2  = m->vcpu[0].arch.hcr_el2;  /* mirror vCPU0              */

    /* Publish the register file BEFORE the state: a target that wakes through
     * any path other than the kick (spurious pending IRQ completing wfi) must
     * never observe ON_PENDING with the authoring still in flight — the
     * trailing dsb only orders against the kick. The receiver's acquire load
     * completes the pair (gate-2 Important 1). */
    __asm__ volatile("dmb ish" ::: "memory");
    (void)vcpu_state_step(&v->state, VCPU_EV_CPU_ON);
    /* Queue AFTER the state publish: the owner loop drops (dequeues and
     * discards) a picked vCPU that is not yet ON_PENDING/RUNNABLE, so
     * enqueued-while-OFF would strand the target. After: the owner either
     * sees the queue empty (idles until our kick) or picks it enterable. */
    sched_enqueue(v);
    u32 pcpu = m->config->vcpu_pcpu[idx];
    printk("[hv] PSCI: CPU_ON vCPU%u -> ON_PENDING (pCPU%u)\n",
           (unsigned)idx, (unsigned)pcpu);

    /* State first, then barrier, then kick: the target's pick loop must be
     * able to observe ON_PENDING when the kick interrupt lands (same shape
     * as the old VM-level off-flag protocol). */
    __asm__ volatile("dsb ish" ::: "memory");
    cpu_arch_kick(pcpu);
    return PSCI_RET_SUCCESS;
}

/*
 * CPU_OFF (M11 slice A): stops ONLY this vCPU. The pCPU unwinds to its run
 * loop (hv_restore), which sees OFF and waits — a sibling CPU_ON can bring
 * this vCPU back (Linux cpu hotplug off/on cycles rely on that).
 */
static void psci_cpu_off_self(void)
{
    struct vcpu *v = current_vcpu();

    printk("[hv] PSCI: VM%u CPU_OFF - stopping vCPU%u (pCPU%u)\n",
           (unsigned)v->owner->id, (unsigned)v->vcpu_idx,
           (unsigned)current_vcpu_id());

    (void)vcpu_state_step(&v->state, VCPU_EV_OFF);
    __asm__ volatile("dsb ish" ::: "memory");
    hv_restore();   /* unwind to this pCPU's run loop; never erets back */
}

/*
 * SYSTEM_OFF / SYSTEM_RESET(2): the WHOLE VM stops. Every vCPU (ON_PENDING
 * included) goes OFF; only the siblings that were RUNNING in their guest get
 * kicked — a kick to a pCPU parked at EL2 (idle/OFF-wait, IRQs masked at EL2
 * C) pends forever and makes its wfi return immediately: unbounded busy-spin
 * (gate-1 R5). Parked siblings observe OFF that way only via their own exit
 * paths, which never come; that is correct: they are already stopped.
 */
static void psci_system_off_vm(const char *what)
{
    struct vm *m = current_vcpu()->owner;
    u32 caller_pcpu = current_vcpu_id();

    printk("[hv] PSCI: VM%u %s - stopping all vCPUs\n",
           (unsigned)m->id, what);

    for (u32 idx = 0; idx < (u32)VCPUS_PER_VM; idx++) {
        struct vcpu *v = &m->vcpu[idx];
        bool was_running = (v->state == VCPU_RUNNING);

        if (v->state == VCPU_OFF)
            continue;
        (void)vcpu_state_step(&v->state, VCPU_EV_OFF);
        if (was_running) {
            u32 pcpu = m->config->vcpu_pcpu[idx];
            if (pcpu == caller_pcpu)
                continue;
            __asm__ volatile("dsb ish" ::: "memory");
            cpu_arch_kick(pcpu);   /* in-guest sibling: parks in el2_irq_handler */
        }
    }

    hv_restore();   /* own pCPU unwinds; run loop sees OFF and idles forever */
}

/*
 * CPU_SUSPEND (M11 pre-B shape, slice-A state machine). Retention: sleep at
 * EL2 until a wake interrupt, then return SUCCESS with the caller's context
 * intact (power_down entry point / context id accepted, ignored — ruling
 * 9.3). BLOCK before wfi and WAKE+LOAD after: the wake must land on RUNNING,
 * because this path resumes the guest via eret WITHOUT passing the run loop
 * — a RUNNABLE leak here makes the next timer exit re-run vgic_init and wipe
 * the live guest's GIC state (gate-1 R3). No physical reload happens in that
 * LOAD: the vCPU never left this pCPU.
 */
static u64 psci_cpu_suspend(void)
{
    struct vcpu *v = current_vcpu();

    (void)vcpu_state_step(&v->state, VCPU_EV_BLOCK);
    __asm__ volatile("wfi");
    (void)vcpu_state_step(&v->state, VCPU_EV_WAKE);
    (void)vcpu_state_step(&v->state, VCPU_EV_LOAD);
    return PSCI_RET_SUCCESS;
}

/*
 * AFFINITY_INFO (M11 slice A): reads the per-vCPU run state. Level must be
 * 0; the target affinity maps exactly like CPU_ON (Aff0 = VM-local vCPU
 * index, higher affinity fields must be zero). ON = pending ∪ runnable ∪
 * running ∪ blocked (suspend is still on); ON_PENDING is the window between
 * CPU_ON authorization and the target pCPU's first pick.
 */
static u64 psci_affinity_info(struct arch_regs *regs)
{
    struct vm *m = current_vcpu()->owner;
    u64 target_aff = regs->x[1];
    u32 level      = (u32)regs->x[2];

    if (level != 0U)
        return PSCI_RET_INVALID_PARAMETERS;

    u64 aff = target_aff & 0xFFULL;   /* Aff0 */
    if ((target_aff & ~0xFFULL) != 0ULL || aff >= (u64)VCPUS_PER_VM)
        return PSCI_RET_INVALID_PARAMETERS;

    switch (m->vcpu[(u32)aff].state) {
    case VCPU_ON_PENDING:
        return PSCI_AFF_LEVEL_ON_PENDING;
    case VCPU_RUNNABLE:
    case VCPU_RUNNING:
    case VCPU_BLOCKED:
        return PSCI_AFF_LEVEL_ON;
    case VCPU_OFF:
    default:
        return PSCI_AFF_LEVEL_OFF;
    }
}

/* PSCI_FEATURES: SUCCESS for the calls we implement, else NOT_SUPPORTED. */
static u64 psci_features(u32 fn)
{
    switch (fn) {
    case PSCI_VERSION:
    case PSCI_CPU_OFF:
    case PSCI_CPU_SUSPEND_32:
    case PSCI_CPU_SUSPEND_64:
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64:
    case PSCI_MIGRATE_INFO_TYPE:
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
    case PSCI_SYSTEM_RESET2_32:
    case PSCI_SYSTEM_RESET2_64:
    case PSCI_FEATURES:
        return PSCI_RET_SUCCESS;
    default:
        return PSCI_RET_NOT_SUPPORTED;
    }
}

void psci_handle(struct arch_regs *regs)
{
    u32 fn = (u32)regs->x[0];

    switch (fn) {
    case PSCI_VERSION:
        regs->x[0] = PSCI_VERSION_1_1;
        break;
    case PSCI_FEATURES:
        regs->x[0] = psci_features((u32)regs->x[1]);
        break;
    case PSCI_CPU_SUSPEND_32:
    case PSCI_CPU_SUSPEND_64:
        regs->x[0] = psci_cpu_suspend();
        break;
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64:
        regs->x[0] = psci_affinity_info(regs);
        break;
    case PSCI_MIGRATE_INFO_TYPE:
        /* No trusted OS, MP-safe (DEN0022 5.7; kernel PSCI_0_2_TOS_MP). */
        regs->x[0] = PSCI_TOS_NOT_PRESENT_MP;
        break;
    case PSCI_CPU_OFF:
        psci_cpu_off_self();                /* no return */
        break;
    case PSCI_SYSTEM_OFF:
        psci_system_off_vm("SYSTEM_OFF");   /* no return */
        break;
    case PSCI_SYSTEM_RESET:
        psci_system_off_vm("SYSTEM_RESET"); /* no return */
        break;
    case PSCI_SYSTEM_RESET2_32:
    case PSCI_SYSTEM_RESET2_64:
        /* reset_type: 0 = SYSTEM_WARM_RESET, 1 = SYSTEM_COLD_RESET
         * (DEN0022 5.13); anything else is INVALID_PARAMETERS. */
        if (regs->x[1] > 1ULL) {
            regs->x[0] = PSCI_RET_INVALID_PARAMETERS;
            break;
        }
        psci_system_off_vm("SYSTEM_RESET2"); /* no return */
        break;
    case PSCI_CPU_ON_32:
    case PSCI_CPU_ON_64:
        regs->x[0] = psci_cpu_on_guest(regs);
        break;
    default:
        printk("[hv] PSCI: unknown fn=0x%x\n", (unsigned)fn);
        regs->x[0] = PSCI_RET_NOT_SUPPORTED;
        break;
    }
}
