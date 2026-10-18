# SPDX-License-Identifier: TBD
# Link-check board for the x86 stub. PL011 is portable MMIO C and only
# satisfies the uart_* symbols the generic code links against; a real x86
# board would select a 16550 driver instead.
board-drivers := \
    drivers/uart/pl011.o

board-nr-cpus := 4

# Same contract as every board (see arm64 qemu_virt/board.mk).
board-cflags := -DCONFIG_NR_CPUS=$(board-nr-cpus)
