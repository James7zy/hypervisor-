/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>

#define UART_DR    0x000
#define UART_FR    0x018
#define UART_IBRD  0x024
#define UART_FBRD  0x028
#define UART_LCR_H 0x02C
#define UART_CR    0x030
#define UART_IMSC  0x038

#define FR_RXFE       (1U << 4)
#define FR_TXFF       (1U << 5)
#define LCR_H_WLEN_8  (3U << 5)
#define LCR_H_FEN     (1U << 4)
#define CR_UARTEN     (1U << 0)
#define CR_TXE        (1U << 8)
#define CR_RXE        (1U << 9)

/* 115200 baud @ 24 MHz: divisor = 24000000 / (16 * 115200) = 13.02 */
#define BAUD_IBRD  13U
#define BAUD_FBRD   1U

static uintptr_t uart_base_addr;

static inline void mmio_write32(uintptr_t addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(uintptr_t addr)
{
    return *(volatile u32 *)addr;
}

void uart_init(uintptr_t base)
{
    uart_base_addr = base;

    mmio_write32(base + UART_CR, 0);
    mmio_write32(base + UART_IMSC, 0);
    mmio_write32(base + UART_IBRD, BAUD_IBRD);
    mmio_write32(base + UART_FBRD, BAUD_FBRD);
    mmio_write32(base + UART_LCR_H, LCR_H_WLEN_8 | LCR_H_FEN);
    mmio_write32(base + UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

void uart_putc(char c)
{
    while (mmio_read32(uart_base_addr + UART_FR) & FR_TXFF);
    mmio_write32(uart_base_addr + UART_DR, (u32)(unsigned char)c);
}

int uart_getc(void)
{
    if (mmio_read32(uart_base_addr + UART_FR) & FR_RXFE)
        return -1;                                   /* RX FIFO empty */
    return (int)(mmio_read32(uart_base_addr + UART_DR) & 0xFFU);
}
