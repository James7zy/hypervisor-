# SPDX-License-Identifier: TBD
# x86 is a link-check stub (ADR-0015): it proves the generic code compiles for
# a second architecture and that every *_arch_* hook has an implementation.
# It is built by `make test-x86-stub` and never run.
arch-objs := \
    arch/x86/stub/hooks.o \
    arch/x86/board/$(BOARD)/board.o

arch-includes := \
    -Ihypervisor/arch/x86/board/$(BOARD) \
    -Ihypervisor/arch/x86/include

# Kernel-style codegen: no FP/SSE (same rule as arm64), no red zone (an
# interrupt on the hypervisor stack would clobber it), no PIE (host gcc
# defaults to it; we link with a fixed address).
arch-cflags := -mgeneral-regs-only -mno-red-zone -fno-pie

arch-ldscript := hypervisor/arch/x86/board/$(BOARD)/linker.lds

include hypervisor/arch/x86/board/$(BOARD)/board.mk
