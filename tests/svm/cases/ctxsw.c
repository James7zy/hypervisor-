/* SPDX-License-Identifier: TBD */
/*
 * SVM case "ctxsw" (M11 slice A): the context-switch DETECTOR, landed in its
 * controlled-red form. vCPU0 stamps EL1 registers that a real context switch
 * must preserve, CPU_ONs vCPU1 onto the SAME pCPU (svm_ctxsw profile:
 * vcpu_pcpu = {0,0}), and waits. Without a scheduler the pCPU only ever runs
 * the lowest vCPU, so vCPU1 never executes — the case ASSERTS that failure
 * (this is the expected output until slice B lands the runqueue/switch_to,
 * which flips this case green and folds it into `make test`).
 *
 * Stamps are registers NOT consumed until a controlled event (VBAR/CPACR/
 * TPIDR/TPIDRRO(EL0): stamping SCTLR/TTBR/MAIR today would break the running
 * guest (their value is consumed on every access) — those join when slice B
 * actually switches and restores (Q9 discipline: the half-list failure mode
 * is recorded in the daily report, not run).
 *
 * Run: make test-qemu-ctxsw   (independent target, not in the aggregate)
 */
#include "../svm_lib.h"

/* Keep in sync with arch/psci.h (test guest has no access to hv headers). */
#define PSCI_CPU_ON_64  0xC4000003UL

static volatile unsigned long vcpu1_ran;

static volatile unsigned char vcpu1_stack[0x4000] __attribute__((aligned(16)));

static void vcpu1_main(void) __attribute__((used));
static void vcpu1_main(void)
{
    vcpu1_ran = 1;
    for (;;)
        ;                     /* park: no scheduler means we never return */
}

extern char ctxsw_secondary_entry[];   /* asm label below */
__asm__(
    ".global ctxsw_secondary_entry\n"
    "ctxsw_secondary_entry:\n"
    "   msr daifset, #0xf\n"
    "   ldr x0, =vcpu1_stack + 0x4000\n"
    "   mov sp, x0\n"
    "   b vcpu1_main\n"
);

#define STAMP(i) ((unsigned long)0xC0DE0000UL + (i))

static void check(int cond, const char *what)
{
    svm_puts(cond ? what : "CTXSW: CHECK FAILED\n");
}

void SVM_ENTRY _start(void)
{
    unsigned long freq, t0, now;

    /* Stamp the safe subset (see file comment). */
    __asm__ volatile("msr vbar_el1, %0"  ::"r"(STAMP(0)));
    __asm__ volatile("msr cpacr_el1, %0" ::"r"(STAMP(1)));
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(STAMP(2)));
    __asm__ volatile("msr tpidrro_el0, %0"::"r"(STAMP(3)));
    __asm__ volatile("isb");

    svm_puts("CTXSW: EL1 stamps written (VBAR/CPACR/TPIDR/TPIDRRO)\n");

    /* Bring vCPU1 onto this same pCPU. */
    unsigned long r = svm_hvc4(PSCI_CPU_ON_64, 1UL,
                               (unsigned long)ctxsw_secondary_entry, 0UL);
    check(r == 0, "CTXSW: CPU_ON(vCPU1) == SUCCESS\n");

    /* Bounded wait (~CNTVCT third of a second; run_svm_test.sh TIMEOUT=10
     * is wall-clock TCG — keep generous headroom). */
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t0));
    do {
        if (vcpu1_ran)
            break;
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    } while (now - t0 < freq / 3UL);

    check(vcpu1_ran == 0,
          "CTXSW: vCPU1 never ran - no scheduler yet (slice A controlled red)\n");

    /* Nobody else ran, so the stamps must be exactly ours. */
    unsigned long v0, v1, v2, v3;
    __asm__ volatile("mrs %0, vbar_el1"   : "=r"(v0));
    __asm__ volatile("mrs %0, cpacr_el1"  : "=r"(v1));
    __asm__ volatile("mrs %0, tpidr_el1"  : "=r"(v2));
    __asm__ volatile("mrs %0, tpidrro_el0": "=r"(v3));
    check(v0 == STAMP(0) && v1 == STAMP(1) && v2 == STAMP(2) && v3 == STAMP(3),
          "CTXSW: EL1 stamps intact (clobber check begins at slice B)\n");

    svm_done(0);
}
