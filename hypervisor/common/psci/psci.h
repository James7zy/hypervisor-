/* SPDX-License-Identifier: TBD */
#ifndef HV_PSCI_H
#define HV_PSCI_H

#include <vm.h>   /* struct vcpu_regs */

/* PSCI function IDs (Arm DEN0022; SMC32 unless suffixed _64). */
#define PSCI_VERSION       0x84000000U
#define PSCI_CPU_OFF       0x84000002U
#define PSCI_CPU_ON_32     0x84000003U
#define PSCI_SYSTEM_OFF    0x84000008U
#define PSCI_SYSTEM_RESET  0x84000009U
#define PSCI_FEATURES      0x8400000AU
#define PSCI_CPU_ON_64     0xC4000003U

/* Version word: major 1 (bits 31:16), minor 1 (bits 15:0) => PSCI v1.1. */
#define PSCI_VERSION_1_1   0x00010001U

/* Return codes (Arm DEN0022). Negative values sign-extend into the 64-bit reg. */
#define PSCI_RET_SUCCESS              0ULL
#define PSCI_RET_NOT_SUPPORTED       (~0ULL)        /* -1 */
#define PSCI_RET_INVALID_PARAMETERS  (~1ULL)        /* -2 */
#define PSCI_RET_INTERNAL_FAILURE    (~5ULL)        /* -6 */
#define PSCI_RET_ALREADY_ON          (~3ULL)        /* -4 */

/* Dispatch a PSCI call. regs->x[0] holds the function ID on entry; the
 * result (for calls that return) is written back into regs->x[0]. The
 * power-down calls (CPU_OFF/SYSTEM_OFF/SYSTEM_RESET) do not return. */
void psci_handle(struct vcpu_regs *regs);

/* Issue a physical PSCI CPU_ON (smc) to QEMU firmware to power on a secondary
 * pCPU at `entry` (an EL2 PA, since EL2 runs MMU-off) with x0 = ctx_id.
 * `target_mpidr` is the affinity of the target pCPU. Returns the PSCI status. */
s64 psci_cpu_on(u64 target_mpidr, u64 entry, u64 ctx_id);

#endif /* HV_PSCI_H */
