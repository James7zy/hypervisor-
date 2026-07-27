#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: drive the EL2 shell (hypervisor/dm/hv_shell.c) over the
# physical console and check every command's output.
#
# Called by: make test-qemu-shell (after make all svm svm4, HV_GUEST=svm_dual)
# Requires: SVM_BIN, SVM_BIN2, HYPERVISOR_ELF set by Makefile,
#           qemu-system-aarch64 in PATH.
#
# Why the SVM dual profile and not Linux: this needs NR_VMS=2 so vm_list has
# two rows and vm_console 1 has somewhere to go, but it does NOT need a real
# guest OS -- the shell runs entirely in EL2. The SVM guests reach their
# steady state in ~2s versus ~25s for a Linux boot, and print a fixed number
# of lines, which keeps this test both fast and deterministic.
#
# Unlike the other scenarios (which run </dev/null), this one WRITES to the
# QEMU serial stdin. run-qemu.sh uses -serial mon:stdio, so bytes on our
# stdout reach the guest UART and land in el2_irq_handler's PL011 RX drain.
set -eu

: "${SVM_BIN:?must be set by make test-qemu-shell}"
: "${SVM_BIN2:?must be set by make test-qemu-shell}"
: "${HYPERVISOR_ELF:?must be set by make test-qemu-shell}"

# Ctrl-T, the shell toggle (HV_SHELL_ESCAPE_KEY in hypervisor/dm/hv_shell.h).
ESC=$(printf '\024')
BS=$(printf '\010')
TIMEOUT=25

# Input must not start until the guests are up, otherwise the bytes are sent
# before EL2 has enabled PL011 RX and are simply lost. BOOT_WAIT is generous
# next to the ~2s the SVM guests actually take.
BOOT_WAIT=6
# Between commands: the shell processes a line inside the RX IRQ handler, so
# this only needs to cover QEMU scheduling, not any real work.
STEP=1

drive_shell() {
    sleep "${BOOT_WAIT}"
    printf '%s' "${ESC}"          # enter the shell
    sleep "${STEP}"
    printf 'vm_list\r'            # the table, focus on VM0
    sleep "${STEP}"
    printf 'help\r'               # every command listed
    sleep "${STEP}"
    printf 'bogus\r'              # unknown command
    sleep "${STEP}"
    printf 'vm_console 9\r'       # out-of-range id names the valid range
    sleep "${STEP}"
    printf '   \r'                # blank line is a no-op, just reprompts
    sleep "${STEP}"
    printf 'vm_console\r'         # missing argument is rejected, not parsed as 0
    sleep "${STEP}"
    # Backspace editing: type "vm_consoXX", erase both X's, finish the line.
    # Proves the line buffer, not just the echo, drops the erased bytes.
    printf 'vm_consoXX%s%sle 1\r' "${BS}" "${BS}"
    sleep "${STEP}"
    # A successful attach must leave the HV shell immediately. This probe is
    # therefore routed to VM1's vuart and must not be echoed/interpreted by
    # the HV shell (the bare-metal VM deliberately does not consume it).
    printf 'post_attach_probe\r'
    sleep "${STEP}"
    printf '%s' "${ESC}"          # re-enter the shell from VM1
    sleep "${STEP}"
    printf 'vm_list\r'            # focus moved to VM1
    sleep "${STEP}"
}

OUTPUT=$(drive_shell | SVM_BIN="${SVM_BIN}" SVM_BIN2="${SVM_BIN2}" \
         HYPERVISOR_ELF="${HYPERVISOR_ELF}" \
         timeout "${TIMEOUT}" ./scripts/run-qemu.sh 2>&1 || true)

FAILURES=0
check() {
    if echo "${OUTPUT}" | grep -qF "$1"; then
        printf "PASS: '%s'\n" "$1"
    else
        printf "FAIL: '%s' not found in output\n" "$1"
        FAILURES=$((FAILURES + 1))
    fi
}

# Absence checks matter as much as presence here: a shell that echoed the
# escape key or leaked bytes to a guest would still pass the greps above.
check_absent() {
    if echo "${OUTPUT}" | grep -qF "$1"; then
        printf "FAIL: '%s' should NOT appear in output\n" "$1"
        FAILURES=$((FAILURES + 1))
    else
        printf "PASS: absent as expected: '%s'\n" "$1"
    fi
}

# Both guests still reached their steady state -- the shell must not have
# disturbed them.
check "Hello from EL2"
check "SVM4: hello from VM1"

# Ctrl-T printed a prompt.
check "hv> "

# vm_list: header plus one row per VM, with the pCPU split from vm_config.
check "ID  pCPUs  STATE   CONSOLE"
check "   0    0-1  on      *"
check "   1    2-3  on"

# help lists all three commands.
check "help	list commands"
check "vm_list	list VMs, their pCPUs, state and console focus"
check "vm_console	vm_console <n> - attach console input to VM n"

# Error paths.
check "Error: Invalid command."
check "Error: invalid VM id (valid: 0-1)."

# Backspace left the buffer holding exactly "vm_console 1", so focus moved
# and the command immediately handed the physical console to VM1.
check "[hv] console: VM1"
check_absent "post_attach_probe"
# Ctrl-T re-entered the shell, and the second vm_list shows VM1 focused.
check "   1    2-3  on      *"

# The escape key is consumed by the drain loop, never echoed and never
# delivered to a guest (hv_shell.h's contract).
check_absent "hv> hv> hv>"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
