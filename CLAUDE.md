# CLAUDE.md

Guidance for Claude Code (claude.ai/code) in this repository.

Architecture docs must use Mermaid: sequence, class and module-relationship diagrams.

## 本地 Superpowers skills

若 `.claude/skills/using-superpowers/SKILL.md` 存在，会话开始先用 `Skill` 加载
`using-superpowers`。本工程使用本地 skills，不使用 Claude 插件；上游文档里的
`superpowers:<name>` 对应本地 `<name>`，不要调用插件命名空间。安装与更新见
[操作说明](docs/reference/superpowers.md)；本工程的架构与验证约束优先。

## Project

A research Type-1 ARM64 hypervisor on QEMU `virt` (AArch64), later RK3588. Inspired by
ACRN, Xvisor and bao; the directory layout mirrors ACRN's `hypervisor/`.

**M10 is done**: two Linux VMs, two vCPUs each, statically pinned 1:1 to pCPUs (2+2 on 4),
EL2-owned console and shell. **Next: M11 (vCPU scheduler)**; the long-term target is the
full ACRN model (Service VM + userspace Device Model). Roadmap: [AGENTS.md](AGENTS.md#milestone-roadmap).
For M11 also read the
[arch-boundary alignment](docs/superpowers/specs/2026-10-04-m11-arch-boundary-alignment.md).

**Design principle:** small, fast iterations; break work into the smallest practical,
clearly defined units; high cohesion, low coupling. Each milestone gets a spec in
`docs/superpowers/specs/` and a plan in `docs/superpowers/plans/`.

## Build

```sh
export PATH=<toolchain>/bin:$PATH                         # the Makefile has no toolchain path
export ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-   # ARCH defaults to arm64; x86 = link-check stub
make qemu_virt_defconfig                                  # board → .config (`make help` lists boards)
make                                                      # build/hypervisor.elf + .bin
make test                                                 # see Verification
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run   # Ctrl-A x exits
```

`scripts/build.sh -h` wraps this flow. `.config` is required and must match `ARCH`.
Needs `aarch64-none-linux-gnu` gcc/binutils ≥ 10, `qemu-system-aarch64` ≥ 6.0, `dtc`, `perl`.
GDB: `QEMU_EXTRA_ARGS="-s -S" make run`, then `target remote :1234`.

Gotchas (history: `docs/debug/m3-boot-debug-walkthrough.md`):
- **Run QEMU only through `make run` / `make test`**: `-smp` comes from the board's
  `board-nr-cpus`, exported as `HV_NR_CPUS`. **`-m 4G` is required**: VM0 RAM is at PA
  `0x80000000`, VM1 at `0xC0000000` (1 GB-aligned for Stage-2 L1 blocks, ADR-0004).
- **The kernel `Image` must be ≲ 31 MB** (Image at PA `0x80080000`, DTB at `0x82000000`); a full
  arm64 `defconfig` is ~37 MB. Use `guest/linux-6.12-qemu_virt.config` (a **full** `.config`, not a
  savedefconfig): `cp` it to the kernel tree, `make olddefconfig`, `make Image`.
- **No header dependency tracking**: after editing a header, `make clean`.

## Verification (no CI; local tests)

1. `make test` runs, in order: arch-boundary check, x86 stub link check, host/target struct
   offsets, then QEMU scenarios `basic` (HVC/PSCI), `vtimer`, `dual`, EL2 `shell`, `vspi`.
2. `make` must build with **zero warnings** (`-Werror`).
3. Linux SMP: boot with `LINUX_IMAGE`; check `/sys/devices/system/cpu/online` is `0-1` and both
   CPU columns of `/proc/interrupts` show timer and IPI counts, in both VMs.

Test rules:
- Bare-metal guests live in `tests/svm/`. A new scenario = `cases/<case>.c` + `expect/<scenario>.txt`
  + one Makefile target; **do not add another `tests/svmN/` directory**.
- The shell and vSPI tests write QEMU serial stdin; `BOOT_WAIT` is deliberately generous because
  bytes sent before EL2 enables PL011 RX are lost.

**"Done" means observed running.** A milestone is done only after its DoD was run in QEMU and the
result seen. Build-clean + `readelf` alone is **static-only**; say so (M3 was once marked done that
way and its first real run found three bugs: `docs/debug/m3-boot-verification.md`).

## Where knowledge lives

Would it still hold on another machine, for another person?
- **Yes** → the repo: lessons in `docs/debug/`, decisions in `docs/adr/`, standing rules here and in `AGENTS.md`.
- **No** (local paths, personal workflow) → private memory under `~/.claude/`. No in-repo `memory/`.
- **New docs are written in Chinese** (identifiers and technical terms stay English); never keep the
  same content in two languages. Rules: [docs/README.md](docs/README.md).

## Known defects (not fixed; M11 rewrites this state)

- **`CPU_OFF` kills the whole VM** (`arch/arm64/vmexit/vpsci.c`): it shares `psci_power_down()`
  with `SYSTEM_OFF`, sets per-VM `vm->off` and parks every pCPU. PSCI says it must stop only the
  caller. Shown as `halted` in the shell's `vm_list`.
- **`vm_console <n>` accepts a halted VM** and silently does nothing.

## Architecture

Current modules, objects and boot/console sequences:
[current-architecture.md](docs/reference/arm/current-architecture.md).
[target-architecture.md](docs/reference/arm/target-architecture.md) is the future ACRN model, not implemented.

### Arch boundary: core vs arch/ (2026-09-29)

Code **outside `hypervisor/arch/`** reaches the architecture only through `<object>_arch_<verb>`
hooks (`cpu_`, `vm_`, `vcpu_`) and `<arch/xxx.h>` headers. Forbidden outside `arch/`: inline asm and
`.S`, system register names (`*_ELn`, `ICH_*`, `ICC_*`), PSCI, direct `vgic_*`/`stage2_*`/`gic_*`/
`vtimer_*` calls, `BOARD_*`/`board.h` (only `common/vm/vm_configs.c` may read `BOARD_*`), and include
paths into a concrete `arch/<name>/`. Decision: [ADR-0015](docs/adr/0015-arch-boundary-core-vs-arch.md).

- **Enforced by `make test`** (`scripts/check-arch-boundary.sh`), zero tolerance, including headers
  reached transitively. A hit means the code belongs in `arch/` behind a hook.
- **Hooks**: prototypes in the generic headers (`include/{cpu,percpu,spinlock,vm}.h`), implemented by
  every arch, **no weak defaults**. Add a hook only for a real call site. `arch/x86/` is a link-check
  stub (`stub/hooks.c`, never run): a hook without an x86 stub fails `make test-x86-stub`.
- **One public header directory, `hypervisor/include/`.** Module-private headers are included with
  quotes from their own directory; `-I` has no `dm/` or `common/vm/`.
- **Board data**: `arch/<arch>/board/<board>/board.mk` gives `board-drivers`, `board-nr-cpus`,
  `board-cflags` (e.g. `-DCONFIG_NR_CPUS`); runtime data reaches common code via `include/board_info.h`.
- **Device-specific is not arch-specific**: an emulated PL011 stays in `dm/` and injects via a hook.

### Physical GIC vs vGIC separation (2026-09-08)

| Directory | Owns |
|---|---|
| `arch/arm64/irq/` | `gic_v3.c` (the physical GICv3), `irq_handler.c` (EL2 dispatcher) |
| `arch/arm64/vgic/` | `vgic.c` (ICH_LR injection), `vgic_v3_mmio.c` (GICD/GICR emulation), `vgic_sgi.c` (IPI) |

**One-way dependency: `vgic/` may call public `gic_*` functions; `irq/gic_v3.c` must never know about
VMs, vCPUs or the vGIC.** Enforced by review. Today `vgic/` calls only `gic_ppi_set_enable()` and
`gic_kick_pcpu()` (3 call sites); keep that list short.
- Physical INTIDs stay out of `vgic/`; cross-core kicks go through `gic_kick_pcpu()`. Do not write
  `ICC_SGI1R_EL1` outside `gic_v3.c`.
- `irq_handler.c` only dispatches (ack, decide, inject, EOI). Console RX arbitration is
  `dm/console.c` (`console_rx_drain()`).
- Kicking a VM's other pCPUs on power-down is PSCI policy (`psci_kick_vm_other_pcpus()` in `vpsci.c`).
- Do not create a shared `gic_regs.h`: `gic_v3.h` has registers EL2 *writes*, `vgic_v3_mmio.h` the
  offsets it *emulates*.
- `docs/superpowers/` and `docs/adr/` cite pre-split paths on purpose; they are historical.

### Key invariants

- **`-mgeneral-regs-only` is mandatory**: FP/SIMD state is not saved. Never emit FP/SIMD instructions.
- **printk supports only** `%s %c %d %u %x %lx %%`: no width, precision, floats or `%p`.
- **`drivers/uart/pl011.c` has no magic addresses**: the base comes from `uart_init()`.
- **Empty directories keep a `.gitkeep`** to preserve the ACRN-style skeleton.
- **dtc does not concatenate adjacent string literals**: `guest/qemu_virt.dts` takes its whole `bootargs`
  as one `-DHV_BOOTARGS='"..."'` from the Makefile; `make guest` emits one DTB per VM, differing only
  in `hv.vm=`.
- **Guest `/init` is `guest/initramfs-init.sh`** (the `.cpio.gz` is outside the repo). It turns `hv.vm=`
  into `PS1` (`vm0:~ #` / `vm1:~ #`); the busybox has 11 applets and no `hostname`.

## Reference source trees (in `../`)

`../acrn-hypervisor` (x86; primary structural inspiration), `../xvisor` (ARM-first; Stage-2 and vCPU
design), `../bao-hypervisor`. Consult the smallest relevant file before designing a subsystem (Stage-2,
vGIC, PSCI, virtio…). Specs: [ARM ARM](https://developer.arm.com/documentation/ddi0487/latest),
[GIC](https://developer.arm.com/documentation/ihi0069/latest),
[pKVM](https://source.android.com/docs/core/virtualization).
