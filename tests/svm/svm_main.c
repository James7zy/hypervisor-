/* SPDX-License-Identifier: TBD */
/*
 * Minimal bare-metal SVM guest for M1 / M1.5 verification.
 *
 * Loaded by QEMU at IPA 0x40200000 via:
 *   -device loader,file=svm.bin,addr=0x40200000
 *
 * M1.5: issues HVC PSCI_VERSION (0x84000000), then reports the returned
 * version word back to the hypervisor through HC_GUEST_DONE in x1. The
 * hypervisor prints "[hv] SVM HVC: done (x1=0x<version>)", which the
 * integration test greps for.
 *
 * Build:
 *   CROSS=aarch64-none-linux-gnu-
 *   ${CROSS}gcc -ffreestanding -nostdlib -nostartfiles \
 *       -T tests/svm/svm.lds -o tests/svm/svm.elf tests/svm/svm_main.c
 *   ${CROSS}objcopy -O binary tests/svm/svm.elf tests/svm/svm.bin
 *
 * Run:
 *   SVM_BIN=tests/svm/svm.bin make run
 */

#define HC_GUEST_DONE 0x80000001UL
#define PSCI_VERSION  0x84000000UL

void _start(void)
{
    unsigned long version;

    /* 1. Query PSCI_VERSION. 0x84000000 == 0x8400 << 16, a single movz. */
    {
        register unsigned long r0 __asm__("x0") = PSCI_VERSION;
        __asm__ volatile("hvc #0" : "+r"(r0) :: "memory");
        version = r0;   /* expect 0x00010001 once M1.5 lands */
    }

    /* 2. Report the version back via the vendor "done" hypercall (x1). */
    {
        register unsigned long r0 __asm__("x0") = HC_GUEST_DONE;
        register unsigned long r1 __asm__("x1") = version;
        __asm__ volatile("hvc #0" : "+r"(r0) : "r"(r1) : "memory");
    }

    for (;;)
        __asm__ volatile("wfi");
}
