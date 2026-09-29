/* SPDX-License-Identifier: TBD */
/*
 * SVM case "vm1" (M10): VM1's half of the dual-VM scenario.
 *
 * Same PSCI_VERSION-then-HC_GUEST_DONE flow as "basic", plus a UART banner so
 * this VM's output is independently identifiable in the QEMU log, proving
 * VM1 actually booted.
 *
 * QEMU loads it at PA 0xC0200000; Stage-2 maps that to the SAME guest IPA as
 * VM0 (0x40200000, unified guest address map), which is why every case links
 * at 0x40200000 via svm.lds regardless of which physical RAM backs it.
 * TX to the vuart is never focus-gated, so the banner reaches the console
 * unconditionally.
 *
 * Run: SVM_BIN=.../svm-basic.bin SVM_BIN2=.../svm-vm1.bin make run
 *      (HV_GUEST=svm_dual build)
 */
#include "../svm_lib.h"

void SVM_ENTRY _start(void)
{
    svm_puts("SVM: hello from VM1\n");
    svm_done(svm_hvc(PSCI_VERSION, 0));
}
