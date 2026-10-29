/* SPDX-License-Identifier: TBD */
/*
 * SVM case "psci" (m11-pre-b): the completed PSCI/SMCCC interface surface,
 * asserted from the guest over the hvc conduit.
 *
 * Flow: SMCCC probe (version + ARCH_FEATURES), then the new PSCI functions'
 * return codes (AFFINITY_INFO both paths, MIGRATE_INFO_TYPE, FEATURES).
 * CPU_SUSPEND(standby) is exercised with the virtual timer pre-armed: the
 * call sleeps at EL2, the timer PPI (INTID 27) wakes it through the existing
 * HW-forward path, and the guest IRQ handler disarms/EOIs it (vtimer case
 * skeleton). CPU_SUSPEND(power_down) then asserts the retention contract:
 * the caller's local state survives the call.
 *
 * Run: SVM_BIN=build/svm/svm-psci.bin make run   (HV_GUEST=svm build)
 */
#include "../svm_lib.h"

/* Keep in sync with arch/psci.h (test guest has no access to hv headers). */
#define PSCI_CPU_SUSPEND_32     0x84000001UL
#define PSCI_AFFINITY_INFO_32   0x84000004UL
#define PSCI_MIGRATE_INFO_TYPE  0x84000006UL
#define PSCI_FEATURES           0x8400000AUL
#define PSCI_STATE_TYPE_POWERDOWN (1UL << 30)

#define SMCCC_VERSION_FN        0x80000000UL
#define SMCCC_ARCH_FEATURES_FN  0x80000001UL

#define VTIMER_INTID    27U
#define RET_INVALID_PARAMETERS  (~1UL)   /* -2 */

static volatile int g_fired;

void svm_irq_handler(void)
{
    unsigned long iar, intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == VTIMER_INTID) {
        __asm__ volatile("msr CNTV_CTL_EL0, %0" :: "r"(0UL));
        __asm__ volatile("isb");
        svm_puts("SVM: timer woke the suspended CPU\n");
    }

    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    __asm__ volatile("msr ICC_DIR_EL1, %0"   :: "r"(iar));
    __asm__ volatile("isb");
    g_fired = 1;
}

static void check(int cond, const char *what)
{
    svm_puts(cond ? what : "SVM: CHECK FAILED\n");
}

void SVM_ENTRY _start(void)
{
    unsigned long r, freq;

    check(svm_hvc(SMCCC_VERSION_FN, 0) == 0x10001UL,
          "SVM: SMCCC version 1.1\n");
    check(svm_hvc(SMCCC_ARCH_FEATURES_FN, SMCCC_ARCH_FEATURES_FN) == 0,
          "SVM: SMCCC ARCH_FEATURES(self) == 0\n");
    check(svm_hvc(PSCI_FEATURES, PSCI_CPU_SUSPEND_32) == 0,
          "SVM: FEATURES(CPU_SUSPEND) == 0\n");
    check(svm_hvc4(PSCI_AFFINITY_INFO_32, 0UL, 0UL, 0UL) == 0,
          "SVM: AFFINITY_INFO(aff0=0, lvl0) == ON\n");
    check(svm_hvc4(PSCI_AFFINITY_INFO_32, 0x100UL, 0UL, 0UL)
              == RET_INVALID_PARAMETERS,
          "SVM: AFFINITY_INFO(aff1!=0) == -2\n");
    check(svm_hvc(PSCI_MIGRATE_INFO_TYPE, 0) == 2,
          "SVM: MIGRATE_INFO_TYPE == 2 (no TOS, MP)\n");

    /* CPU_SUSPEND(standby) with the timer armed: the call must sleep and be
     * woken by the timer PPI through the HW-forward path. */
    svm_gic_el1_init();
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" :: "r"(freq / 100UL));
    __asm__ volatile("msr CNTV_CTL_EL0, %0"  :: "r"(1UL));
    __asm__ volatile("isb");
    __asm__ volatile("msr daifclr, #2");

    r = svm_hvc4(PSCI_CPU_SUSPEND_32, 0UL, 0UL, 0UL);   /* standby */
    check(r == 0 && g_fired, "SVM: CPU_SUSPEND(standby) slept and woke\n");

    /* CPU_SUSPEND(power_down): retention contract -- caller context survives
     * (entry/context ignored, KVM precedent; spec'd for the M11 rework).
     * Re-arm the timer: the call sleeps until a wake event like standby. */
    volatile unsigned long canary = 0xC0FFEEUL;
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" :: "r"(freq / 100UL));
    __asm__ volatile("msr CNTV_CTL_EL0, %0"  :: "r"(1UL));
    __asm__ volatile("isb");
    r = svm_hvc4(PSCI_CPU_SUSPEND_32, PSCI_STATE_TYPE_POWERDOWN, 0UL, 0UL);
    check(r == 0 && canary == 0xC0FFEEUL,
          "SVM: CPU_SUSPEND(powerdown) returned, context intact\n");

    svm_done(0);
}
