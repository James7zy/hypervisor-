---
name: toolchain-no-header-deps
description: The Makefile tracks no header dependencies; header-only edits need `make clean`
metadata:
  type: project
---

The repo Makefile compiles `.c → .o` with **no `-MMD`/`.d` dependency files**, so
editing a header (e.g. `board.h`, `vm.h`) does **not** trigger recompilation of
the `.o` files that include it. `make` will relink stale objects and silently
keep old constant values.

Observed 2026-06-19: changing `BOARD_LINUX_RAM_PA` in `board.h` had no effect
until `make clean` — the banner kept printing the old `ram_pa`.

**Why:** can cause confusing "my fix didn't take" debugging loops.
**How to apply:** after any header-only change, run `make clean && make`. See
[[toolchain-setup]] and [[m3-never-booted-in-qemu]].
