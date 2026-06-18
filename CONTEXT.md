# Hypervisor — Domain Glossary
# ARM64 Hypervisor Context

## Goal

This project implements a minimal ARM64 Type-1 hypervisor capable of booting a Linux guest on QEMU virt and later on real ARM64 hardware.

## Domain Terms

- EL2: ARM exception level used by the hypervisor.
- EL1: guest kernel execution level.
- EL0: guest user-space execution level.
- VCPU: virtual CPU state owned by the hypervisor.
- Stage-2 translation: guest physical address to host physical address translation.
- Trap: an exception from guest EL1/EL0 into EL2.
- VM exit: transition from guest execution to hypervisor.
- VM entry: transition from hypervisor to guest execution.
- HCR_EL2: hypervisor control register.
- VTTBR_EL2: root pointer for stage-2 translation tables.
- SPSR_EL2 / ELR_EL2: guest return state.
- GIC: ARM interrupt controller.
- PSCI: firmware interface normally used by Linux for CPU and power management.

## Non-goals for MVP

- No live migration.
- No overcommit.
- No nested virtualization.
- No full device virtualization.
- No SMP guest at first.
- No performance optimization before Linux guest reaches init.

## Board

A platform-specific configuration unit. Lives under `arch/<arch>/board/<name>/`. Provides the hardware constants (UART base address, clock frequency, DRAM base) and the linker script for one target machine. A Board is not a Driver — it supplies addresses, not protocol logic.

## Driver

A protocol-level hardware driver. Receives a base address at runtime via an `init` function; never contains a hard-coded board address. Example: `uart_pl011.c` implements the PL011 register protocol; the Board supplies the address it receives.

## board.h resolution (shadow model)

When arch-independent code writes `#include <board.h>`, the compiler resolves it directly to the active Board's `board.h` because the board directory is listed first in the `-I` search order. The file `arch/arm64/include/board.h` is an empty placeholder and is never reached by this lookup.
