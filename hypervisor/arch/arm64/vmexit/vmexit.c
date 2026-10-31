/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <cpu.h>
#include <sched.h>
#include <arch/hypercall.h>
#include "vpsci.h"
#include "data_abort.h"
#include <vgic_sgi.h>

/*
 * ESR_EL2 Exception Class values (ARM DDI0487, ESR_ELx.EC field; names and
 * values follow Linux arch/arm64/include/esr.h for easy cross-reference).
 */
#define EC_WFX       0x01U   /* Trapped WFI/WFE (HCR_EL2.TWI/TWE) */
#define EC_HVC64     0x16U   /* HVC instruction execution, AArch64 */
#define EC_SMC64     0x17U   /* SMC instruction execution, AArch64 (TSC) */
#define EC_SYS64     0x18U   /* Trapped MSR/MRS */
#define EC_DABT_LOW  0x24U   /* Data Abort from a lower EL */

static void handle_hvc(struct arch_regs *regs)
{
    u32 func_id = (u32)regs->x[0];
    u8  svc     = (u8)(func_id >> 24);

    /*
     * SMCCC base probes (M11 pre-B), before the PSCI dispatch: the kernel's
     * smccc probe queries these over the same hvc conduit. ARCH_FEATURES
     * answers SUCCESS only for itself and SMCCC_VERSION -- no TRNG, no
     * ARCH_SOC_ID, everything else NOT_SUPPORTED (DEN 0028 A).
     */
    if (func_id == SMCCC_VERSION) {
        regs->x[0] = SMCCC_VERSION_1_1;
        return;
    }
    if (func_id == SMCCC_ARCH_FEATURES) {
        u32 query = (u32)regs->x[1];
        regs->x[0] = (query == SMCCC_VERSION || query == SMCCC_ARCH_FEATURES)
                     ? 0ULL : SMCCC_NOT_SUPPORTED;
        return;
    }

    if (svc == 0x84 || svc == 0xC4) {   /* 32-bit or 64-bit PSCI */
        psci_handle(regs);
        return;
    }

    switch (func_id) {
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

/* EC=0x01, trapped wfi/wfe (HCR_EL2.TWI/TWE). WFE passes through; WFI is
 * the M11-B1 cooperative yield point (see the body). Unlike HVC/SMC, a
 * trapped instruction exception reports ELR AT the instruction, so
 * advancing past it is required -- without it the guest re-executes the
 * same wfi forever. */
static void handle_wfx(struct arch_regs *regs, u64 esr)
{
    /* WFE (bit 0 of the trapped instruction's ISS): keep passing through —
     * events are self-contained in the guest. WFI: cooperative yield (M11
     * B1): advance ELR, mark the vCPU RUNNABLE, requeue, and unwind to this
     * pCPU's scheduler loop via hv_restore (never eret back into the wfi —
     * the loop re-enters this vCPU or switches). No vcpu_arch_put here:
     * put happens only on a REAL switch-away in the loop (gate-1 R1). */
    if ((esr & 1ULL) == 0ULL) {                 /* WFI */
        struct vcpu *v = current_vcpu();
        regs->elr_el2 += 4ULL;
        if (vcpu_state_step(&v->state, VCPU_EV_YIELD) == 0)
            sched_yield_cur();
        /* step failure = swept OFF mid-flight: no requeue, just unwind. */
        hv_restore();
    }
    regs->elr_el2 += 4ULL;                      /* WFE: pass through */
}

/* EC=0x17, trapped smc (HCR_EL2.TSC, ADR-0017): never let a guest smc reach
 * EL3 firmware; answer SMCCC_NOT_SUPPORTED. ELR_EL2 already points past the
 * SMC instruction, same convention as HVC. Reachable only on hardware with
 * EL3 (RK3588); QEMU forces TSC RES0 without EL3. */
static void handle_smc(struct arch_regs *regs)
{
    printk("[hv] SMC trapped (func_id=0x%lx), returning NOT_SUPPORTED\n",
           regs->x[0]);
    regs->x[0] = SMCCC_NOT_SUPPORTED;
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
static void handle_sysreg_trap(struct arch_regs *regs, u64 esr)
{
    u32 iss = (u32)(esr & 0x1FFFFFFU);
    u32 op2 = (iss >> 17) & 0x7U;
    u32 op1 = (iss >> 14) & 0x7U;
    u32 crn = (iss >> 10) & 0xFU;
    u32 crm = (iss >>  1) & 0xFU;
    u32 rt  = (iss >>  5) & 0x1FU;
    bool wr = (iss & 0x1U) == 0U;       /* direction bit 0: 0 = write */
    u64 enc = ((u64)crn << 8) | ((u64)crm << 4) | op2;   /* op0=3 implied */

    /*
     * TID3 ID group (op0=3, op1=0, CRn=0): read the physical register and
     * forward the real value, so the guest sees exactly what it saw before
     * the trap existed. One exception: MPIDR_EL1 must return the VIRTUAL
     * cpu id -- an untrapped EL1 read returns VMPIDR_EL2 (set by
     * vcpu_arch_load), and forwarding the physical MPIDR would break VM1's
     * SMP boot (its pCPUs are Aff0=2/3, its DTB says 0/1). TID2's
     * CSSELR/CCSIDR/CLIDR/CTR_EL0 are not trapped (TID2 clear) and never
     * reach this branch.
     */
    if (crn == 0U && op1 == 0U) {
        if (wr) {
            printk("[hv] TID3: write to ID reg C0_C%u_%u dropped\n",
                   (unsigned)crm, (unsigned)op2);
        } else {
            u64 v = 0;
            /*
             * Encodings from Linux arch/arm64/tools/sysreg (same kernel we
             * boot): AA64PFR=CRm4, AA64DFR/AFR=CRm5, AA64ISAR=CRm6,
             * AA64MMFR=CRm7; CRm1-3 hold the AArch32 ID_* set and MVFR*.
             * Raw S-encodings throughout: the toolchain's binutils does not
             * know every name. Unimplemented registers read-as-zero, which
             * is what a cpu without the feature reports anyway.
             */
            switch (enc) {
            #define FWD_ID(reg, e) case e: \
                __asm__ volatile("mrs %0, " #reg : "=r"(v)); break;
            /* CRm=0 */
            FWD_ID(midr_el1,           (0U << 4) | 0U)
            FWD_ID(vmpidr_el2,         (0U << 4) | 5U)  /* MPIDR_EL1: virtual */
            FWD_ID(revidr_el1,         (0U << 4) | 6U)
            /* CRm=1: ID_PFR0/1, ID_DFR0, ID_AFR0, ID_MMFR0-3 */
            FWD_ID(S3_0_C0_C1_0,       (1U << 4) | 0U)
            FWD_ID(S3_0_C0_C1_1,       (1U << 4) | 1U)
            FWD_ID(S3_0_C0_C1_2,       (1U << 4) | 2U)
            FWD_ID(S3_0_C0_C1_3,       (1U << 4) | 3U)
            FWD_ID(S3_0_C0_C1_4,       (1U << 4) | 4U)
            FWD_ID(S3_0_C0_C1_5,       (1U << 4) | 5U)
            FWD_ID(S3_0_C0_C1_6,       (1U << 4) | 6U)
            FWD_ID(S3_0_C0_C1_7,       (1U << 4) | 7U)
            /* CRm=2: ID_ISAR0-5, ID_MMFR4, ID_ISAR6 */
            FWD_ID(S3_0_C0_C2_0,       (2U << 4) | 0U)
            FWD_ID(S3_0_C0_C2_1,       (2U << 4) | 1U)
            FWD_ID(S3_0_C0_C2_2,       (2U << 4) | 2U)
            FWD_ID(S3_0_C0_C2_3,       (2U << 4) | 3U)
            FWD_ID(S3_0_C0_C2_4,       (2U << 4) | 4U)
            FWD_ID(S3_0_C0_C2_5,       (2U << 4) | 5U)
            FWD_ID(S3_0_C0_C2_6,       (2U << 4) | 6U)
            FWD_ID(S3_0_C0_C2_7,       (2U << 4) | 7U)
            /* CRm=3: MVFR0-2, ID_PFR2, ID_DFR1, ID_MMFR5 */
            FWD_ID(S3_0_C0_C3_0,       (3U << 4) | 0U)
            FWD_ID(S3_0_C0_C3_1,       (3U << 4) | 1U)
            FWD_ID(S3_0_C0_C3_2,       (3U << 4) | 2U)
            FWD_ID(S3_0_C0_C3_4,       (3U << 4) | 4U)
            FWD_ID(S3_0_C0_C3_5,       (3U << 4) | 5U)
            FWD_ID(S3_0_C0_C3_6,       (3U << 4) | 6U)
            /* CRm=4: ID_AA64PFR0/1/2, ZFR0, SMFR0, FPFR0 */
            FWD_ID(S3_0_C0_C4_0,       (4U << 4) | 0U)
            FWD_ID(S3_0_C0_C4_1,       (4U << 4) | 1U)
            FWD_ID(S3_0_C0_C4_2,       (4U << 4) | 2U)
            FWD_ID(S3_0_C0_C4_4,       (4U << 4) | 4U)
            FWD_ID(S3_0_C0_C4_5,       (4U << 4) | 5U)
            FWD_ID(S3_0_C0_C4_7,       (4U << 4) | 7U)
            /* CRm=5: ID_AA64DFR0/1, ID_AA64AFR0/1 */
            FWD_ID(S3_0_C0_C5_0,       (5U << 4) | 0U)
            FWD_ID(S3_0_C0_C5_1,       (5U << 4) | 1U)
            FWD_ID(S3_0_C0_C5_4,       (5U << 4) | 4U)
            FWD_ID(S3_0_C0_C5_5,       (5U << 4) | 5U)
            /* CRm=6: ID_AA64ISAR0-3 */
            FWD_ID(S3_0_C0_C6_0,       (6U << 4) | 0U)
            FWD_ID(S3_0_C0_C6_1,       (6U << 4) | 1U)
            FWD_ID(S3_0_C0_C6_2,       (6U << 4) | 2U)
            FWD_ID(S3_0_C0_C6_3,       (6U << 4) | 3U)
            /* CRm=7: ID_AA64MMFR0-4 */
            FWD_ID(S3_0_C0_C7_0,       (7U << 4) | 0U)
            FWD_ID(S3_0_C0_C7_1,       (7U << 4) | 1U)
            FWD_ID(S3_0_C0_C7_2,       (7U << 4) | 2U)
            FWD_ID(S3_0_C0_C7_3,       (7U << 4) | 3U)
            FWD_ID(S3_0_C0_C7_4,       (7U << 4) | 4U)
            default:
                /* Unknown ID-group read: read-as-zero and say so -- matches
                 * hardware without the feature, and keeps the gap visible. */
                printk("[hv] TID3: unknown ID reg C0_C%u_%u read as 0\n",
                       (unsigned)crm, (unsigned)op2);
                break;
            }
            if (rt != 31U)
                regs->x[rt] = v;
        }
        regs->elr_el2 += 4ULL;
        return;
    }

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

void handle_exit(struct arch_regs *regs, u64 esr)
{
    u32 ec = (u32)(esr >> 26) & 0x3FU;

    switch (ec) {
    case EC_WFX:      /* no scheduler yet: wfi/wfe pass through as no-ops */
        handle_wfx(regs, esr);
        return;
    case EC_HVC64:
        handle_hvc(regs);
        return;
    case EC_SMC64:    /* ADR-0017: answer SMCCC_NOT_SUPPORTED, never EL3 */
        handle_smc(regs);
        return;
    case EC_SYS64:    /* see handle_sysreg_trap */
        handle_sysreg_trap(regs, esr);
        return;
    case EC_DABT_LOW: /* → MMIO trap-and-emulate */
        if (mmio_handle_data_abort(regs, esr) == 0)
            return;   /* handled: ELR advanced; el1_sync_handler erets back */
        /* fall through to the diagnostic + park on no handler / ISV=0 */
        /* fallthrough */
    default:
        printk("[hv] unexpected exit cpu%u EC=0x%x ESR=0x%lx ELR=0x%lx\n",
               (unsigned)current_vcpu_id(), (unsigned)ec, esr, regs->elr_el2);
        cpu_arch_halt();
    }
}
