# SPDX-License-Identifier: TBD
# Device drivers this board's hardware needs (paths relative to hypervisor/).
board-drivers := \
    drivers/uart/pl011.o

# Physical core count: the single source for both the build (NR_CPUS) and
# QEMU's -smp (the Makefile exports it to scripts/run-qemu.sh as HV_NR_CPUS).
board-nr-cpus := 4

# Board facts the generic code needs as compile-time constants (C and asm).
# CONFIG_, not BOARD_: generic code reads it, and BOARD_* is arch-private
# (ADR-0015). It is not a .config option.
board-cflags := -DCONFIG_NR_CPUS=$(board-nr-cpus)
