#!/bin/sh
set -eu

# Boot an unmodified arm64 Linux Image as the guest.
#   LINUX_IMAGE  — path to a prebuilt arm64 'Image' (required)
#   GUEST_DTB    — path to the compiled guest DTB (default: build/guest/guest.dtb)
#   LINUX_INITRD — (M3.4, OPTIONAL) path to a busybox cpio.gz initramfs.
#                  If unset, no initramfs is loaded (M3.0-M3.3 behaviour).
#
# Physical load addresses MUST match the hv address map (board.h) and the
# guest .dts (guest IPA = PA - 0x08000000 for the RAM region):
#   Image   @ PA 0x48080000  (guest IPA 0x40080000)
#   DTB     @ PA 0x4A000000  (guest IPA 0x42000000)
#   initrd  @ PA 0x4C000000  (guest IPA 0x44000000)  <- must match dts linux,initrd-start
: "${LINUX_IMAGE:?LINUX_IMAGE must point to a prebuilt arm64 Image, e.g.: LINUX_IMAGE=/path/to/Image make run}"
GUEST_DTB="${GUEST_DTB:-build/guest/guest.dtb}"

[ -f "$GUEST_DTB" ] || { echo "ERROR: $GUEST_DTB not found. Run 'make guest' first."; exit 1; }

# Optional initramfs loader (M3.4). Built as a shell variable so the qemu line
# stays a single exec; empty when LINUX_INITRD is unset.
INITRD_LOADER=""
if [ -n "${LINUX_INITRD:-}" ]; then
  [ -f "$LINUX_INITRD" ] || { echo "ERROR: LINUX_INITRD=$LINUX_INITRD not found."; exit 1; }
  INITRD_LOADER="-device loader,file=${LINUX_INITRD},addr=0x4C000000"
fi

exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  -device loader,file="${LINUX_IMAGE}",addr=0x48080000 \
  -device loader,file="${GUEST_DTB}",addr=0x4A000000 \
  ${INITRD_LOADER} \
  ${QEMU_EXTRA_ARGS:-}
