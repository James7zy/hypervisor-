#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: boot the hypervisor with one or two bare-metal SVM guests
# in QEMU and check that every line of an expect file appears in the output
# (fixed-string match, order-independent).
#
# Usage: sh tests/run_svm_test.sh tests/svm/expect/<scenario>.txt
# Called by: make test-qemu / test-qemu-vtimer / test-qemu-dual
# Requires: SVM_BIN (and SVM_BIN2 for dual), HYPERVISOR_ELF set by Makefile,
#           qemu-system-aarch64 in PATH.
set -eu

EXPECT=${1:?usage: run_svm_test.sh <expect-file>}
: "${SVM_BIN:?must be set by make}"
[ -f "${EXPECT}" ] || { echo "ERROR: ${EXPECT} not found."; exit 1; }

TIMEOUT=10

OUTPUT=$(timeout "${TIMEOUT}" ./scripts/run-qemu.sh </dev/null 2>&1 || true)

FAILURES=0
while IFS= read -r line; do
    [ -n "${line}" ] || continue
    if printf '%s\n' "${OUTPUT}" | grep -qF -- "${line}"; then
        printf "PASS: '%s'\n" "${line}"
    else
        printf "FAIL: '%s' not found in output\n" "${line}"
        FAILURES=$((FAILURES + 1))
    fi
done < "${EXPECT}"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
