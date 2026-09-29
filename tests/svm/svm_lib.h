/* SPDX-License-Identifier: TBD */
/*
 * Shared runtime for the bare-metal SVM test guests (tests/svm/cases/<case>.c).
 *
 * Every case is linked with svm_lib.c + svm_vectors.S + svm.lds into its own
 * build/svm/svm-<case>.bin, loaded at IPA 0x40200000. One binary per case,
 * because HC_GUEST_DONE ends the QEMU run and the dual-VM scenario needs two
 * distinguishable images.
 *
 * A case provides _start (placed in .text.start so it sits at the load
 * address) and, if it takes interrupts, svm_irq_handler().
 */
#ifndef SVM_LIB_H
#define SVM_LIB_H

#define HC_GUEST_DONE 0x80000001UL
#define PSCI_VERSION  0x84000000UL

#define SVM_ENTRY __attribute__((section(".text.start")))

/* Write to the PL011 at 0x09000000 (trapped to the hypervisor's vuart). */
void svm_puts(const char *s);

/* Issue HVC #0 with x0/x1; returns the x0 the hypervisor hands back. */
unsigned long svm_hvc(unsigned long x0, unsigned long x1);

/* HC_GUEST_DONE with x1 = result; the hypervisor ends the test here. */
void svm_done(unsigned long result) __attribute__((noreturn));

/* Point VBAR_EL1 at svm_vectors and enable the EL1 ICC_* interface with
 * Group-1 IRQs. PSTATE.I stays masked; the case unmasks when ready. */
void svm_gic_el1_init(void);

/* Called from svm_vectors.S on a virtual IRQ. Weak default parks. */
void svm_irq_handler(void);

#endif /* SVM_LIB_H */
