/* SPDX-License-Identifier: TBD */
/*
 * Minimal bare-metal SVM guest for M5 slice 3: VM1's half of the dual-VM
 * test scenario. Structurally identical to tests/svm/svm_main.c (M1/M1.5's
 * VM0 guest) -- same PSCI_VERSION-then-HC_GUEST_DONE flow -- plus a UART
 * banner print (same direct-PL011-write pattern as tests/svm2/svm2_main.c
 * and tests/svm3/svm3_main.c) so this VM's output is independently
 * identifiable in the QEMU log, proving VM1 actually booted.
 *
 * Loaded by QEMU at IPA 0x40200000 (SAME guest IPA as VM0's svm.bin --
 * unified guest address map, M5 design) via:
 *   -device loader,file=svm4.bin,addr=0xC0200000
 * Stage-2 maps that shared IPA to the DIFFERENT backing PA BOARD_SVM2_RAM_PA
 * for VM1; this guest is linked at the SAME link address as svm.lds
 * (0x40200000) so the compiled code is correct regardless of which physical
 * RAM is actually behind that IPA.
 *
 * Guest accesses to UART_BASE trap to the hypervisor's vuart model (M5 slice
 * 2); TX is never focus-gated (see hypervisor/dm/vuart.c / irq_handler.c), so
 * this banner reaches the physical console unconditionally, the same as
 * VM0's boot banner.
 *
 * Build (see Makefile target `svm4`):
 *   ${CROSS}gcc -ffreestanding -nostdlib -nostartfiles -O2 -Werror \
 *       -T tests/svm4/svm4.lds -o build/svm4/svm4.elf tests/svm4/svm4_main.c
 *   ${CROSS}objcopy -O binary build/svm4/svm4.elf build/svm4/svm4.bin
 *
 * Run (dual-VM):
 *   SVM_BIN=build/.../svm.bin SVM_BIN2=build/.../svm4.bin make run
 */

#define UART_BASE     0x09000000UL
#define UART_DR       0x00U
#define UART_FR       0x18U
#define UART_FR_TXFF  (1U << 5)

#define HC_GUEST_DONE 0x80000001UL
#define PSCI_VERSION  0x84000000UL

static void uart_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)(UART_BASE + UART_FR);
    volatile unsigned int *dr = (volatile unsigned int *)(UART_BASE + UART_DR);
    while (*fr & UART_FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void _start(void)
{
    unsigned long version;

    uart_puts("SVM4: hello from VM1\n");

    /* 1. Query PSCI_VERSION. 0x84000000 == 0x8400 << 16, a single movz. */
    {
        register unsigned long r0 __asm__("x0") = PSCI_VERSION;
        __asm__ volatile("hvc #0" : "+r"(r0) :: "memory");
        version = r0;
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
