#!/bin/sh
set -eu

# M3.0: boot an unmodified arm64 Linux Image as the guest.
#   LINUX_IMAGE — path to a prebuilt arm64 'Image' (required)
#   GUEST_DTB   — path to the compiled guest DTB (default: build/guest/guest.dtb)
#
# Physical load addresses MUST match the hv address map (board.h):
#   Image @ 0x48080000  (guest IPA 0x40080000)
#   DTB   @ 0x4A000000  (guest IPA 0x42000000)
: "${LINUX_IMAGE:?LINUX_IMAGE must point to a prebuilt arm64 Image, e.g.: LINUX_IMAGE=/path/to/Image make run}"
GUEST_DTB="${GUEST_DTB:-build/guest/guest.dtb}"

[ -f "$GUEST_DTB" ] || { echo "ERROR: $GUEST_DTB not found. Run 'make guest' first."; exit 1; }

exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  -device loader,file="${LINUX_IMAGE}",addr=0x48080000 \
  -device loader,file="${GUEST_DTB}",addr=0x4A000000 \
  ${QEMU_EXTRA_ARGS:-}
