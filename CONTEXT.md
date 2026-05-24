# Hypervisor — Domain Glossary

## Board

A platform-specific configuration unit. Lives under `arch/<arch>/board/<name>/`. Provides the hardware constants (UART base address, clock frequency, DRAM base) and the linker script for one target machine. A Board is not a Driver — it supplies addresses, not protocol logic.

## Driver

A protocol-level hardware driver. Receives a base address at runtime via an `init` function; never contains a hard-coded board address. Example: `uart_pl011.c` implements the PL011 register protocol; the Board supplies the address it receives.

## board.h resolution (shadow model)

When arch-independent code writes `#include <board.h>`, the compiler resolves it directly to the active Board's `board.h` because the board directory is listed first in the `-I` search order. The file `arch/arm64/include/board.h` is an empty placeholder and is never reached by this lookup.
