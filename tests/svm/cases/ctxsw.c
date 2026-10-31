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
static volatile unsigned char vcpu1_stack[0x4000] __attribute__((aligned(16), used));

/* Callee-saved bank stamps: these live across the whole switch cycle only
 * if put/resume preserves the GPR frame — which the per-exception asm owns
 * (saved into vcpu->regs at every exit). Any rot here is a lost frame. */
#define STAMP(i) ((unsigned long)0xD00D0000UL + (i))

static void vcpu1_main(void) __attribute__((used));
static void vcpu1_main(void)
{
    for (;;) {
        if (turn >= ROUNDS)
            for (;;)
                __asm__ volatile("wfi");   /* park: vCPU0 concludes */
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

static void check(int cond, const char *what)
{
    svm_puts(cond ? what : "CTXSW: CHECK FAILED\n");
}

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
        svm_puts("CTXSW: r0\n");          /* fixed-string expect lines */
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

    svm_done(0);
}
