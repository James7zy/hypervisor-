/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <hypercall.h>
#include <psci.h>
#include <vgic.h>
#include "mmio.h"
#include "../irq/vgic_sgi.h"

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
        vgic_inject_sw(&vm[0].vcpu[0], (u32)regs->x[1], 0xA0);
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

/*
 * Trapped GIC CPU-interface sysreg (EC=0x18). QEMU's ICH_HCR_EL2.TC=1 traps a
 * broader set than the architectural SGI generation registers: it also traps
 * the IRQ/FIQ priority group ICC_PMR_EL1, ICC_CTLR_EL1, ICC_RPR_EL1 (QEMU
 * gicv3_irqfiq_access). We need TC to trap ICC_SGI1R_EL1 for cross-core IPI
 * relay, so we must also faithfully EMULATE the other trapped registers — a
 * guest whose PMR/CTLR writes are dropped will mis-mask its interrupts and
 * wedge (this was the M3.5 SMP secondary hang).
 *
 * The guest's accesses target its VIRTUAL CPU interface; at EL2 those are the
 * ICV_* registers. We forward the trapped read/write to the matching ICV_*
 * register so the guest sees correct behaviour.
 *
 * ISS layout: Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10] Rt[9:5] CRm[4:1]
 * Direction[0] (0 = write).
 */
static void handle_sysreg_trap(struct vcpu_regs *regs, u64 esr)
{
    u32 iss = (u32)(esr & 0x1FFFFFFU);
    u32 op2 = (iss >> 17) & 0x7U;
    u32 crn = (iss >> 10) & 0xFU;
    u32 crm = (iss >>  1) & 0xFU;
    u32 rt  = (iss >>  5) & 0x1FU;
    bool wr = (iss & 0x1U) == 0U;       /* direction bit 0: 0 = write */
    u64 enc = ((u64)crn << 8) | ((u64)crm << 4) | op2;   /* op0=3,op1=0 implied */

    /* ICC_SGI*R_EL1: relay as an IPI (write only). */
    if (crn == 12U && crm == 11U && (op2 == 5U || op2 == 6U || op2 == 7U)) {
        if (wr)
            vgic_sgi_trap((rt == 31U) ? 0ULL : regs->x[rt]);
        regs->elr_el2 += 4ULL;
        return;
    }

    /*
     * Other TC-trapped CPU-interface regs. The guest's VIRTUAL interface state
     * lives in ICH_VMCR_EL2 (VPMR[31:24], VBPR/VEOIM/VCBPR/VENG fields). The HW
     * normally updates these automatically when the guest accesses ICC_* with
     * SRE; TC forces a trap, so we replicate that update against ICH_VMCR_EL2.
     * We must NOT touch the physical ICC_* (that is EL2's own interface and
     * would break the hypervisor's interrupt handling).
     */
    u64 vmcr;
    __asm__ volatile("mrs %0, S3_4_C12_C11_7" : "=r"(vmcr));   /* ICH_VMCR_EL2 */
    u64 val = (!wr) ? 0ULL : ((rt == 31U) ? 0ULL : regs->x[rt]);

    switch (enc) {
    case (4U << 8) | (6U << 4) | 0U:    /* ICC_PMR_EL1 <-> VMCR.VPMR[31:24] */
        if (wr) {
            vmcr = (vmcr & ~(0xFFULL << 24)) | ((val & 0xFFULL) << 24);
            __asm__ volatile("msr S3_4_C12_C11_7, %0" :: "r"(vmcr));
            __asm__ volatile("isb");
        } else {
            val = (vmcr >> 24) & 0xFFULL;
        }
        break;
    case (12U << 8) | (12U << 4) | 4U:  /* ICC_CTLR_EL1: EOImode<->VMCR.VEOIM,
                                         * CBPR<->VMCR.VCBPR; other bits RAZ/WI */
        if (wr) {
            vmcr &= ~((1ULL << 9) | (1ULL << 4));               /* VEOIM | VCBPR */
            if (val & (1ULL << 1)) vmcr |= (1ULL << 9);         /* CTLR.EOImode */
            if (val & (1ULL << 0)) vmcr |= (1ULL << 4);         /* CTLR.CBPR    */
            __asm__ volatile("msr S3_4_C12_C11_7, %0" :: "r"(vmcr));
            __asm__ volatile("isb");
        } else {
            val = 0;
            if (vmcr & (1ULL << 9)) val |= (1ULL << 1);
            if (vmcr & (1ULL << 4)) val |= (1ULL << 0);
        }
        break;
    case (12U << 8) | (11U << 4) | 3U:  /* ICC_RPR_EL1 (RO): running priority */
        if (!wr)
            val = 0;   /* idle priority; good enough for the guest's checks */
        break;
    default:
        printk("[hv] EC=0x18 unhandled sysreg cpu%u C%u_C%u_%u %s\n",
               (unsigned)current_vcpu_id(), (unsigned)crn, (unsigned)crm,
               (unsigned)op2, wr ? "wr" : "rd");
        break;
    }
    if (!wr && rt != 31U)
        regs->x[rt] = val;
    regs->elr_el2 += 4ULL;
}

void handle_exit(struct vcpu_regs *regs, u64 esr)
{
    u32 ec = (u32)(esr >> 26) & 0x3FU;

    switch (ec) {
    case 0x16:   /* HVC from AArch64 EL1 */
        handle_hvc(regs);
        return;
    case 0x18:
        /* Trapped MSR/MRS (EC=0x18). See handle_sysreg_trap. */
        handle_sysreg_trap(regs, esr);
        return;
    case 0x24:   /* Data Abort from lower EL → MMIO trap-and-emulate */
        if (mmio_handle_data_abort(regs, esr) == 0)
            return;   /* handled: ELR advanced; el1_sync_handler erets back */
        /* fall through to the diagnostic + park on no handler / ISV=0 */
        /* fallthrough */
    default:
        printk("[hv] unexpected exit cpu%u EC=0x%x ESR=0x%lx ELR=0x%lx\n",
               (unsigned)current_vcpu_id(), (unsigned)ec, esr, regs->elr_el2);
        for (;;)
            asm volatile("wfi");
    }
}
