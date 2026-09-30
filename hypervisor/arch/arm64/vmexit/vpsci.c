/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vm_config.h>   /* struct vm_config (pcpu_base) */
#include <percpu.h>
#include <arch/psci.h>
#include "vpsci.h"
#include <gic_v3.h>

/* secondary_entry (head.S): EL2 PA the secondary core is powered on at. */
extern char secondary_entry[];

/*
 * Guest-driven CPU_ON (M3.5 Slice 4). The guest's Linux issues PSCI CPU_ON to
 * bring up its vCPU1; that HVC traps here. We map the requested affinity to a
 * vCPU index, author that vCPU's boot state from the guest's args, power on the
 * matching pCPU at secondary_entry, and hand back SUCCESS once it is online.
 */
static u64 psci_cpu_on_guest(struct vcpu_regs *regs)
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
    u32 pcpu = m->config->pcpu_base + idx;  /* physical target     */
    if (percpu[pcpu].online)
        return PSCI_RET_ALREADY_ON;

    /*
     * Author the secondary's arm64 boot state (Documentation/arm64/booting.rst
     * secondary path): EL1h, DAIF masked, x0 = context_id (NOT the DTB), PC =
     * entry. Shares vCPU0's Stage-2 / VMID / HCR (Stage-2 vttbr was already
     * set on every vCPU by stage2_init).
     */
    struct vcpu *v = &m->vcpu[idx];
    v->regs.elr_el2  = entry;
    v->regs.x[0]     = ctx_id;
    v->regs.x[1]     = 0;
    v->regs.x[2]     = 0;
    v->regs.x[3]     = 0;
    v->regs.spsr_el2 = 0x3C5ULL;                 /* EL1h, DAIF masked         */
    v->hcr_el2       = m->vcpu[0].hcr_el2;       /* mirror vCPU0              */

    /* Power on the matching pCPU at our EL2 secondary_entry, ctx = pCPU id.
     * Target the physical affinity (== pcpu for QEMU virt GICv3, <16 cores). */
    s64 ret = psci_cpu_on((u64)pcpu, (u64)(uintptr_t)secondary_entry, (u64)pcpu);
    if (ret != (s64)PSCI_RET_SUCCESS)
        return PSCI_RET_INVALID_PARAMETERS;

    /* Wait (bounded) for the secondary to publish online before returning
     * SUCCESS, so the guest's CPU_ON contract (CPU running on return) holds. */
    for (u64 i = 0; i < 100000000ULL; i++) {
        if (percpu[pcpu].online)
            return PSCI_RET_SUCCESS;
        asm volatile("dmb ish" ::: "memory");
    }
    return PSCI_RET_INTERNAL_FAILURE;
}

/*
 * Force every ONLINE pCPU of VM `m` other than `caller_pcpu` into EL2 via the
 * physical kick SGI. Carries no virtual interrupt and touches no vGIC state:
 * the kick exists purely so each target re-enters EL2 and observes `m->off`
 * (el2_irq_handler's kick-SGI branch), then parks instead of re-entering its
 * guest. That makes this PSCI policy, not interrupt virtualization, which is
 * why it lives here rather than in the vGIC.
 *
 * Callers must set m->off and order it (dsb ish) BEFORE calling.
 */
static void psci_kick_vm_other_pcpus(struct vm *m, u32 caller_pcpu)
{
    for (u32 idx = 0; idx < (u32)VCPUS_PER_VM; idx++) {
        u32 pcpu = m->config->pcpu_base + idx;
        if (pcpu == caller_pcpu)
            continue;
        if (percpu[pcpu].online)
            gic_kick_pcpu(pcpu);
    }
}

/*
 * VM-scoped power-down (M5 slice 3). A guest's CPU_OFF/SYSTEM_OFF/
 * SYSTEM_RESET must stop only ITS OWN VM: mark the VM `off`, kick its other
 * pCPU(s) into EL2 so they notice `off` (el2_irq_handler's kick-SGI branch)
 * and park instead of re-entering their guest, then park the calling pCPU
 * itself. Other VMs' pCPUs are untouched and keep running.
 */
static void psci_power_down(const char *what)
{
    struct vm *m = current_vcpu()->owner;
    u32 caller_pcpu = current_vcpu_id();

    printk("[hv] PSCI: VM%u %s - halting vCPU%u (pCPU%u)\n",
           (unsigned)m->id, what, (unsigned)current_vcpu()->vcpu_idx,
           (unsigned)caller_pcpu);

    m->off = 1;
    asm volatile("dsb ish" ::: "memory");
    psci_kick_vm_other_pcpus(m, caller_pcpu);

    for (;;)
        asm volatile("wfi");
}

/* PSCI_FEATURES: SUCCESS for the calls we implement, else NOT_SUPPORTED. */
static u64 psci_features(u32 fn)
{
    switch (fn) {
    case PSCI_VERSION:
    case PSCI_CPU_OFF:
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
    case PSCI_FEATURES:
        return PSCI_RET_SUCCESS;
    default:
        return PSCI_RET_NOT_SUPPORTED;
    }
}

void psci_handle(struct vcpu_regs *regs)
{
    u32 fn = (u32)regs->x[0];

    switch (fn) {
    case PSCI_VERSION:
        regs->x[0] = PSCI_VERSION_1_1;
        break;
    case PSCI_FEATURES:
        regs->x[0] = psci_features((u32)regs->x[1]);
        break;
    case PSCI_CPU_OFF:
        psci_power_down("CPU_OFF");        /* no return */
        break;
    case PSCI_SYSTEM_OFF:
        psci_power_down("SYSTEM_OFF");      /* no return */
        break;
    case PSCI_SYSTEM_RESET:
        psci_power_down("SYSTEM_RESET");    /* no return */
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
