/* SPDX-License-Identifier: TBD */
/*
 * M2 SVM guest: proves vGIC software injection.
 *
 * Loaded by QEMU at IPA 0x40200000 via:
 *   -device loader,file=svm2.bin,addr=0x40200000
 *
 * Flow: install VBAR_EL1, enable the ICC_* interface, unmask PSTATE.I,
 * HVC HC_INJECT_TEST (x1=vINTID). The hypervisor writes a pending vIRQ into
 * ICH_LR0_EL2; on the eret back from the HVC the virtual CPU interface
 * delivers it to svm2_irq_handler, which EOIs it. Then HVC HC_GUEST_DONE.
 *
 * Build (see Makefile target `svm2`):
 *   ${CROSS}gcc -ffreestanding -nostdlib -nostartfiles -O2 -Werror \
 *       -T tests/svm2/svm2.lds -o build/svm2/svm2.elf \
 *       tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S
 *   ${CROSS}objcopy -O binary build/svm2/svm2.elf build/svm2/svm2.bin
 */

#define UART_BASE     0x09000000UL
#define UART_DR       0x00U
#define UART_FR       0x18U
#define UART_FR_TXFF  (1U << 5)

#define HC_INJECT_TEST  0x80000002UL
#define HC_GUEST_DONE   0x80000001UL
#define TEST_VINTID     32U

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

/* Called from svm2_vectors.S (svm2_irq_entry) on a virtual IRQ. */
void svm2_irq_handler(void)
{
    unsigned long iar;
    unsigned long intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == TEST_VINTID)
        uart_puts("[svm] vIRQ received (INTID=32)\n");
    else
        uart_puts("[svm] vIRQ received (unexpected INTID)\n");

    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    __asm__ volatile("isb");
}

void __attribute__((section(".text.start"))) _start(void)
{
    extern char svm2_vectors[];

    uart_puts("[svm] EL1 init\n");

    /* Point EL1 exceptions at our own vector table. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(svm2_vectors));
    __asm__ volatile("isb");

    /* Enable the EL1 ICC_* system-register interface and Group-1 IRQs.
     * These succeed only once the hypervisor sets ICC_SRE_EL2.Enable=1. */
    __asm__ volatile("msr ICC_SRE_EL1, %0"     :: "r"(7UL));   /* SRE|DFB|DIB */
    __asm__ volatile("isb");
    __asm__ volatile("msr ICC_PMR_EL1, %0"     :: "r"(0xFFUL)); /* allow all prios */
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    __asm__ volatile("isb");
    uart_puts("[svm] vGIC EL1 configured\n");

    /* Unmask PSTATE.I so the pending virtual IRQ is taken on return. */
    __asm__ volatile("msr daifclr, #2");

    uart_puts("[svm] requesting injection (vINTID=32)\n");
    {
        register unsigned long r0 __asm__("x0") = HC_INJECT_TEST;
        register unsigned long r1 __asm__("x1") = TEST_VINTID;
        __asm__ volatile("hvc #0" : "+r"(r0) : "r"(r1) : "memory");
    }
    /* The injected vIRQ is delivered here, on return from the inject HVC,
     * before the next statement runs. */

    uart_puts("[svm] signalling HVC done\n");
    {
        register unsigned long r0 __asm__("x0") = HC_GUEST_DONE;
        __asm__ volatile("hvc #0" : "+r"(r0) :: "memory");
    }

    for (;;)
        __asm__ volatile("wfi");
}
