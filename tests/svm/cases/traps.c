/* SPDX-License-Identifier: TBD */
/*
 * SVM case "traps" (m11-pre-a slice 3): the new HCR_EL2 trap set, made
 * observable in order:
 *
 *  1. midr_el1 read -- HCR.TID3 traps ID-group reads; the handler forwards
 *     a value whose top byte is the ARM implementer (0x41).
 *  2. wfi -- HCR.TWI traps it (QEMU: target/arm/tcg/op_helper.c
 *     check_wfx_trap reports ELR AT the instruction); the handler advances
 *     past it as a no-op. Reaching the next print proves the return path.
 *  3. wfe -- HCR.TWE, same.
 *
 * Not covered: HCR.TSC/handle_smc. Without EL3, QEMU forces TSC to RES0
 * (target/arm/helper.c hcr_write) and a guest smc UNDEFs at EL1 -- it never
 * reaches EL2, so smc interception is static-verified until real hardware
 * with EL3 (M15, RK3588).
 */
#include "../svm_lib.h"

static int midr_impl_ok(void)
{
    unsigned long midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    return (midr >> 24) == 0x41UL;   /* implementer 0x41 = ARM Ltd */
}

void SVM_ENTRY _start(void)
{
    svm_puts(midr_impl_ok() ? "SVM: TID3 MIDR forwarded (impl=0x41)\n"
                            : "SVM: TID3 MIDR BAD\n");
    __asm__ volatile("wfi");
    svm_puts("SVM: TWI wfi passed through\n");
    __asm__ volatile("wfe");
    svm_puts("SVM: TWE wfe passed through\n");
    svm_done(0);
}
