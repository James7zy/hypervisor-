/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <psci.h>

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
    u64 target_aff = regs->x[1];   /* MPIDR affinity of the target vCPU      */
    u64 entry      = regs->x[2];   /* guest IPA entry point                  */
    u64 ctx_id     = regs->x[3];   /* opaque context id, returned in x0      */

    /* VMPIDR_EL2 gives vCPU N affinity Aff0=N; only {0,1} exist (NR_CPUS=2). */
    u64 aff = target_aff & 0xFFULL;   /* Aff0 */
    if ((target_aff & ~0xFFULL) != 0ULL || aff >= (u64)NR_CPUS)
        return PSCI_RET_INVALID_PARAMETERS;

    u32 idx = (u32)aff;
    if (percpu[idx].online)
        return PSCI_RET_ALREADY_ON;

    /*
     * Author the secondary's arm64 boot state (Documentation/arm64/booting.rst
     * secondary path): EL1h, DAIF masked, x0 = context_id (NOT the DTB), PC =
     * entry. Shares vCPU0's Stage-2 / VMID / HCR (set up in vm_init).
     */
    struct vcpu *v = &g_vm.vcpu[idx];
    v->regs.elr_el2  = entry;
    v->regs.x[0]     = ctx_id;
    v->regs.x[1]     = 0;
    v->regs.x[2]     = 0;
    v->regs.x[3]     = 0;
    v->regs.spsr_el2 = 0x3C5ULL;                 /* EL1h, DAIF masked         */
    v->hcr_el2       = g_vm.vcpu[0].hcr_el2;      /* mirror vCPU0              */
    v->vttbr_el2     = g_vm.vcpu[0].vttbr_el2;    /* same Stage-2 table/VMID   */

    /* Power on the matching pCPU at our EL2 secondary_entry, ctx = vCPU index.
     * Target the physical affinity (== index for QEMU virt GICv3, <16 cores). */
    s64 ret = psci_cpu_on(aff, (u64)(uintptr_t)secondary_entry, (u64)idx);
    if (ret != (s64)PSCI_RET_SUCCESS)
        return PSCI_RET_INVALID_PARAMETERS;

    /* Wait (bounded) for the secondary to publish online before returning
     * SUCCESS, so the guest's CPU_ON contract (CPU running on return) holds. */
    for (u64 i = 0; i < 100000000ULL; i++) {
        if (percpu[idx].online)
            return PSCI_RET_SUCCESS;
        asm volatile("dmb ish" ::: "memory");
    }
    return PSCI_RET_INTERNAL_FAILURE;
}

/* Single-vCPU build: a power-down request just halts this CPU forever. */
static void psci_power_down(const char *what)
{
    printk("[hv] PSCI: %s - halting vCPU\n", what);
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

s64 psci_cpu_on(u64 target_mpidr, u64 entry, u64 ctx_id)
{
    register u64 x0 __asm__("x0") = PSCI_CPU_ON_64;
    register u64 x1 __asm__("x1") = target_mpidr;
    register u64 x2 __asm__("x2") = entry;
    register u64 x3 __asm__("x3") = ctx_id;

    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (s64)x0;
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
