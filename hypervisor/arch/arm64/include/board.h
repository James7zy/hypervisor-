/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_BOARD_H
#define HV_ARCH_BOARD_H

/*
 * Per-arch board indirection header (M0 placeholder).
 *
 * In M0 the board directory is listed first in -I order, so
 * #include <board.h> resolves directly to the active board's board.h.
 * This file is never reached by that lookup. It exists as a documented
 * placeholder for M1+ arch-level board glue (e.g. struct board_info).
 */

#endif /* HV_ARCH_BOARD_H */
