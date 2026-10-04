# SPDX-License-Identifier: TBD
# Device drivers this board's hardware needs (paths relative to hypervisor/).
board-drivers := \
    drivers/uart/pl011.o

# Board facts the generic code needs as compile-time constants (C and asm).
# CONFIG_NR_CPUS: physical cores; must match -smp in scripts/run-qemu.sh
# (not checked). CONFIG_, not BOARD_: generic code reads it, and BOARD_* is
# arch-private (ADR-0015). It is not a .config option.
board-cflags := -DCONFIG_NR_CPUS=4
