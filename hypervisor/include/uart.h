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

#endif /* HV_UART_H */
