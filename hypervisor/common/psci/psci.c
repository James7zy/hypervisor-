/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <psci.h>

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
        printk("[hv] PSCI: CPU_ON not supported (M3)\n");
        regs->x[0] = PSCI_RET_NOT_SUPPORTED;
        break;
    default:
        printk("[hv] PSCI: unknown fn=0x%x\n", (unsigned)fn);
        regs->x[0] = PSCI_RET_NOT_SUPPORTED;
        break;
    }
}
