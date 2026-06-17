/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <board.h>
#include "gpa.h"

void *gpa_to_hva_len(u64 gpa, u64 len)
{
    u64 base = BOARD_LINUX_RAM_IPA;
    u64 end  = BOARD_LINUX_RAM_IPA + BOARD_LINUX_RAM_SIZE;

    /* Reject wrap, zero-length, and anything outside the RAM window. */
    if (len == 0 || gpa < base || gpa > end)
        return NULL;
    if (gpa + len < gpa)                 /* overflow */
        return NULL;
    if (gpa + len > end)
        return NULL;

    return (void *)(uintptr_t)(gpa - BOARD_LINUX_RAM_IPA + BOARD_LINUX_RAM_PA);
}

void *gpa_to_hva(u64 gpa)
{
    return gpa_to_hva_len(gpa, 1);
}
