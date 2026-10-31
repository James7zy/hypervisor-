/* SPDX-License-Identifier: TBD */
/*
 * SVM case "ctxsw" (M11 slice B1): cooperative round-robin on a SHARED pCPU.
 * The svm_ctxsw profile affines both vCPUs to pCPU0; the B1 scheduler plus
 * the WFI yield point rotate them. GPR-level correctness: vCPU0 stamps the
 * callee-saved bank, both vCPUs take turns printing a fixed marker per round
 * and yielding; after ROUNDS rounds vCPU0 verifies its stamps survived every
 * switch. Full EL1-sysreg clobber detection (SCTLR/TTBR/MAIR/VBAR stamps)
 * joins in slice B2 when vcpu_arch_put grows the bulk save/restore set.
 *
 * Case boundary (gate-1 R5): no vGIC use, no CNTV programming, DAIF stays
 * masked — this profile is publisher-free by construction.
 *
 * Run: make test-qemu-ctxsw   (in the `test` aggregate since B1)
 */
#include "../svm_lib.h"

/* Keep in sync with arch/psci.h (test guest has no access to hv headers). */
#define PSCI_CPU_ON_64  0xC4000003UL

#define ROUNDS 8

static volatile unsigned long turn;      /* round counter shared by both */
/* ---- vtimer segment (M11 B2 T3, D4) ---------------------------------- */
/* R5 CNTV/vGIC bans LIFT for this segment (per the B2 plan): the
 * publisher-wins merge is pending (T5); interim safety = all publishers
 * local (single pCPU, no console input) and put's IMASK keeps PPI 27
 * firing only on the loaded owner. Two vCPUs arm DISTINCT deadlines on
 * one pCPU; each interrupt must land in its owner's context and each
 * owner's timer state must survive every switch. */
static volatile unsigned long fired_count;

void svm_irq_handler(void)
{
    unsigned long iar, intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == 27U) {
        __asm__ volatile("msr CNTV_CTL_EL0, %0" :: "r"(0UL));  /* disarm MINE */
        __asm__ volatile("isb");
        fired_count++;
    }

    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    __asm__ volatile("msr ICC_DIR_EL1, %0"   :: "r"(iar));
    __asm__ volatile("isb");
}

static void arm_my_timer(unsigned long divisor)
{
    unsigned long freq;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" :: "r"(freq / divisor));
    __asm__ volatile("msr CNTV_CTL_EL0, %0"  :: "r"(1UL));
    __asm__ volatile("isb");
    __asm__ volatile("msr daifclr, #2");
}

/* Rotate (print + wfi-yield) until `cond`; returns 0 on timeout. */
static int rotate_until(volatile int *cond, int vcpu1)
{
    for (int i = 0; i < 4000; i++) {
        if (*cond)
            return 1;
        svm_puts(vcpu1 ? "CTXSW: v1\n" : "CTXSW: r0\n");
        __asm__ volatile("wfi");
    }
    return *cond;
}


static void check(int cond, const char *what)
{
    svm_puts(cond ? what : "CTXSW: CHECK FAILED\n");
}

static volatile unsigned char vcpu1_stack[0x4000] __attribute__((aligned(16), used));

/* Callee-saved bank stamps: these live across the whole switch cycle only
 * if put/resume preserves the GPR frame — which the per-exception asm owns
 * (saved into vcpu->regs at every exit). Any rot here is a lost frame. */
#define STAMP(i) ((unsigned long)0xD00D0000UL + (i))

static volatile int t1_done;

static void vcpu1_main(void) __attribute__((used));
static void vcpu1_main(void)
{
    for (;;) {
        if (turn >= ROUNDS) {
            /* GPR rounds over: timer phase. My deadline is the LONG one. */
            svm_gic_el1_init();
            arm_my_timer(20UL);            /* vCPU0 armed 1/50 */
            if (rotate_until(&t1_done, 1)) {
                unsigned long ctl;
                __asm__ volatile("mrs %0, CNTV_CTL_EL0" : "=r"(ctl));
                check(ctl == 0UL, "CTXSW: t1 timer is MY disarmed one\n");
                svm_puts("CTXSW: t1 timer fired\n");
            }
            for (;;)
                __asm__ volatile("wfi");   /* park: vCPU0 concludes */
        }
        svm_puts("CTXSW: v1\n");
        __asm__ volatile("wfi");           /* yield to vCPU0 */
    }
}

