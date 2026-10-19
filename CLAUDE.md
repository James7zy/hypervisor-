# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The architecture documentation should include sequence diagrams, class diagrams, and diagrams showing the 
relationships between modules, all represented using Mermaid. A picture is worth a thousand words.

## 本地 Superpowers skills

若 `.claude/skills/using-superpowers/SKILL.md` 存在，会话开始先用 `Skill` 加载
`using-superpowers`。本工程使用本地 skills，不使用 Claude 插件；上游文档里的
`superpowers:<name>` 对应本地 `<name>`（例如 `brainstorming`），不要调用插件命名空间。
安装与更新见 [操作说明](docs/reference/superpowers.md)；本工程的架构与验证约束优先。

## Project Overview

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then Rockchip RK3588. Inspired by ACRN, Xvisor, bao-hypervisor. The directory layout
mirrors ACRN's `hypervisor/` structure.

**M10 (multi-VM foundation)** is complete: two Linux VMs, each with two
vCPUs statically pinned 1:1 to its own pCPUs (2+2 on 4), with an EL2-owned
console and shell. Next: **M11 (vCPU scheduler)**. The long-term target form
is the **full ACRN model** (Service VM + userspace Device Model); see the
roadmap below. The RK3588 port moved to M15, after the ACRN-model
core chain is proven on QEMU.

## Build Commands

```sh
make qemu_virt_defconfig   # copy arch/arm64/configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make test               # arch boundary + host/target offsets + five QEMU scenarios
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
make clean              # remove build/
```

Boot constraints learned from the first real run (see `docs/debug/m3-boot-debug-walkthrough.md`):
- **`scripts/run-qemu.sh` uses `-m 4G` and this is required** — VM0's RAM is
  backed at the 1 GB-aligned PA `0x80000000` and VM1's at `0xC0000000` (so the
  Stage-2 L1 1 GB block can map each), and VM1's block ends at `0x100000000`,
  inside QEMU `virt` DRAM only with ≥3 GB beyond its `0x40000000` base
  (ADR-0004). Its `-smp` is not hard-coded: it is the board's
  `board-nr-cpus` (`board.mk`), exported by the Makefile as `HV_NR_CPUS`, so
  run the script through `make run` / `make test`, not directly.
- **The kernel `Image` must fit the address budget** (~31 MB: Image at PA `0x80080000`,
  DTB at `0x82000000`). A full arm64 `defconfig` Image is ~37 MB and overruns the
  DTB; trim unused subsystems (NET/PCI/USB/DRM/DEBUG_INFO, keep PL011 + virtio +
  devtmpfs + initramfs) to get under budget.
  A known-good config (4.7 MB Image, Linux 6.12, two VMs boot to shells) is
  checked in as `guest/linux-6.12-qemu_virt.config` — a **full** `.config`,
  not a savedefconfig: it starts from `allnoconfig`, so a defconfig-style diff
  would re-enable defaults on restore. Use it with
  `cp guest/linux-6.12-qemu_virt.config <linux>/.config && make olddefconfig && make Image`
  (ARCH/CROSS_COMPILE set).
- **The Makefile does not track header dependencies** — after editing any header
  (e.g. `board.h`), run `make clean` or stale `.o`s relink with old values.

Board selection works like Linux. `ARCH` (default `arm64`; `x86` is the link-check
stub) and `CROSS_COMPILE` are plain environment/make variables; the board is chosen by
`make <board>_defconfig`, which copies `hypervisor/arch/$(ARCH)/configs/<board>_defconfig`
to `.config`:

```sh
export ARCH=arm64                               # default; ARCH=x86 for the stub
export CROSS_COMPILE=aarch64-none-linux-gnu-    # compiler must be on PATH
make qemu_virt_defconfig                        # ARCH=x86 make stub_defconfig
make
```

