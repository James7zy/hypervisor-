#!/bin/sh
set -eu

# Boot either a bare-metal SVM test guest or an unmodified arm64 Linux Image.
# Exactly one of SVM_BIN and LINUX_IMAGE must be set.
#   SVM_BIN      — path to a bare-metal SVM binary (tests)
#   LINUX_IMAGE  — path to a prebuilt arm64 'Image' (normal run)
#   HYPERVISOR_ELF — hypervisor image (default: build/hypervisor.elf)
#   GUEST_DTB    — path to the compiled guest DTB (default: build/guest/guest.dtb)
#   LINUX_INITRD — (M3.4, OPTIONAL) path to a busybox cpio.gz initramfs.
#                  If unset, no initramfs is loaded (M3.0-M3.3 behaviour).
#
# Physical load addresses MUST match the hv address map (board.h) and the
# guest .dts (guest IPA = PA - 0x40000000 for the RAM region; RAM_PA is
# 1 GB-aligned so the Stage-2 L1 1 GB block can map it):
#   Image   @ PA 0x80080000  (guest IPA 0x40080000)          VM0
#   DTB     @ PA 0x82000000  (guest IPA 0x42000000)          VM0
#   initrd  @ PA 0x84000000  (guest IPA 0x44000000)  <- must match dts linux,initrd-start (VM0)
#   Image   @ PA 0xC0080000  (guest IPA 0x40080000, VM1)     -- SAME Image file, reloaded
#   DTB     @ PA 0xC2000000  (guest IPA 0x42000000, VM1)     <- guest-vm1.dtb (hv.vm=1)
#   initrd  @ PA 0xC4000000  (guest IPA 0x44000000, VM1)     -- SAME initrd file, reloaded
# VM1's backing block requires DRAM extending through 0x100000000 (-m 4G).
HYPERVISOR_ELF="${HYPERVISOR_ELF:-build/hypervisor.elf}"

if [ -n "${SVM_BIN:-}" ] && [ -n "${LINUX_IMAGE:-}" ]; then
  echo "ERROR: set only one of SVM_BIN or LINUX_IMAGE."
  exit 1
elif [ -n "${SVM_BIN:-}" ]; then
  [ -f "$SVM_BIN" ] || { echo "ERROR: SVM_BIN=$SVM_BIN not found."; exit 1; }
  set -- -device "loader,file=${SVM_BIN},addr=0x40200000"
  if [ -n "${SVM_BIN2:-}" ]; then
    [ -f "$SVM_BIN2" ] || { echo "ERROR: SVM_BIN2=$SVM_BIN2 not found."; exit 1; }
    set -- "$@" -device "loader,file=${SVM_BIN2},addr=0xC0200000"
  fi
elif [ -n "${LINUX_IMAGE:-}" ]; then
  GUEST_DTB="${GUEST_DTB:-build/guest/guest.dtb}"
  # VM1 gets its OWN DTB: identical to VM0's except for the hv.vm= bootargs
  # token that gives each guest a distinct hostname. Same Image and initramfs
  # are still reloaded for both.
  GUEST_DTB1="${GUEST_DTB1:-build/guest/guest-vm1.dtb}"
  [ -f "$LINUX_IMAGE" ] || { echo "ERROR: LINUX_IMAGE=$LINUX_IMAGE not found."; exit 1; }
  [ -f "$GUEST_DTB" ] || { echo "ERROR: $GUEST_DTB not found. Run 'make guest' first."; exit 1; }
  [ -f "$GUEST_DTB1" ] || { echo "ERROR: $GUEST_DTB1 not found. Run 'make guest' first."; exit 1; }

  set -- -device "loader,file=${LINUX_IMAGE},addr=0x80080000" \
          -device "loader,file=${GUEST_DTB},addr=0x82000000" \
          -device "loader,file=${LINUX_IMAGE},addr=0xC0080000" \
          -device "loader,file=${GUEST_DTB1},addr=0xC2000000"
  if [ -n "${LINUX_INITRD:-}" ]; then
    [ -f "$LINUX_INITRD" ] || { echo "ERROR: LINUX_INITRD=$LINUX_INITRD not found."; exit 1; }
    set -- "$@" -device "loader,file=${LINUX_INITRD},addr=0x84000000" \
                -device "loader,file=${LINUX_INITRD},addr=0xC4000000"
  fi
else
  echo "ERROR: set SVM_BIN for a test guest or LINUX_IMAGE for Linux."
  exit 1
fi

[ -f "$HYPERVISOR_ELF" ] || { echo "ERROR: HYPERVISOR_ELF=$HYPERVISOR_ELF not found."; exit 1; }

exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 4 -m 4G \
  `# -smp 4 (M5 slice 3): QEMU must create pCPU0..3 -- 2 statically pinned to` \
  `# VM0, 2 to VM1 -- for the hypervisor to PSCI CPU_ON each VM's pCPUs. Each` \
  `# guest's own DTB only ever describes ITS 2 vCPUs (cpu@0/cpu@1); a VM never` \
  `# sees the other VM's physical cores.` \
  `# -m 4G is REQUIRED: VM0's RAM is backed at PA 0x80000000 and VM1's at PA` \
  `# 0xC0000000 (both 1 GB-aligned so the Stage-2 L1 1 GB block can map them).` \
  `# QEMU virt RAM starts at 0x40000000, so VM1's block (through 0x100000000)` \
  `# falls inside DRAM only when >=3 GB beyond the base is present; -m 2G ends` \
  `# RAM at 0x80000000, before VM1's block even starts.` \
  -nographic -serial mon:stdio \
  -kernel "${HYPERVISOR_ELF}" \
  "$@" \
  ${QEMU_EXTRA_ARGS:-}
