# Repository Guidelines

## Project Structure & Module Organization

This is a freestanding ARM64 Type-1 hypervisor for QEMU `virt`. Core C and assembly live under `hypervisor/`: shared VM and PSCI code is in `common/`, architecture-specific code in `arch/arm64/`, interfaces in `include/`, and early boot in `boot/`. Board defaults belong in `configs/`, guest device trees in `guest/`, and launch helpers in `scripts/`. Tests are under `tests/`, including host layout checks and bare-metal SVM guests. Generated files belong only in `build/`. Record decisions in `docs/adr/` and designs or investigations in the appropriate `docs/` subtree.

## Build, Test, and Development Commands

- `make defconfig`: create `.config` from `configs/qemu_virt_defconfig`; run this before other build targets.
- `make`: cross-compile `build/hypervisor.elf` and `build/hypervisor.bin` with warnings treated as errors.
- `make test`: run struct-offset checks and all QEMU SVM integration tests.
- `make test-qemu-svm3`: run only the timer/GIC integration scenario while iterating.
- `LINUX_IMAGE=/path/to/Image make run`: build the guest DTB and boot a Linux guest in QEMU. Exit with `Ctrl-A x`.
- `make clean`: remove generated output. Run it after changing headers because the Makefile does not track header dependencies.

Required tools are an `aarch64-none-linux-gnu-` GCC/binutils toolchain, QEMU AArch64, and `dtc`. Override settings with `CROSS_COMPILE=...`, `ARCH=arm64`, and `BOARD=qemu_virt`.

## Coding Style & Naming Conventions

Match nearby code: four-space C indentation, braces on function-definition lines, and aligned operands in `.S` files. Use `snake_case` for functions and variables, lowercase underscore-separated filenames, and uppercase macros and constants. Keep code freestanding: do not introduce libc assumptions, floating point, or SIMD; `-mgeneral-regs-only` is an invariant. No formatter is configured, so preserve local layout and comment on hardware constraints or non-obvious control flow.

## Testing Guidelines

Add host checks as `tests/check_*.c` and QEMU scenarios as `tests/run_*_test.sh` with matching guest sources under `tests/<scenario>/`. Tests should fail on a nonzero exit and assert stable serial-output markers. Before submitting, run `make clean && make test`; for Linux-facing changes, also boot with `make run` and report the observed prompt or banner.

## Commit & Pull Request Guidelines

Recent history uses short imperative subjects with a scope, for example `feat(vgic): add ...`, `docs: update ...`, or milestone slices such as `m3.5(slice2): ...`. Keep each commit focused. Pull requests should explain the hardware-visible behavior, link the relevant issue/spec/ADR, list exact verification commands, and include serial-log excerpts for boot or interrupt changes. Call out changes to assembly/C layout, memory maps, or configuration defaults explicitly.
