#!/bin/sh
# Guest-visible vSPI mapping/stress; serial focus is transport, not an assertion.
set -eu
: "${HYPERVISOR_ELF:?}" "${SVM_BIN:?}" "${SVM_BIN2:?}"
export HYPERVISOR_ELF SVM_BIN SVM_BIN2
case "${VSPI_MODE:-gate}" in
    gate) mode=G ;;
    *) echo 'unsupported VSPI_MODE' >&2; exit 1 ;;
esac
tmp=$(mktemp -d /tmp/vspi-run-XXXXXX)
log=${VSPI_LOG:-$tmp/qemu.log}
echo "VSPI log: $log"
qemu_pid=
cleanup() {
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null || :
        wait "$qemu_pid" 2>/dev/null || :
    fi
    exec 3>&-
    rm -f "$tmp/input"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
mkfifo "$tmp/input"
exec 3<>"$tmp/input"
LINUX_IMAGE= timeout 180 ./scripts/run-qemu.sh <"$tmp/input" >"$log" 2>&1 &
qemu_pid=$!
fail() { echo "VSPI FAIL: $*" >&2; cat "$log" >&2; exit 1; }
wait_marker() {
    wanted=$1
    i=0
    while [ "$i" -lt 300 ]; do
        if grep -q 'VSPI .* FAIL' "$log"; then fail 'guest failure'; fi
        kill -0 "$qemu_pid" 2>/dev/null || fail 'QEMU exited early'
        if tr -d '\r' <"$log" | grep -qxF "$wanted"; then return; fi
        i=$((i + 1)); sleep 0.05
    done
    fail "missing marker: $wanted"
}
wait_boot_online() {
    for pcpu in 1 2 3; do
        i=0
        while ! grep -qF "[hv] pCPU$pcpu online, entering guest" "$log"; do
            if grep -q 'VSPI .* FAIL' "$log"; then fail 'guest failure'; fi
            kill -0 "$qemu_pid" 2>/dev/null || fail 'QEMU exited early'
            [ "$i" -lt 300 ] || fail "missing pCPU$pcpu boot announcement"
            i=$((i + 1)); sleep 0.05
        done
    done
}
wait_prompt() {
    i=0
    while [ "$i" -lt 300 ]; do
        if grep -q 'VSPI .* FAIL' "$log"; then fail 'guest failure'; fi
        kill -0 "$qemu_pid" 2>/dev/null || fail 'QEMU exited early'
        if grep -qF 'hv> ' "$log"; then return; fi
        i=$((i + 1)); sleep 0.05
    done
    fail 'missing shell prompt'
}
wait_marker 'VSPI VM0 BOOT'
# Transport quiescence only; guest readiness/timers are checked separately.
wait_boot_online
for vm in 0 1; do
    if [ "$vm" -eq 1 ]; then
        printf '\024' >&3
        wait_prompt
        printf 'vm_console 1\r' >&3
        wait_marker '[hv] console: VM1'
    fi
    printf '%s' "$mode" >&3
    wait_marker "VSPI VM$vm ACTIVE"
    wait_marker "VSPI VM$vm GATE PASS"
    wait_marker "VSPI VM$vm DONE"
done
printf '\001x' >&3
if wait "$qemu_pid"; then qemu_pid=; else
    rc=$?; qemu_pid=; fail "QEMU status $rc"
fi
if grep -qE 'VSPI .* FAIL|ERROR|PANIC' "$log"; then fail 'error output'; fi
echo 'VSPI ALL PASS'
