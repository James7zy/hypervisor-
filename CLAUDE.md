# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A learning/research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then Rockchip RK3588. Inspired by ACRN, Xvisor, and Xen. The directory layout mirrors ACRN's `hypervisor/` structure.

**Current milestone: M0 — Hello EL2** (entering EL2, printing banner over PL011 UART, halting).

## Build Commands

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make run                # invoke scripts/run-qemu.sh (QEMU)
make clean              # remove build/
```

Override defaults with: `ARCH=arm64 BOARD=qemu_virt CROSS_COMPILE=aarch64-none-linux-gnu-`

**Toolchain required:**
- `aarch64-none-linux-gnu-gcc` ≥ 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0

Exit QEMU with `Ctrl-A x`. GDB attach: `QEMU_EXTRA_ARGS="-s -S" make run`, then `aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.

## Verification (no CI, no test framework)

There is no automated test suite. Verification is:
1. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
2. **Static inspection**: `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf` — entry point must be `0x40080000`; `.text` section must start at `0x40080000`.
3. **Run + observe**: `make run` must print `[hv] Hello from EL2, CurrentEL=0x8` within 3 seconds.

## Architecture

### Layering

```
hypervisor/boot/main.c          ← arch-independent C entry (hypervisor_main)
    ↓ #include <board.h>        ← resolved via -I path ordering (no arch name in source)
hypervisor/arch/arm64/board/qemu_virt/board.h   ← BOARD_UART_BASE, BOARD_DRAM_BASE, board_name[]
hypervisor/arch/arm64/boot/head.S               ← _start, EL2 assert, BSS clear, VBAR, DAIF, → C
hypervisor/debug/uart_pl011.c   ← PL011 protocol driver (receives base via uart_init(base))
hypervisor/lib/print.c          ← minimal printk → uart_putc
hypervisor/lib/string.c         ← freestanding memset (memcpy added in M1)
hypervisor/arch/arm64/cpu/cpu.c ← read_currentel(), cpu_wfi(), cpu_relax()
```

The arch sub-Makefile passes `-Ihypervisor/arch/arm64/board/$(BOARD) -Ihypervisor/arch/arm64/include` so `#include <board.h>` in arch-independent code resolves to the active board's header without naming the board in source.

### Board vs Driver separation

`hypervisor/debug/uart_pl011.c` is the PL011 *protocol* driver — it never contains a hard-coded UART address. The board's `board.h` provides `BOARD_UART_BASE`. The only chartered exception is `head.S`'s pre-C early-panic path, where C is not yet available.

### Key invariants

- **`-mgeneral-regs-only` is mandatory**: M0 does not save FP/SIMD state. Never add code that forces the compiler to emit FP/SIMD instructions.
- **No magic numbers in `uart_pl011.c`**: driver receives base from `uart_init`.
- **printk supports only**: `%s %c %d %u %x %lx %%`. No width, precision, floats, or `%p`.
- **Empty directories use `.gitkeep`** to preserve the ACRN-style skeleton shape for future milestones.
- **`.config` is required**: `make` fails with an error if `.config` is absent — always run `make defconfig` first. The Makefile converts `CONFIG_FOO=y` lines to `-DCONFIG_FOO=1`.

### Compiler flags (all translation units)

```
-ffreestanding -nostdlib -nostartfiles
-fno-pic -fno-stack-protector
-mgeneral-regs-only -mstrict-align
-Wall -Wextra -Werror -O2 -g
```

### Boot sequence

```
QEMU → _start (head.S)
  1. Park non-boot CPUs (MPIDR_EL1.Aff0 != 0 → secondary_park / wfi)
  2. Assert CurrentEL == 0b1000; else → panic_early (emits "!EL\n" directly to PL011 DR)
  3. Set SP_EL2 = __stack_top
  4. Clear BSS (memset)
  5. VBAR_EL2 = hv_vectors (panic stubs)
  6. DAIF mask, dsb/isb
  7. bl hypervisor_main(dtb_phys)
     → uart_init(BOARD_UART_BASE)
     → printk("[hv] Hello from EL2, CurrentEL=0x%lx\n", read_currentel())
     → for(;;) cpu_wfi()
```

### Load address

`0x40080000` — QEMU `virt` default kernel load address, set in `hypervisor/arch/arm64/board/qemu_virt/linker.lds`.

## Milestone Roadmap

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | **current** | Enter EL2, print banner |
| M1 — Bare-metal guest | future | Stage-2 MMU, minimal vCPU |
| M2 — Multi-vCPU + interrupts | future | vGICv3, virtual timer, PSCI |
| M3 — Linux guest | future | Boot Linux to shell, virtio-console |
| M4 — RK3588 port | future | Run on real RK3588 hardware |

Each milestone gets its own spec in `docs/superpowers/specs/` and plan in `docs/superpowers/plans/`.

**Design principle:** every file written in M0 must leave room for M1–M4 to add code, but must not contain placeholder stubs for them.

## Reference Source Trees

The following production hypervisor source trees are available in the parent directory (`../`) for reference when making design or implementation decisions:

| Path | Project | Notes |
|------|---------|-------|
| `../acrn-hypervisor` | [ACRN](https://github.com/projectacrn/acrn-hypervisor) | Type-1, x86 + ARM64; primary structural inspiration for this repo's layout |
| `../xvisor` | [Xvisor](https://github.com/avpatel/xvisor-next) | Type-1, ARM-first; reference for Stage-2 MMU and vCPU design |
| `../hypervisor` | local Rust hypervisor | Rust-based hypervisor (NOT a copy of this project); reference for Rust idioms applied to bare-metal hypervisor design |

**When to consult these:** look up an existing implementation before designing any new subsystem (Stage-2 MMU, vGIC, PSCI, virtio, etc.). Prefer reading the smallest relevant file rather than loading entire trees.
