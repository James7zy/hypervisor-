/* SPDX-License-Identifier: TBD */
/*
 * SVM case "basic" (M1 / M1.5): EL2 → EL1 entry and the HVC round trip.
 *
 * Issues HVC PSCI_VERSION (0x84000000), then reports the returned version
 * word back through HC_GUEST_DONE in x1. The hypervisor prints
 * "[hv] SVM HVC: done (x1=0x<version>)", which the test greps for.
 *
 * Also VM0 of the dual-VM and EL2-shell scenarios. Prints nothing itself.
 *
 * Run: SVM_BIN=build/svm/svm-basic.bin make run   (HV_GUEST=svm build)
 */
#include "../svm_lib.h"

void SVM_ENTRY _start(void)
{
    svm_done(svm_hvc(PSCI_VERSION, 0));
}
