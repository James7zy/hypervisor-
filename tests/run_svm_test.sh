#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: run hypervisor + SVM in QEMU, verify output.
# Called by: make test-qemu (after make all svm)
# Requires: SVM_BIN set by Makefile, qemu-system-aarch64 in PATH.
set -eu

: "${SVM_BIN:?must be set by make test-qemu}"

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
check "SVM: launching VMID="
check "SVM HVC: done (x1=0x10001)"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
