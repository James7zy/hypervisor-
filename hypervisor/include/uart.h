/* SPDX-License-Identifier: TBD */
#ifndef HV_UART_H
#define HV_UART_H

#include <types.h>

/* Configure UART at `base` for 115200 8N1, TX polling. No RX in M0. */
void uart_init(uintptr_t base);

/* Spin until TX FIFO has room, then send `c`. No \r translation. */
void uart_putc(char c);

#endif /* HV_UART_H */
