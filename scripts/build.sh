#!/bin/sh
# SPDX-License-Identifier: TBD
set -eu

# One-stop wrapper around the Makefile's Linux-style flow:
#
#   export ARCH=... CROSS_COMPILE=...; make <board>_defconfig; make
#
# It sets the toolchain environment, (re)generates .config when it does not
# match the requested arch/board, then runs the requested command. It adds no
# build logic of its own; everything below is a plain `make` invocation.

usage() {
  cat <<'EOF'
Usage: scripts/build.sh [options] [command]

Commands:
  build    configure if needed, then make            (default)
  guest    build the per-VM guest DTBs (needs dtc)
  run      boot two Linux VMs in QEMU (needs LINUX_IMAGE; LINUX_INITRD optional)
  test     make test: boundary, offsets, QEMU scenarios, x86 stub link check
  x86      link-check the x86 stub only (build/test-x86; never run)
  clean    remove build/
  help     show this text

Options:
  -a ARCH  target architecture: arm64 (default) or x86      [env: ARCH]
  -b BOARD board name, e.g. qemu_virt; default per arch      [env: BOARD]
  -c       make clean first (the Makefile does not track header deps)
  -j N     parallel jobs for build/guest
  -h       show this text

Environment:
  CROSS_COMPILE  toolchain prefix (default: aarch64-none-linux-gnu- for arm64,
                 x86_64-linux-gnu- for x86)
  TOOLCHAIN_BIN  optional directory to prepend to PATH, so the cross compiler
                 can be found without editing your shell profile
  LINUX_IMAGE / LINUX_INITRD   guest kernel and initramfs for `run`

Examples:
  TOOLCHAIN_BIN=$HOME/toolchain/bin scripts/build.sh
  scripts/build.sh -c build
  LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz scripts/build.sh run
  scripts/build.sh test
  scripts/build.sh x86
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

ARCH="${ARCH:-arm64}"
BOARD="${BOARD:-}"
CLEAN=0
JOBS=""

while getopts "a:b:cj:h" opt; do
  case "$opt" in
    a) ARCH="$OPTARG" ;;
    b) BOARD="$OPTARG" ;;
    c) CLEAN=1 ;;
    j) JOBS="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
CMD="${1:-build}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

case "$CMD" in
  help) usage; exit 0 ;;
  build|guest|run|test|x86|clean) ;;
  *) usage >&2; die "unknown command '$CMD'" ;;
esac

case "$ARCH" in
  arm64) : "${BOARD:=qemu_virt}"; : "${CROSS_COMPILE:=aarch64-none-linux-gnu-}" ;;
  x86)   : "${BOARD:=stub}";      : "${CROSS_COMPILE:=x86_64-linux-gnu-}" ;;
  *)     die "unsupported ARCH '$ARCH' (arm64 or x86)" ;;
esac

if [ -n "${TOOLCHAIN_BIN:-}" ]; then
  [ -d "$TOOLCHAIN_BIN" ] || die "TOOLCHAIN_BIN=$TOOLCHAIN_BIN is not a directory."
  PATH="$TOOLCHAIN_BIN:$PATH"
fi
export ARCH CROSS_COMPILE PATH

MAKE_J=""
[ -n "$JOBS" ] && MAKE_J="-j$JOBS"

# `clean` and `x86` need no .config; the x86 stub supplies its own.
if [ "$CMD" = "clean" ]; then
  exec make clean
fi

command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || {
  X86_CC=x86_64-linux-gnu-gcc
  if [ "$CMD" = "x86" ] && command -v "$X86_CC" >/dev/null 2>&1; then
    :   # the x86 stub target uses its own compiler, not CROSS_COMPILE
  else
    die "'${CROSS_COMPILE}gcc' not found in PATH.
       Put the toolchain on PATH, or run with TOOLCHAIN_BIN=<dir>, or set CROSS_COMPILE."
  fi
}

if [ "$CMD" = "x86" ]; then
  [ "$CLEAN" -eq 0 ] || rm -rf build/test-x86
  exec make test-x86-stub
fi

# Regenerate .config only when it targets a different arch/board, so manual
# edits (e.g. CONFIG_DEBUG_VGIC=y) survive repeated builds.
UP_ARCH=$(printf '%s' "$ARCH" | tr 'a-z' 'A-Z')
UP_BOARD=$(printf '%s' "$BOARD" | tr 'a-z' 'A-Z')
if [ -f .config ] \
   && grep -qx "CONFIG_ARCH_${UP_ARCH}=y" .config \
   && grep -qx "CONFIG_BOARD_${UP_BOARD}=y" .config; then
  echo "[build.sh] .config already targets ARCH=$ARCH BOARD=$BOARD"
else
  echo "[build.sh] configuring: make ${BOARD}_defconfig (ARCH=$ARCH)"
  make "${BOARD}_defconfig"
fi

[ "$CLEAN" -eq 0 ] || make clean

case "$CMD" in
  build)
    ELF=build/hypervisor.elf
    # `make -q` exits 0 only when everything is already up to date.
    if make -q >/dev/null 2>&1; then
      echo "[build.sh] up to date, nothing rebuilt: $ELF"
      # The Makefile does not track header dependencies, so "up to date" can be
      # stale after a header edit; point that out instead of hiding it.
      STALE=$(find hypervisor -name '*.h' -newer "$ELF" 2>/dev/null | head -3)
      if [ -n "$STALE" ]; then
        echo "[build.sh] WARNING: headers newer than the build (not tracked by make):"
        echo "$STALE" | sed 's/^/             /'
        echo "           run 'scripts/build.sh -c build' to rebuild from scratch."
      else
        echo "           (use -c for a clean rebuild)"
      fi
    else
      # shellcheck disable=SC2086  # MAKE_J is intentionally unquoted (empty or -jN)
      make $MAKE_J
      echo "[build.sh] built $ELF ($(wc -c < "$ELF") bytes) and build/hypervisor.bin"
    fi
    ;;
  guest)
    # shellcheck disable=SC2086
    make $MAKE_J guest
    ;;
  run)
    [ "$ARCH" = "arm64" ] || die "'run' only supports ARCH=arm64."
    [ -n "${LINUX_IMAGE:-}" ] || die "set LINUX_IMAGE=/path/to/Image (LINUX_INITRD optional)."
    [ -f "$LINUX_IMAGE" ] || die "LINUX_IMAGE=$LINUX_IMAGE not found."
    exec make run
    ;;
  test)
    [ "$ARCH" = "arm64" ] || die "'test' only supports ARCH=arm64 (it runs QEMU)."
    exec make test
    ;;
esac
