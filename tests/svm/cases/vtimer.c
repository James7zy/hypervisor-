/* SPDX-License-Identifier: TBD */
/*
 * SVM case "vtimer" (M2.5): hardware-forwarded virtual-timer injection.
 *
 * Flow: install the shared vectors and enable the ICC_* interface, arm the
 * EL1 virtual timer (CNTV_TVAL/CTL), unmask PSTATE.I, and wait. The timer PPI
 * (INTID 27) fires, is taken at EL2, and is hardware-forwarded back into this
 * guest's vGIC (ICH_LR0.HW=1). svm_irq_handler disarms the timer (dropping
 * the level-sensitive line), priority-drops (EOIR1) and deactivates (DIR) the
 * virtual INTID — releasing the physical INTID via the LR HW linkage. Then
 * HVC HC_GUEST_DONE.
 *
 * Run: SVM_BIN=build/svm/svm-vtimer.bin make run   (HV_GUEST=svm build)
 */
#include "../svm_lib.h"

#define VTIMER_INTID    27U

static volatile int g_fired;

void svm_irq_handler(void)
{
    unsigned long iar;
    unsigned long intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == VTIMER_INTID)
        svm_puts("[svm] virtual timer IRQ received (INTID=27)\n");
    else
        svm_puts("[svm] virtual timer IRQ received (unexpected INTID)\n");

    /* Disarm the timer so the level-sensitive line de-asserts. */
    __asm__ volatile("msr CNTV_CTL_EL0, %0" :: "r"(0UL));
    __asm__ volatile("isb");

    /* Priority-drop then deactivate the virtual INTID. EOImode is independent
     * for the virtual interface; DIR releases the forwarded physical INTID. */
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    __asm__ volatile("msr ICC_DIR_EL1, %0"   :: "r"(iar));
    __asm__ volatile("isb");

    g_fired = 1;
}

void SVM_ENTRY _start(void)
{
    unsigned long freq;

    svm_puts("[svm] EL1 init\n");

    svm_gic_el1_init();
    svm_puts("[svm] GIC EL1 configured\n");

    /* Arm the EL1 virtual timer to fire in ~1/100 s. */
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" :: "r"(freq / 100UL));
    __asm__ volatile("msr CNTV_CTL_EL0, %0"  :: "r"(1UL));   /* ENABLE, unmasked */
    __asm__ volatile("isb");
    svm_puts("[svm] virtual timer armed\n");

    /* Unmask PSTATE.I so the forwarded virtual IRQ is taken. */
    __asm__ volatile("msr daifclr, #2");

    while (!g_fired)
        __asm__ volatile("wfi");

    svm_puts("[svm] signalling HVC done\n");
    svm_done(0);
}