The board comes from `CONFIG_BOARD_*` in `.config`, which must have been generated for
the same `ARCH` (`CONFIG_ARCH_*`); a mismatch is an error that tells you to re-run
`make <board>_defconfig`. The Makefile has no built-in toolchain path: put the
cross-compiler on `PATH` yourself. `make help` lists the boards of the current `ARCH`.
`scripts/build.sh [-a arch] [-b board] [-c] [build|guest|run|test|x86|clean]` wraps this flow
(sets `ARCH`/`CROSS_COMPILE`, accepts `TOOLCHAIN_BIN=<dir>` to extend `PATH`, regenerates
`.config` only when it targets a different arch/board); see `scripts/build.sh -h`.

**Toolchain required:**
- `aarch64-none-linux-gnu-gcc` ≥ 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0
- `dtc` (device-tree-compiler) — Debian/Ubuntu: `sudo apt-get install device-tree-compiler`
- `perl` — `scripts/check-arch-boundary.sh` (part of `make test`) uses it to blank comments and strings

Exit QEMU with `Ctrl-A x`. GDB attach:
`LINUX_IMAGE=/path/to/Image QEMU_EXTRA_ARGS="-s -S" make run`, then
`aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.

## Verification (no CI; local automated integration tests)

1. **Automated suite**: `make test` checks the arch boundary (including
   transitive headers), compiles and links the generic code for x86_64
   against the `arch/x86` stub (`test-x86-stub`), builds a separate SVM-mode hypervisor under
   `build/test-svm/`, checks host/target C/assembly struct offsets, and runs the `basic`
   (HVC/PSCI round trip) and `vtimer` (HW-forwarded PPI 27) QEMU scenarios,
   plus the dual-VM (`test-qemu-dual`), EL2-shell and vSPI scenarios on
   `NR_VMS=2` builds.
   - **All bare-metal SVM guests live in `tests/svm/`**: one shared runtime
     (`svm_lib.c`, `svm_vectors.S`, `svm.lds`) and one file per scenario in
     `cases/<case>.c`, built to `build/.../svm/svm-<case>.bin`. Expected output
     lines live in `expect/<scenario>.txt`, checked by the single runner
     `tests/run_svm_test.sh`. A new scenario is a new case file + expect file
     + one Makefile target — **do not add another `tests/svmN/` directory**.
     (`tests/vspi/` predates this and is still separate.)
   - `tests/run_shell_test.sh` and `tests/run_vspi_test.sh` both **write** to QEMU
     serial stdin; the shared SVM runner uses `</dev/null`. The shell test drives `Ctrl-T`, `vm_list`,
     `help`, `vm_console`, the error paths, and backspace line editing. It
     needs `NR_VMS=2` but no guest OS, so it uses the bare-metal SVM guests and
     finishes in seconds instead of a ~25s Linux boot. Input timing is a fixed
     `sleep` before the first byte — bytes sent before EL2 enables PL011 RX are
     lost, so `BOOT_WAIT` in that script is deliberately generous.
2. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
3. **Linux SMP run**: boot with `LINUX_IMAGE` and optionally `LINUX_INITRD`, then
   verify CPU1 boots, `/sys/devices/system/cpu/online` reports `0-1`, and both CPU
   columns in `/proc/interrupts` have timer and IPI activity.

**"Done" means observed running.** A milestone is marked done only after its
DoD was actually run in QEMU and the result seen. Build-clean + `readelf`
checks alone make it **static-only** — say so explicitly. M3 was once marked
done on static checks and its first real run found three bugs
(`docs/debug/m3-boot-verification.md`).

## Where knowledge lives

Test: *would it still hold on another machine, for another person?*
- **Yes** → the repo: debugging lessons in `docs/debug/`, decisions in
  `docs/adr/`, standing rules in `CLAUDE.md`/`AGENTS.md`.
- **No** (local paths, personal workflow) → the agent's private memory under
  `~/.claude/`, never checked in. There is no in-repo `memory/` directory;
  do not recreate one.
- **New docs are written in Chinese** (code identifiers and technical terms
  stay English); never keep the same content in two languages. Category,
  language and file-naming rules: [docs/README.md](docs/README.md).

## Known defects (found, not yet fixed)

- **`CPU_OFF` kills the whole VM** (`hypervisor/arch/arm64/vmexit/vpsci.c`, found
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


## Architecture

Read [current-architecture.md](docs/reference/arm/current-architecture.md) for
current module boundaries, object ownership and boot/console sequences.
[target-architecture.md](docs/reference/arm/target-architecture.md) describes
the future ACRN model, not implemented functionality.

### Board vs Driver separation

`arch/arm64/board/<board>/board.mk` selects `board-drivers` and supplies
`board-nr-cpus` to both the C/asm build and QEMU. Runtime board data reaches
common code through `include/board_info.h`; PL011 lives in `drivers/uart/`
and receives its base through `uart_init()`.

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
  is `psci_kick_vm_other_pcpus()` in `arch/arm64/vmexit/vpsci.c`.
- **Do not create a shared `gic_regs.h`.** Both directories describe the same
  ARM spec but take disjoint subsets for opposite purposes — `gic_v3.h` holds
  registers EL2 *writes*, `vgic_v3_mmio.h` holds the `VGICD_`/`VGICR_` offsets
  it *emulates*. Merging them produces a header neither side uses fully.
- `docs/superpowers/{specs,plans}/` and `docs/adr/` still cite the pre-split
  `irq/vgic*.c` paths on purpose; they are historical records.

### Arch boundary: core vs arch/ (2026-09-29)

Code **outside `hypervisor/arch/`** (`common/`, `boot/`, `dm/`, `drivers/`,
`debug/`, `lib/`, `include/`) must reach the architecture only through
`<object>_arch_<verb>` hooks (`cpu_`, `vm_`, `vcpu_`) and `<arch/xxx.h>`
headers. Forbidden outside `arch/`: inline asm and `.S` files, system register
names (`*_ELn`, `ICH_*`, `ICC_*`), PSCI, direct `vgic_*`/`stage2_*`/`gic_*`/
`vtimer_*` calls, `BOARD_*`/`board.h` (except the static VM config table),
and include paths into a concrete `arch/<name>/` directory.
Decision and rejected options: [ADR-0015](docs/adr/0015-arch-boundary-core-vs-arch.md);
migration plan: `docs/superpowers/specs/2026-09-29-arch-boundary-design.md`.

- **Enforced by `make test`** (`scripts/check-arch-boundary.sh`), with zero
  tolerance: the 2026-09-29 migration emptied its allowlist and the allowlist
  was deleted. It greps comment-stripped source *and* preprocesses each
  non-arch `.c`, so an arch-private header reached through another header
  (e.g. `board.h` via a config header) fails too. A hit means the code belongs
  in `arch/` behind a hook; the only exemption (`common/vm/vm_configs.c`, the
  static VM config table, may read `BOARD_*`) is hard-coded in the script.
- Hooks today: `include/cpu.h` (`cpu_arch_init/halt/power_on`),
  `include/percpu.h` (`cpu_arch_this_percpu/set_this_percpu`),
  `include/spinlock.h` (`atomic_arch_fetch_inc_u32`, `cpu_arch_relax`),
  `include/vm.h` (`vm_arch_init/devices_init`,
  `vcpu_arch_reset/load/run/inject_irq`). arm64 implements them in
  `arch/arm64/{cpu,guest}/`, `vmexit/vmexit_asm.S` and `<arch/*.h>`.
  `arch/x86/` is a **link-check stub** (empty hooks in `stub/hooks.c`, never
  run): a new hook without an x86 stub fails `make test-x86-stub` at link
  time. Arch-only codegen flags (`-mstrict-align` etc.) live in each arch
  Makefile's `arch-cflags`.
  Board data reaches common code
  through `include/board_info.h`; compile-time board constants (e.g.
  `CONFIG_NR_CPUS`, also needed by asm) come from `board-cflags` in the
  board's `board.mk`; the MMIO bus is `common/io/`.
- Hook prototypes live in the **generic** header; every arch implements them;
  **no weak defaults**. Add a hook only when a real call site needs it.
- Device-specific is not arch-specific: an emulated PL011 stays in `dm/`, it
  just injects via a hook instead of calling the vGIC.
- The physical GIC / vGIC split above sits *inside* this boundary.

### Key invariants

- **`-mgeneral-regs-only` is mandatory**: the current static-pinning implementation still does not save FP/SIMD state. Never add code that forces the compiler to emit FP/SIMD instructions.
- **No magic numbers in `drivers/uart/pl011.c`**: driver receives base from `uart_init`. Which drivers a board links is listed in its `board.mk` (`board-drivers`).
- **printk supports only**: `%s %c %d %u %x %lx %%`. No width, precision, floats, or `%p`.
- **Empty directories use `.gitkeep`** to preserve the ACRN-style skeleton shape for future milestones.
- **`.config` is required**: `make` fails with an error if `.config` is absent — always run `make <board>_defconfig` first. The Makefile converts `CONFIG_FOO=y` lines to `-DCONFIG_FOO=1`.
- **dtc does not concatenate adjacent string literals** (unlike C), so `guest/qemu_virt.dts` takes its whole `bootargs` value as one `-DHV_BOOTARGS='"..."'` string from the Makefile. The DTS is preprocessed with `$(CPP)` before `dtc`; `make guest` emits one DTB per VM, differing only in the `hv.vm=` token.
- **The guest initramfs `/init` lives at `guest/initramfs-init.sh`** (the `.cpio.gz` itself is outside the repo). It reads `hv.vm=` into `PS1` so the two VMs show `vm0:~ #` / `vm1:~ #` — without it both guests print an identical prompt on the shared console and `vm_console <n>` gives no visible feedback. The busybox it targets has only 11 applets and **no `hostname`**, hence `PS1` rather than a real hostname.

### Compiler flags (all translation units)

```
-ffreestanding -nostdlib -nostartfiles
-fno-pic -fno-stack-protector -fno-strict-aliasing
-mgeneral-regs-only -mstrict-align
-Wall -Wextra -Werror -O2 -g
```

### Boot sequence

```
QEMU → _start (arch/arm64/boot/head.S, boot pCPU0 only)
  → park unexpected non-boot entrants at boot_core_park
  → check CurrentEL, boot stack, clear BSS, install hv_vectors, mask DAIF
  → hypervisor_main(dtb_phys) (boot/main.c; DTB currently unused)
    → uart_init(board_uart_base)
    → cpu_arch_init(): banner + physical GIC + vtimer
    → vm_init(): per-VM arch init + boot-vCPU reset; register vGIC/vuart MMIO
    → vm_run(): set percpu[0] + current pointer; vcpu_arch_load(VM0 vCPU0)
      → enable physical UART RX IRQ; cpu_arch_power_on(other VMs' boot pCPUs)
      → loop: vcpu_arch_run(VM0 vCPU0)

Physical CPU_ON → secondary_entry → secondary_main
  → per-pCPU stack/current pointer/vectors, GIC/timer init
  → select VM/vCPU, publish online, vcpu_arch_load, loop: vcpu_arch_run
  VM1 vCPU0 is HV-started; each VM's vCPU1 is guest-PSCI-started.
  Normal sync/IRQ handling erets directly; timer IRQ does not return to the C loop.
```

### Load address

`0x40080000` — QEMU `virt` default kernel load address, set in `hypervisor/arch/arm64/board/qemu_virt/linker.lds`.

## Milestone Roadmap

See [AGENTS.md](AGENTS.md#milestone-roadmap) for the full M0–M15 table,
renumbering map and ACRN-model strategy. The former local `roadmap` skill is
not tracked. For M11 work, also read the
[arch-boundary alignment](docs/superpowers/specs/2026-10-04-m11-arch-boundary-alignment.md)
alongside the original scheduler spec; the scheduler is still unimplemented.

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
