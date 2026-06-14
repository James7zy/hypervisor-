#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: run hypervisor + M2.5 timer SVM in QEMU, verify the
# virtual-timer PPI is taken at EL2 and hardware-forwarded to the guest.
# Called by: make test-qemu-svm3 (after make all svm3)
# Requires: SVM_BIN set by Makefile, qemu-system-aarch64 in PATH.
set -eu

: "${SVM_BIN:?must be set by make test-qemu-svm3}"

TIMEOUT=10

OUTPUT=$(SVM_BIN="${SVM_BIN}" \
         timeout "${TIMEOUT}" ./scripts/run-qemu.sh </dev/null 2>&1 || true)

FAILURES=0
check() {
    if echo "${OUTPUT}" | grep -qF "$1"; then
        printf "PASS: '%s'\n" "$1"
    else
        printf "FAIL: '%s' not found in output\n" "$1"
        FAILURES=$((FAILURES + 1))
    fi
}

check "Hello from EL2"
check "[hv] GIC: initialized (dist=0x8000000 rdist=0x80a0000 PPI=27)"
check "[hv] vtimer: CNTHCTL_EL2=0x3, CNTVOFF_EL2=0x0"
check "SVM: launching VMID="
check "[svm] EL1 init"
check "[svm] GIC EL1 configured"
check "[svm] virtual timer armed"
check "[svm] virtual timer IRQ received (INTID=27)"
check "[svm] signalling HVC done"
check "SVM HVC: done"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
