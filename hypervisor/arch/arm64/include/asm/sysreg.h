/* SPDX-License-Identifier: TBD */
#ifndef HV_ASM_SYSREG_H
#define HV_ASM_SYSREG_H

#include <types.h>

#define SYSREG_READ(reg) ({                              \
    u64 __v;                                             \
    __asm__ volatile("mrs %0, " #reg : "=r"(__v));       \
    __v;                                                 \
})

#define SYSREG_WRITE(reg, val) do {                      \
    u64 __v = (u64)(val);                                \
    __asm__ volatile("msr " #reg ", %0" :: "r"(__v));    \
} while (0)

#endif /* HV_ASM_SYSREG_H */
