/* SPDX-License-Identifier: TBD */
#ifndef HV_BOARD_INFO_H
#define HV_BOARD_INFO_H

#include <types.h>

/*
 * Board data every board provides (ADR-0008 static board config, reached
 * without the arch-private BOARD_* macros; ADR-0015). Defined in the active
 * board's board.c.
 */
extern const char      board_name[];
extern const uintptr_t board_uart_base;   /* physical console UART */

#endif /* HV_BOARD_INFO_H */
