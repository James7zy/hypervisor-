/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_SPINLOCK_H
#define HV_ARCH_SPINLOCK_H

#include <types.h>

/* Atomically return *p and store *p + 1 (gcc emits LOCK XADD inline). */
static inline u32 atomic_arch_fetch_inc_u32(u32 *p)
{
    return __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
}

/* Spin-wait hint. */
static inline void cpu_arch_relax(void)
{
    __asm__ volatile("pause");
}

#endif /* HV_ARCH_SPINLOCK_H */