extern char ctxsw_secondary_entry[];      /* asm label below */
__asm__(
    ".global ctxsw_secondary_entry\n"
    "ctxsw_secondary_entry:\n"
    "   msr daifset, #0xf\n"
    "   ldr x0, =vcpu1_stack + 0x4000\n"
    "   mov sp, x0\n"
    "   b vcpu1_main\n"
);

void SVM_ENTRY _start(void)
{
    /* Stamp x19-x28 as register-asm locals: the compiler tracks them, so
     * it will NOT allocate its own values over them (plain clobber-only asm
     * writes are invisible to gcc, which happily reused x19-x23 for loop
     * variables in the outermost frame — the first cut of this case). Calls
     * preserve them per the ABI; the switch must return them untouched. */
    register unsigned long r19 asm("x19") = STAMP(0);
    register unsigned long r20 asm("x20") = STAMP(1);
    register unsigned long r21 asm("x21") = STAMP(2);
    register unsigned long r22 asm("x22") = STAMP(3);
    register unsigned long r23 asm("x23") = STAMP(4);
    register unsigned long r24 asm("x24") = STAMP(5);
    register unsigned long r25 asm("x25") = STAMP(6);
    register unsigned long r26 asm("x26") = STAMP(7);
    register unsigned long r27 asm("x27") = STAMP(8);
    register unsigned long r28 asm("x28") = STAMP(9);

    svm_puts("CTXSW: stamps written (x19-x28)\n");

    unsigned long r = svm_hvc4(PSCI_CPU_ON_64, 1UL,
                               (unsigned long)ctxsw_secondary_entry, 0UL);
    check(r == 0, "CTXSW: CPU_ON(vCPU1) == SUCCESS\n");

    while (turn < ROUNDS) {
        /* Round-indexed markers: the expect file pins r0..r7, so a
         * scheduler that under-rotates fails the gate (tester N4). */
        static const char *const marks[ROUNDS] = {
            "CTXSW: r0\n", "CTXSW: r1\n", "CTXSW: r2\n", "CTXSW: r3\n",
            "CTXSW: r4\n", "CTXSW: r5\n", "CTXSW: r6\n", "CTXSW: r7\n",
        };
        svm_puts(marks[turn]);
        turn++;
        __asm__ volatile("wfi");          /* yield to vCPU1 */
    }

    /* Verify the stamps survived ROUNDS full switch cycles. */
    check(r19 == STAMP(0) && r20 == STAMP(1) && r21 == STAMP(2) &&
          r22 == STAMP(3) && r23 == STAMP(4),
          "CTXSW: x19-x23 stamps intact\n");
    check(r24 == STAMP(5) && r25 == STAMP(6) && r26 == STAMP(7) &&
          r27 == STAMP(8) && r28 == STAMP(9),
          "CTXSW: x24-x28 stamps intact\n");

    /* Timer phase (D4): I arm the SHORT deadline; vCPU1 arms the LONG one
     * when it sees the GPR rounds are done. Exactly two PPIs total; each
     * must land in its OWNER's context (its handler disarms its own view);
     * each owner's CTL must read its own disarm afterwards. My wait loop
     * keeps yielding (wfi three-way), so vCPU1 rotates meanwhile. */
    svm_gic_el1_init();
    arm_my_timer(50UL);
    while (fired_count < 1UL)
        __asm__ volatile("wfi");
    {
        unsigned long ctl;
        __asm__ volatile("mrs %0, CNTV_CTL_EL0" : "=r"(ctl));
        check(ctl == 0UL, "CTXSW: t0 timer is MY disarmed one\n");
        svm_puts("CTXSW: t0 timer fired\n");
    }
    while (fired_count < 2UL)
        __asm__ volatile("wfi");            /* vCPU1's long deadline */
    t1_done = 1;                            /* release vCPU1's waiter */
    for (volatile int i = 0; i < 20000000; i++)
        ;                                   /* let vCPU1 finish its check */
    svm_puts("CTXSW: both timers fired in their owners\n");

    svm_done(0);
}
