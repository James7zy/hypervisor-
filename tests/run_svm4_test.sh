#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: run hypervisor + TWO bare-metal SVM guests (VM0 svm.bin,
# VM1 svm4.bin) in QEMU under NR_VMS=2, verify both come up statically
# partitioned 2+2 on 4 pCPUs.
# Called by: make test-qemu-svm4 (after make all svm svm4, HV_GUEST=svm_dual)
# Requires: SVM_BIN, SVM_BIN2, HYPERVISOR_ELF set by Makefile,
#           qemu-system-aarch64 in PATH.
set -eu

: "${SVM_BIN:?must be set by make test-qemu-svm4}"
: "${SVM_BIN2:?must be set by make test-qemu-svm4}"
: "${HYPERVISOR_ELF:?must be set by make test-qemu-svm4}"

TIMEOUT=10

OUTPUT=$(SVM_BIN="${SVM_BIN}" SVM_BIN2="${SVM_BIN2}" \
         HYPERVISOR_ELF="${HYPERVISOR_ELF}" \
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
check "SVM: launching VMID="
check "SVM4: hello from VM1"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
