/* SPDX-License-Identifier: TBD */
/*
 * M2.5 timer SVM guest: proves hardware-forwarded virtual-timer injection.
 *
 * Loaded by QEMU at IPA 0x40200000 via:
 *   -device loader,file=svm3.bin,addr=0x40200000
 *
 * Flow: install VBAR_EL1, enable the ICC_* interface, arm the EL1 virtual
 * timer (CNTV_TVAL/CTL), unmask PSTATE.I, and wait. The timer PPI (INTID 27)
 * fires, is taken at EL2, and is hardware-forwarded back into this guest's
 * vGIC (ICH_LR0.HW=1). The virtual CPU interface delivers it to
 * svm3_irq_handler, which disarms the timer (dropping the level-sensitive
 * line), priority-drops (EOIR1) and deactivates (DIR) the virtual INTID —
 * releasing the physical INTID via the LR HW linkage. Then HVC HC_GUEST_DONE.
 *
 * Build (see Makefile target `svm3`):
 *   ${CROSS}gcc -ffreestanding -nostdlib -nostartfiles -O2 -Werror \
 *       -T tests/svm3/svm3.lds -o build/svm3/svm3.elf \
 *       tests/svm3/svm3_main.c tests/svm3/svm3_vectors.S
 *   ${CROSS}objcopy -O binary build/svm3/svm3.elf build/svm3/svm3.bin
 */

#define UART_BASE     0x09000000UL
#define UART_DR       0x00U
#define UART_FR       0x18U
#define UART_FR_TXFF  (1U << 5)

#define HC_GUEST_DONE   0x80000001UL
#define VTIMER_INTID    27U

static volatile int g_fired;

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

/* Called from svm3_vectors.S (svm3_irq_entry) on the forwarded virtual IRQ. */
void svm3_irq_handler(void)
{
    unsigned long iar;
    unsigned long intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == VTIMER_INTID)
        uart_puts("[svm] virtual timer IRQ received (INTID=27)\n");
    else
        uart_puts("[svm] virtual timer IRQ received (unexpected INTID)\n");

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

void __attribute__((section(".text.start"))) _start(void)
{
    extern char svm3_vectors[];
    unsigned long freq;

    uart_puts("[svm] EL1 init\n");

    /* Point EL1 exceptions at our own vector table. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(svm3_vectors));
    __asm__ volatile("isb");

    /* Enable the EL1 ICC_* system-register interface and Group-1 IRQs.
     * These succeed only once the hypervisor sets ICC_SRE_EL2.Enable=1. */
    __asm__ volatile("msr ICC_SRE_EL1, %0"     :: "r"(7UL));   /* SRE|DFB|DIB */
    __asm__ volatile("isb");
    __asm__ volatile("msr ICC_PMR_EL1, %0"     :: "r"(0xFFUL)); /* allow all prios */
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    __asm__ volatile("isb");
    uart_puts("[svm] GIC EL1 configured\n");

    /* Arm the EL1 virtual timer to fire in ~1/100 s. */
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" :: "r"(freq / 100UL));
    __asm__ volatile("msr CNTV_CTL_EL0, %0"  :: "r"(1UL));   /* ENABLE, unmasked */
    __asm__ volatile("isb");
    uart_puts("[svm] virtual timer armed\n");

    /* Unmask PSTATE.I so the forwarded virtual IRQ is taken. */
    __asm__ volatile("msr daifclr, #2");

    while (!g_fired)
        __asm__ volatile("wfi");

    uart_puts("[svm] signalling HVC done\n");
    {
        register unsigned long r0 __asm__("x0") = HC_GUEST_DONE;
        __asm__ volatile("hvc #0" : "+r"(r0) :: "memory");
    }

    for (;;)
        __asm__ volatile("wfi");
}
