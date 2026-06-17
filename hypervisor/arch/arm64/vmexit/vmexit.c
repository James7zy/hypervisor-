/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <hypercall.h>
#include <psci.h>
#include <vgic.h>
#include "mmio.h"

/* Defined in vmexit_asm.S; does not return */
extern void hv_restore(void);

static void handle_hvc(struct vcpu_regs *regs)
{
    u32 func_id = (u32)regs->x[0];
    u8  svc     = (u8)(func_id >> 24);

    if (svc == 0x84 || svc == 0xC4) {   /* 32-bit or 64-bit PSCI */
        psci_handle(regs);
        return;
    }

    switch (func_id) {
    case HC_INJECT_TEST:
        vgic_inject_sw(&g_vm.vcpu, (u32)regs->x[1], 0xA0);
        printk("[hv] SVM HVC: inject vINTID=%u\n", (unsigned)regs->x[1]);
        break;
    case HC_GUEST_DONE:
        printk("[hv] SVM HVC: done (x1=0x%lx)\n", regs->x[1]);
        hv_restore();   /* no return */
        break;
    default:
        printk("[hv] HVC: unknown func_id=0x%x\n", (unsigned)func_id);
        regs->x[0] = SMCCC_NOT_SUPPORTED;
        /* ELR_EL2 already points past the HVC instruction */
        break;
    }
}

void handle_exit(struct vcpu_regs *regs, u64 esr)
{
    u32 ec = (u32)(esr >> 26) & 0x3FU;

    switch (ec) {
    case 0x16:   /* HVC from AArch64 EL1 */
        handle_hvc(regs);
        return;
    case 0x24:   /* Data Abort from lower EL → MMIO trap-and-emulate */
        if (mmio_handle_data_abort(regs, esr) == 0)
            return;   /* handled: ELR advanced; el1_sync_handler erets back */
        /* fall through to the diagnostic + park on no handler / ISV=0 */
        /* fallthrough */
    default:
        printk("[hv] unexpected exit EC=0x%x ESR=0x%lx ELR=0x%lx\n",
               (unsigned)ec, esr, regs->elr_el2);
        for (;;)
            asm volatile("wfi");
    }
}
