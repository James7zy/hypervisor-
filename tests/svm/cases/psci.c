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
#define PSCI_CPU_OFF            0x84000002UL
#define PSCI_AFFINITY_INFO_32   0x84000004UL
#define PSCI_MIGRATE_INFO_TYPE  0x84000006UL
#define PSCI_FEATURES           0x8400000AUL
#define PSCI_CPU_ON_64          0xC4000003UL
#define PSCI_STATE_TYPE_POWERDOWN (1UL << 30)
#define PSCI_RET_ALREADY_ON     (~3UL)   /* -4 */
#define PSCI_AFF_LEVEL_OFF      1UL

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

/* ---- vCPU-granularity assertions (M11 slice A) ------------------------- */

static volatile unsigned long sec_flag;   /* 1=alive, 2=release, 3=off-ing */
/* Referenced only from the asm entry stub below — keep the symbols. */
static unsigned char psci_sec_stack[0x4000]
    __attribute__((aligned(16), used));
static void psci_secondary_main(void) __attribute__((used));
static void psci_secondary_main(void)
{
    svm_puts("SVM: vCPU1 alive\n");
    sec_flag = 1;
    while (sec_flag != 2)
        ;
    svm_puts("SVM: vCPU1 CPU_OFF\n");
    sec_flag = 3;
    (void)svm_hvc(PSCI_CPU_OFF, 0);   /* stops this vCPU; no return to us */
    for (;;)
        ;                             /* not reached while vCPU is stopped */
}

/* Naked secondary entry per the arm64 boot protocol: the hypervisor authors
 * elr/x0/spsr only — SP is ours to set (vspi_entry.S precedent). */
extern char psci_secondary_entry[];   /* asm label below */
__asm__(
    ".global psci_secondary_entry\n"
    "psci_secondary_entry:\n"
    "   msr daifset, #0xf\n"
    "   ldr x0, =psci_sec_stack + 0x4000\n"
    "   mov sp, x0\n"
    "   b psci_secondary_main\n"
);

static int wait_flag(unsigned long v, unsigned long spins)
{
    while (spins--) {
        if (sec_flag == v)
            return 1;
    }
    return 0;
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

    /* CPU_ON brings up vCPU1 (affinity Aff0=1) via the slice-A kick path. */
    r = svm_hvc4(PSCI_CPU_ON_64, 1UL,
                 (unsigned long)psci_secondary_entry, 0xCAFEUL);
    check(r == 0, "SVM: CPU_ON(vCPU1) == SUCCESS\n");
    check(wait_flag(1, 200000000UL), "SVM: vCPU1 alive\n");

    /* vCPU1 is authorized/running: a second CPU_ON must report ALREADY_ON
     * (percpu[].online is always true now — the check is vCPU state). */
    r = svm_hvc4(PSCI_CPU_ON_64, 1UL,
                 (unsigned long)psci_secondary_entry, 0UL);
    check(r == PSCI_RET_ALREADY_ON,
          "SVM: CPU_ON(vCPU1) again == ALREADY_ON\n");

    /* Release vCPU1: it CPU_OFFs ITSELF; vCPU0 must survive that. */
    sec_flag = 2;
    check(wait_flag(3, 200000000UL), "SVM: vCPU1 CPU_OFF\n");
    r = svm_hvc4(PSCI_AFFINITY_INFO_32, 1UL, 0UL, 0UL);
    check(r == PSCI_AFF_LEVEL_OFF,
          "SVM: AFFINITY_INFO(vCPU1) == OFF after CPU_OFF\n");
    svm_puts("SVM: vCPU0 alive after sibling CPU_OFF\n");

    svm_done(0);
}
