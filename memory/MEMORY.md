# Memory Index

- [Toolchain setup](toolchain-setup.md) — `source ~/Downloads/toolchain.sh` before any `make`; toolchain not on default PATH
- [M3 execution plan](m3-execution-plan.md) — M3.1→M3.4 sequential subagents, tag per phase, no Linux Image (run = operator handoff)
- [M3 never booted in QEMU](m3-never-booted-in-qemu.md) — M3 "done" was static-only; first real run (2026-06-19) found Stage-2 bug (fixed) + an open early-boot abort
- [Toolchain caveat: no header deps](toolchain-no-header-deps.md) — `make` tracks no `.d` files; header-only edits need `make clean`
