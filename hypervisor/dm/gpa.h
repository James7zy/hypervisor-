/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_GPA_H
#define HV_DM_GPA_H

#include <types.h>

/*
 * Translate a guest-physical address (IPA) inside the guest RAM window to a
 * hypervisor-usable pointer (the backing PA, which the hv maps 1:1).
 *
 * The M3.0 Stage-2 map is non-identity: guest IPA BOARD_LINUX_RAM_IPA maps to
 * PA BOARD_LINUX_RAM_PA over BOARD_LINUX_RAM_SIZE bytes. Returns NULL if `gpa`
 * (or `gpa + len - 1`) falls outside that window; callers must drop the access.
 */
void *gpa_to_hva(u64 gpa);

/* Same, but require [gpa, gpa+len) to fit entirely in the RAM window. */
void *gpa_to_hva_len(u64 gpa, u64 len);

#endif /* HV_DM_GPA_H */
