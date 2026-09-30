/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_SPINLOCK_H
#define HV_ARCH_SPINLOCK_H

#include <types.h>

/* Atomically return *p and store *p + 1 (LDXR/STXR retry loop; no LSE, and
 * no outline-atomics helper, which the freestanding link does not provide). */
static inline u32 atomic_arch_fetch_inc_u32(u32 *p)
{
    u32 old, tmp, fail;

    __asm__ volatile(
        "1: ldxr    %w0, [%3]\n"        /* old = *p                    */
        "   add     %w1, %w0, #1\n"     /* tmp = old + 1               */
        "   stxr    %w2, %w1, [%3]\n"   /* try *p = tmp; fail==0 wins  */
        "   cbnz    %w2, 1b\n"
        : "=&r"(old), "=&r"(tmp), "=&r"(fail)
        : "r"(p)
        : "memory");
    return old;
}

/* Spin-wait hint. */
static inline void cpu_arch_relax(void)
{
    __asm__ volatile("yield");
}

#endif /* HV_ARCH_SPINLOCK_H */
