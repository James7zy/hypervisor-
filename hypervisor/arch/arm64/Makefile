# SPDX-License-Identifier: TBD
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o \
    arch/arm64/vmexit/vmexit.o \
    arch/arm64/vmexit/vmexit_asm.o \
    arch/arm64/irq/vgic.o \
    arch/arm64/irq/gic_v3.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include \
    -Ihypervisor/arch/arm64/mmu \
    -Ihypervisor/arch/arm64/irq

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
