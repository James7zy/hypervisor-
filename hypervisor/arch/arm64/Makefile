# SPDX-License-Identifier: TBD
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/cpu/secondary.o \
    arch/arm64/cpu/psci.o \
    arch/arm64/guest/vm.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o \
    arch/arm64/vmexit/vmexit.o \
    arch/arm64/vmexit/vmexit_asm.o \
    arch/arm64/vmexit/mmio.o \
    arch/arm64/vmexit/vpsci.o \
    arch/arm64/vgic/vgic.o \
    arch/arm64/vgic/vgic_sgi.o \
    arch/arm64/vgic/vgic_v3_mmio.o \
    arch/arm64/irq/gic_v3.o \
    arch/arm64/irq/irq_handler.o \
    arch/arm64/irq/irq_handler_asm.o \
    arch/arm64/timer/vtimer.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include \
    -Ihypervisor/arch/arm64/mmu \
    -Ihypervisor/arch/arm64/irq \
    -Ihypervisor/arch/arm64/vgic \
    -Ihypervisor/arch/arm64/vmexit \
    -Ihypervisor/arch/arm64/timer

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
