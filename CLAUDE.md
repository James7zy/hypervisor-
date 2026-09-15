# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The architecture documentation should include sequence diagrams, class diagrams, and diagrams showing the 
relationships between modules, all represented using Mermaid. A picture is worth a thousand words.

## Project Overview

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then Rockchip RK3588. Inspired by ACRN, Xvisor, bao-hypervisor. The directory layout
mirrors ACRN's `hypervisor/` structure.

**M9 (SMP)** is complete: an unmodified Linux guest boots with two vCPUs
statically pinned 1:1 to two pCPUs. Next: **M10 (multi-VM foundation)** — the
long-term target form is the **full ACRN model** (Service VM + userspace Device
Model); see the roadmap below. The RK3588 port moved to M15, after the ACRN-model
core chain is proven on QEMU.

## Build Commands

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make test               # offset checks + M1/M3/M4 + dual-VM + EL2-shell QEMU scenarios
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
make clean              # remove build/
```

Boot constraints learned from the first real run (see `docs/debug/m3-boot-debug-walkthrough.md`):
- **`scripts/run-qemu.sh` uses `-m 2G` and this is required** — guest RAM is backed
  at the 1 GB-aligned PA `0x80000000` (so the Stage-2 L1 1 GB block can map it),
  which is only inside QEMU `virt` DRAM when more than 1 GB is present (ADR-0004).
- **The kernel `Image` must fit the address budget** (~31 MB: Image at PA `0x80080000`,
  DTB at `0x82000000`). A full arm64 `defconfig` Image is ~37 MB and overruns the
  DTB; trim unused subsystems (NET/PCI/USB/DRM/DEBUG_INFO, keep PL011 + virtio +
  devtmpfs + initramfs) to get under budget.
- **The Makefile does not track header dependencies** — after editing any header
  (e.g. `board.h`), run `make clean` or stale `.o`s relink with old values.

Override defaults with: `ARCH=arm64 BOARD=qemu_virt CROSS_COMPILE=aarch64-none-linux-gnu-`

**Toolchain required:**
- `aarch64-none-linux-gnu-gcc` ≥ 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0
- `dtc` (device-tree-compiler) — Debian/Ubuntu: `sudo apt-get install device-tree-compiler`

Exit QEMU with `Ctrl-A x`. GDB attach:
`LINUX_IMAGE=/path/to/Image QEMU_EXTRA_ARGS="-s -S" make run`, then
`aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.

## Verification (no CI; local automated integration tests)

1. **Automated suite**: `make test` builds a separate SVM-mode hypervisor under
   `build/test-svm/`, checks C/assembly struct offsets, and runs the M1, M3, and
   M4 QEMU integration scenarios, plus the dual-VM (`svm4`) and EL2-shell
   scenarios on the `build/test-svm-dual/` (`NR_VMS=2`) build.
   - `tests/run_shell_test.sh` is the only scenario that **writes** to the QEMU
     serial stdin (the rest run `</dev/null`). It drives `Ctrl-T`, `vm_list`,
     `help`, `vm_console`, the error paths, and backspace line editing. It
     needs `NR_VMS=2` but no guest OS, so it uses the bare-metal SVM guests and
     finishes in seconds instead of a ~25s Linux boot. Input timing is a fixed
     `sleep` before the first byte — bytes sent before EL2 enables PL011 RX are
     lost, so `BOOT_WAIT` in that script is deliberately generous.
2. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
3. **Linux SMP run**: boot with `LINUX_IMAGE` and optionally `LINUX_INITRD`, then
   verify CPU1 boots, `/sys/devices/system/cpu/online` reports `0-1`, and both CPU
   columns in `/proc/interrupts` have timer and IPI activity.

## Known defects (found, not yet fixed)

- **`CPU_OFF` kills the whole VM** (`hypervisor/common/psci/psci.c`, found
  2026-07-25): `PSCI_CPU_OFF` shares `psci_power_down()` with `SYSTEM_OFF`/
  `SYSTEM_RESET`, so it sets the per-VM `vm->off` flag and parks *every* pCPU of
  the VM. Per the PSCI spec `CPU_OFF` must stop only the calling vCPU. A guest
  doing `echo 0 > /sys/devices/system/cpu/cpu1/online` therefore takes its whole
  VM down. Fixing it needs per-vCPU off state (only per-VM exists today) plus a
  decision on what "last vCPU off" means; deliberately deferred because **M11
  rewrites this state management anyway**. Visible as `halted` in the EL2
  shell's `vm_list`.
- **`vm_console <n>` accepts a halted VM**: attaching focus to a powered-down VM
  silently does nothing (M10 known gap). `vm_list`'s STATE column now at least
  makes the cause visible before you attach.


### Board vs Driver separation

### Physical GIC vs vGIC separation (2026-09-08)

Interrupt code is split by ownership, in two directories that must not
merge back:

| Directory | Owns | Changes when |
|---|---|---|
| `arch/arm64/irq/` | `gic_v3.c` — the physical GICv3 the hypervisor owns; `irq_handler.c` — the EL2 dispatcher | the SoC changes (M15 RK3588) |
| `arch/arm64/vgic/` | `vgic.c` (ICH_LR injection), `vgic_v3_mmio.c` (GICD/GICR trap-and-emulate), `vgic_sgi.c` (SGI/IPI) | guest-visible interrupt semantics change (M11 scheduler) |

