/* SPDX-License-Identifier: TBD */
#include "svm_lib.h"

#define UART_BASE     0x09000000UL
#define UART_DR       0x00U
#define UART_FR       0x18U
#define UART_FR_TXFF  (1U << 5)

static void svm_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)(UART_BASE + UART_FR);
    volatile unsigned int *dr = (volatile unsigned int *)(UART_BASE + UART_DR);
    while (*fr & UART_FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

void svm_puts(const char *s)
{
    while (*s)
        svm_putc(*s++);
}

unsigned long svm_hvc(unsigned long x0, unsigned long x1)
{
    register unsigned long r0 __asm__("x0") = x0;
    register unsigned long r1 __asm__("x1") = x1;
    __asm__ volatile("hvc #0" : "+r"(r0), "+r"(r1) :: "memory");
    return r0;
}

void svm_done(unsigned long result)
{
    svm_hvc(HC_GUEST_DONE, result);
    for (;;)
        __asm__ volatile("wfi");
}

void svm_gic_el1_init(void)
{
    extern char svm_vectors[];

    /* Point EL1 exceptions at the shared vector table. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(svm_vectors));
    __asm__ volatile("isb");

    /* Enable the EL1 ICC_* system-register interface and Group-1 IRQs.
     * These succeed only once the hypervisor sets ICC_SRE_EL2.Enable=1. */
    __asm__ volatile("msr ICC_SRE_EL1, %0"     :: "r"(7UL));   /* SRE|DFB|DIB */
    __asm__ volatile("isb");
    __asm__ volatile("msr ICC_PMR_EL1, %0"     :: "r"(0xFFUL)); /* allow all prios */
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    __asm__ volatile("isb");
}

__attribute__((weak)) void svm_irq_handler(void)
{
    svm_puts("[svm] unexpected IRQ\n");
    for (;;)
        __asm__ volatile("wfi");
}
