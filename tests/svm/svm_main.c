/* SPDX-License-Identifier: TBD */
/*
 * Minimal bare-metal SVM guest for M1 verification.
 *
 * Loaded by QEMU at IPA 0x40200000 via:
 *   -device loader,file=svm.bin,addr=0x40200000
 *
 * Issues HC_GUEST_DONE (0x80000001) via HVC #0, then spins.
 * The hypervisor's handle_hvc() catches this and calls hv_restore().
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

void _start(void)
{
    /*
     * 0x80000001 exceeds the 16-bit movz range, so a plain integer literal
     * in the "r" constraint would cause "immediate cannot be moved by a
     * single instruction". Pinning to x0 via the asm register constraint
     * lets the compiler emit movz+movk before the inline asm.
     */
    register unsigned long hvc_id __asm__("x0") = HC_GUEST_DONE;
    __asm__ volatile("hvc #0" : "+r"(hvc_id) :: "memory");

    for (;;)
        __asm__ volatile("wfi");
}