**The dependency is one-way: `vgic/` may call `gic_v3.h`; `irq/gic_v3.c` must
never know about VMs, vCPUs or the vGIC.** Enforced by review, not by the
build. Today `vgic/` reaches into the driver at exactly three call sites, all
through public `gic_*` functions — keep that list short and deliberate:

| Call site | Calls | Why |
|---|---|---|
| `vgic_v3_mmio.c` (guest enables its virtual PPI 27) | `gic_ppi_set_enable()` | rearm the physical vtimer PPI masked during early SMP bring-up |
| `vgic_sgi.c` (guest IPI to another vCPU) | `gic_kick_pcpu()` | force the target pCPU to EL2 to drain its SGI bitmap |
| `vgic.c` (remote SPI publication) | `gic_kick_pcpu()` | force the target pCPU to EL2 to reload its shadow LR1 |

Consequences worth knowing before editing:
- **Physical INTIDs stay out of `vgic/`.** Cross-core kicks go through
  `gic_kick_pcpu()`, which hides `BOARD_KICK_SGI`. Do not write
  `ICC_SGI1R_EL1` from outside `gic_v3.c`.
- **`irq_handler.c` is a dispatcher only** — ack, decide, inject, EOI. Device
  work does not belong there: PL011 console RX arbitration lives in
  `dm/console.c` (`console_rx_drain()`), which owns `console_focus` and the
  Ctrl-T/shell/vuart routing decision.
- **Kicking a VM's pCPUs for power-down is PSCI policy**, not vGIC work — it
  is `psci_kick_vm_other_pcpus()` in `common/psci/psci.c`.
- **Do not create a shared `gic_regs.h`.** Both directories describe the same
  ARM spec but take disjoint subsets for opposite purposes — `gic_v3.h` holds
  registers EL2 *writes*, `vgic_v3_mmio.h` holds the `VGICD_`/`VGICR_` offsets
  it *emulates*. Merging them produces a header neither side uses fully.
- `docs/superpowers/{specs,plans}/` and `docs/adr/` still cite the pre-split
  `irq/vgic*.c` paths on purpose; they are historical records.

### Key invariants

- **`-mgeneral-regs-only` is mandatory**: M0 does not save FP/SIMD state. Never add code that forces the compiler to emit FP/SIMD instructions.
- **No magic numbers in `uart_pl011.c`**: driver receives base from `uart_init`.
- **printk supports only**: `%s %c %d %u %x %lx %%`. No width, precision, floats, or `%p`.
- **Empty directories use `.gitkeep`** to preserve the ACRN-style skeleton shape for future milestones.
- **`.config` is required**: `make` fails with an error if `.config` is absent — always run `make defconfig` first. The Makefile converts `CONFIG_FOO=y` lines to `-DCONFIG_FOO=1`.
- **dtc does not concatenate adjacent string literals** (unlike C), so `guest/qemu_virt.dts` takes its whole `bootargs` value as one `-DHV_BOOTARGS='"..."'` string from the Makefile. The DTS is preprocessed with `$(CPP)` before `dtc`; `make guest` emits one DTB per VM, differing only in the `hv.vm=` token.
- **The guest initramfs `/init` lives at `guest/initramfs-init.sh`** (the `.cpio.gz` itself is outside the repo). It reads `hv.vm=` into `PS1` so the two VMs show `vm0:~ #` / `vm1:~ #` — without it both guests print an identical prompt on the shared console and `vm_console <n>` gives no visible feedback. The busybox it targets has only 11 applets and **no `hostname`**, hence `PS1` rather than a real hostname.

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

See the `roadmap` skill (`.claude/skills/roadmap/SKILL.md`) for the full M0-M15
milestone table, the 2026-08-10 renumbering map, and the ACRN-model strategy
decisions.

Each milestone gets its own spec in `docs/superpowers/specs/` and plan in `docs/superpowers/plans/`.

**Design principle:** My design philosophy is to move forward in small, fast iterations, 
breaking requirements down into the smallest practical units and defining them clearly. 
The goal is to achieve high cohesion and low coupling across the system.


## Reference Source Trees
The following production hypervisor source trees are available in the parent directory (`../`) for reference when making design or implementation decisions:

| Path | Project | Notes |
|------|---------|-------|
| `../acrn-hypervisor` | [ACRN](https://github.com/projectacrn/acrn-hypervisor) | Type-1, x86 ; primary structural inspiration for this repo's layout |
| `../xvisor` | [Xvisor](https://github.com/avpatel/xvisor-next) | Type-1, ARM-first; reference for Stage-2 MMU and vCPU design |
| `../bao-hypervisor` | local hypervisor | bao hypervisor  | 

**When to consult these:** look up an existing implementation before designing any new subsystem (Stage-2 MMU, vGIC, PSCI, virtio, etc.). 
Prefer reading the smallest relevant file rather than loading entire trees.

Import Reference
- [ARM Architecture Reference Manual (ARMv8-A)](https://developer.arm.com/documentation/ddi0487/latest)
- [ARM GIC Architecture Specification](https://developer.arm.com/documentation/ihi0069/latest)
- [pKVM (Protected KVM)](https://source.android.com/docs/core/virtualization)
