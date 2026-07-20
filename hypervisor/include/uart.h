/* SPDX-License-Identifier: TBD */
#ifndef HV_UART_H
#define HV_UART_H

#include <types.h>

/* Configure UART at `base` for 115200 8N1, TX polling. No RX in M0. */
void uart_init(uintptr_t base);

/* Spin until TX FIFO has room, then send `c`. No \r translation. */
void uart_putc(char c);

/* Non-blocking RX: return the next received byte (0..255), or -1 if the RX
 * FIFO is empty. Polls PL011 FR.RXFE; no interrupts. */
int uart_getc(void);

/* Enable the physical RX + receive-timeout interrupts (IMSC RXIM|RTIM) so EL2
 * takes BOARD_PL011_IRQ promptly once it owns the UART exclusively (M5 slice
 * 2). Call once, after uart_init(), on the pCPU that owns the physical SPI. */
void uart_rx_irq_enable(void);

#endif /* HV_UART_H */
