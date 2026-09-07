#!/bin/sh
# Guest-visible vSPI RX/IPI/replay; serial focus is transport, not an assertion.
set -eu
: "${HYPERVISOR_ELF:?}" "${SVM_BIN:?}" "${SVM_BIN2:?}"
export HYPERVISOR_ELF SVM_BIN SVM_BIN2
case "${VSPI_MODE:-regression}" in
    gate) mode=G ;;
    stress) mode=S ;;
    regression) mode=R ;;
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
# timeout owns this run's process group; TERM in cleanup also arms its
# two-second KILL grace period. No global QEMU-name matching or unbounded reap.
LINUX_IMAGE= timeout -k 2 180 ./scripts/run-qemu.sh <"$tmp/input" >"$log" 2>&1 &
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
    if [ "$mode" != G ]; then
        b=0
        while [ "$b" -lt 128 ]; do
            wait_marker "VSPI VM$vm RX READY $b"
            payload=$(awk -v b="$b" -v vm="$vm" 'BEGIN {
                for (i=0; i<32; i++) printf "%c", 97+((b*32+i+vm*7)%26)
            }')
            chunk=0
            while [ "$chunk" -lt 8 ]; do
                first=$((chunk * 4 + 1)); last=$((first + 3))
                printf '%s' "$payload" | cut -c "$first-$last" | tr -d '\n' >&3
                sleep 0.002
                chunk=$((chunk + 1))
            done
            wait_marker "VSPI VM$vm RX PASS $b"
            b=$((b + 1))
        done
        wait_marker "VSPI VM$vm STRESS PASS"
    fi
    if [ "$mode" = R ]; then
        wait_marker "VSPI VM$vm DRAINED"
        k=1
        while [ "$k" -le 16 ]; do
            wait_marker "VSPI VM$vm IPI PASS $k"
            wait_marker "VSPI VM$vm QUIET PASS $k"
            k=$((k + 1))
        done
        wait_marker "VSPI VM$vm REPLAY PASS"
    fi
    wait_marker "VSPI VM$vm DONE"
done
printf '\001x' >&3
if wait "$qemu_pid"; then qemu_pid=; else
    rc=$?; qemu_pid=; fail "QEMU status $rc"
fi
if grep -qE 'VSPI .* FAIL|ERROR|PANIC' "$log"; then fail 'error output'; fi
echo 'VSPI ALL PASS'
