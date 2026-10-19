# CLAUDE.md

Guidance for Claude Code (claude.ai/code) in this repository.

Architecture docs must use Mermaid: sequence, class and module-relationship diagrams.

## Ponytail：最小完整改动

参考 [Ponytail 的 AGENTS.md](https://github.com/DietrichGebert/ponytail/blob/b088b2df6e08d4306c6a3c3d575fe38c2d2d2989/AGENTS.md)
（[MIT](https://github.com/DietrichGebert/ponytail/blob/b088b2df6e08d4306c6a3c3d575fe38c2d2d2989/LICENSE)），按本工程特点整合以下常驻工作原则。
适用于编码、修复、重构和评审；**本工程的架构边界、已接受的 ADR、硬件正确性和验证要求优先，不能以精简为由削弱。**

### 动手前确定影响范围

- 先读任务和相关代码，列出必须同步修改的调用方、测试、fixtures、配置和导出接口；检查是否会破坏现有调用、丢失数据或暴露数据。
- 修复 bug 前先查找受影响函数的所有调用方，沿实际执行路径定位根因，再在共同的实现处修复。

### 按顺序选择最小可行方案

1. 确认需求确实需要；范围模糊时先做满足核心需求的最小版本，说明未纳入的额外功能。
2. 优先复用仓库已有的实现、接口和模式，遵守其调用约定与分层。
3. 再考虑标准库或平台能力，但必须适用于本工程的 freestanding/EL2 环境，不能假定存在 libc 或操作系统服务。
4. 再考虑已经引入的依赖，不为几行逻辑增加一个新依赖。
5. 能用一行清楚表达就用一行；不能则写最少且可读的完整实现，不做代码压缩游戏。

### 完整性与收尾

- 改动保持在任务范围内，但必须补齐被它影响的调用方、测试和配置；不添加无人要求的抽象、包装、转换、选项或“以后可能用到”的代码。
- 保留现有架构与数据表示；规模相当的方案优先选择边界条件正确的那个。注释解释代码本身不能表达的原因，保持简短。
- 移动、合并或精简代码时保留原有校验与错误处理；信任边界、安全、可访问性、防止数据丢失的处理、硬件所需的校准/屏障/状态管理和用户明确要求均不能删减。
- 新增非平凡逻辑（分支、循环、解析、安全或状态更新等）应留下一个能验证行为的小测试或自检；它不能替代下文规定的构建、回归和 QEMU 验收。
- 有已知限制的临时简化，用 `ponytail:` 注释写明适用上限和重新设计的触发条件，避免“以后再说”没有边界。
- 回复结尾用一两行说明未做或未验证的部分，以及用户需要知道的风险；未实际运行的检查不能写成已通过。

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
full ACRN model (Service VM + userspace Device Model). Roadmap: [Milestone Roadmap](#milestone-roadmap).
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
- **Yes** → the repo: lessons in `docs/debug/`, decisions in `docs/adr/`, standing rules here (`AGENTS.md` is a symlink to this file).
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

## Milestone Roadmap

The arch-boundary refactor is complete; M11 scheduling is not. When starting M11,
read the [alignment note](docs/superpowers/specs/2026-10-04-m11-arch-boundary-alignment.md)
together with the original scheduler design rather than restoring its old paths.

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | **done** | Enter EL2, print banner |
| M1 — Bare-metal guest | **done** | Stage-2 MMU, minimal vCPU |
| M2 — PSCI | **done** | PSCI VERSION/FEATURES/CPU_OFF/SYSTEM_OFF over HVC (no GIC) |
| M3 — vGIC software injection | **done** | HVC → `vgic_inject_sw` → guest EL1 IRQ handler (no physical HW) |
| M4 — Physical timer + GIC + HW-forwarding | **done** | timer PPI → EL2 → `vgic_inject_hw` → guest (ADR-0001) |
| M5 — Linux alive (no interrupts) | **done** | Load `Image` + DTB, arm64 boot protocol, PL011 passthrough earlycon; stalls at first GIC MMIO |
| M6 — MMIO trap framework | **done** | Stage-2 data-abort decode + MMIO trap-and-emulate dispatch |
| M7 — vGICv3 emulation | **done** | GICD/GICR(cpu0) trap-and-emulate on the M6 bus; timer-PPI injection |
| M8 — Boot to shell | **done (boot-verified 2026-06-19)** | initramfs load + DTB initrd nodes → interactive busybox shell prompt (headline M5 goal: UP Linux boots to a busybox shell). Confirmed on a real QEMU run: Linux 6.12.93 reaches `~ #` and runs `ls`/`echo`/`uname` over the ttyAMA0 PL011 passthrough |
| M9 — SMP | **done (boot-verified 2026-06-27; reverified 2026-07-13)** | 2-vCPU Linux, PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization, static 1:1 pinning (no scheduler) |
| M10 — Multi-VM foundation | **done (gate-verified 2026-07-20)** | VM objectification (`g_vm` → `vm[NR_VMS]`): per-VM Stage-2/VMID, per-VM vGIC, static 2+2 CPU partitioning on 4 pCPUs. Memory: QEMU `-m 4G`, VM1 RAM backed at PA `0xC0000000`; both VMs see the identical guest address map (RAM IPA `0x40000000` — the IPA≠PA Stage-2 mechanism is live since M5), so one DTB template and one load-address scheme serve both VMs. Console: EL2 owns the physical PL011 exclusively (revoked the M5 passthrough), both VMs get trap-and-emulate vuarts (`hypervisor/dm/vuart.c`), Ctrl-T switches RX focus (TX is focus-independent). VM-scoped PSCI power-down: `SYSTEM_OFF` in one VM parks only that VM's pCPUs. Landed as three slices, each independently spec- and quality-reviewed, plus a final whole-milestone review; see [[docs/adr/0014-multi-vm-static-partition-el2-console]]. Automated regression: `test-qemu-dual` with shared guests in `tests/svm/`. Known gaps carried into M11+: no automated test for PSCI-off isolation (verified manually only) — the Ctrl-T/`vm_console` focus gap was closed 2026-07-25 by `tests/run_shell_test.sh`; console focus left on a since-shut-down VM is a silent no-op, not fed back to the user (now at least visible as `halted` in `vm_list`); the pre-M10 single-VM overview remains a historical snapshot; current diagrams are in `docs/reference/arm/current-architecture.md` |
| M11 — vCPU scheduler | planned (moved up from M14, 2026-07-19) | Full context switch (incl. FP/SIMD state — lifts the `-mgeneral-regs-only` no-save assumption), time slicing, vCPU count > pCPU count. Deliverable: M10's 2 VMs × 2 vCPUs time-sliced on 2 pCPUs (the other 2 pCPUs left idle for later Service VM work); both guests reach shells and concurrent FP workloads in both VMs run uncorrupted |
| M12 — Hypercall ABI + VM lifecycle | planned | HVC hypercall namespace (distinct from PSCI), VM create/start/pause/destroy, Service VM privilege concept. Deliverable: Service VM controls User VM start/stop via hypercalls |
| M13 — HSM kernel driver + io_req ring | planned | Custom Linux kernel module in the Service VM (modeled on `acrn_hsm`): ioctl interface, io_req shared-memory ring, forwarding User VM MMIO exits to Service VM userspace. Deliverable: a userspace program receives one User VM MMIO access and completes it. Highest-risk milestone — kept minimal on purpose (no virtio) |
| M14 — Device Model + virtio backends | planned | Userspace `dm` program: VM load/start, virtio-mmio console and blk backends. Deliverable: User VM launched by DM, rootfs on a virtio-blk image |
| M15 — RK3588 port | planned | Runtime FDT parsing, real UART/GIC/storage, board bring-up; reproduce the full chain on hardware |
| Deferred | — | SMMU/DMA isolation and the device-passthrough framework: not needed while all User VM devices are DM-emulated on QEMU; schedule when RK3588 passthrough demands it |

### Milestone renumbering (2026-08-10)

The roadmap was flattened to consecutive integers `M0`–`M15`. The old scheme had
decimal milestones (`M1.5`, `M2.5`, `M3.0`–`M3.5`) and a gap where `M4` was never
used, which made ordering ambiguous. **Historical documents were deliberately not
rewritten** — `docs/superpowers/{specs,plans}/`, `docs/adr/` and `docs/debug/`
are records of what happened at the time, and their filenames are referenced from
git history and already-pushed branches. Use this table when reading them:

| Old | New | Milestone |
|---|---|---|
| M0 | M0 | Hello EL2 |
| M1 | M1 | Bare-metal guest |
| M1.5 | **M2** | PSCI |
| M2 | **M3** | vGIC software injection |
| M2.5 | **M4** | Physical timer + GIC + HW-forwarding |
| M3.0 | **M5** | Linux alive (no interrupts) |
| M3.1 | **M6** | MMIO trap framework |
| M3.2 | **M7** | vGICv3 emulation |
| M3.4 | **M8** | Boot to shell |
| M3.5 | **M9** | SMP |
| M5 | **M10** | Multi-VM foundation |
| M6 | **M11** | vCPU scheduler |
| M7 | **M12** | Hypercall ABI + VM lifecycle |
| M8 | **M13** | HSM kernel driver + io_req ring |
| M9 | **M14** | Device Model + virtio backends |
| M10 | **M15** | RK3588 port |

There was never an `M3.3`, and the old `M4` (once "RK3588 port") was retired when
the port moved to the end of the roadmap; neither has a new-scheme equivalent.
Filenames such as `2026-06-23-m3.5-smp-design.md` keep their old numbering.

### ACRN-model strategy (decided 2026-07-14, revised 2026-07-19)

The end state is the full ACRN architecture: a privileged Service VM running a
userspace Device Model that serves virtio backends to User VMs. Standing
decisions shaping the roadmap ordering:

1. **QEMU-first**: the entire ACRN-model core chain (multi-VM → hypercall →
   HSM → DM) is developed and verified on QEMU `virt`; the RK3588 port comes last.
2. **Scheduler right after multi-VM** (revised 2026-07-19; was "scheduler
   last"): the primary project goal is learning EL2 core technology, and the
   scheduler is the densest remaining piece — it forces per-vCPU state
   (FP/SIMD, system registers, vGIC LR save/restore) to be complete. Doing it
   at M11, before the hypercall→HSM→DM chain, means M12–M14 build on a finished
   state-switching foundation instead of accumulating "no-save" assumptions.
   M10 itself still keeps 1:1 vCPU:pCPU pinning (ACRN "partitioned" mode).
3. **No interim in-hypervisor virtio backends**: virtio backends are written
   once, in the userspace DM (M14). Until then User VMs use vuart console +
   initramfs (no disk). Avoids writing/maintaining the backend logic twice.
4. **FDT parsing deferred to M15**: QEMU milestones keep static board config
   (ADR-0008) and prebuilt guest DTB templates.
5. **EL2 owns the console from M10 on** (2026-07-19): the M5 PL011 passthrough
   is revoked; the physical UART belongs to EL2 and every VM sees a
   trap-and-emulate vuart, with an escape key switching input focus. This is
   the only design that gives every VM an interactive shell over one physical
   UART, and it pre-builds the Service VM console path.
6. **One guest address map for all VMs** (2026-07-19): every VM sees RAM at
   IPA `0x40000000` regardless of the backing PA. This is not new mechanism —
   `vm_config` already separates `mem_base` (IPA) from `ram_pa`, and VM0 has
   mapped IPA `0x40000000` → PA `0x80000000` since M5 — so VM1 is just a
   second config with `ram_pa = 0xC0000000`. One DTB template, one set of
   load-offset constants, and the guest-visible machine is identical across
   VMs. (An identity-mapped VM1 was considered and rejected: it would need a
   second DTB template and a second address-constant set for zero gain.)

## Reference source trees (in `../`)

`../acrn-hypervisor` (x86; primary structural inspiration), `../xvisor` (ARM-first; Stage-2 and vCPU
design), `../bao-hypervisor`. Consult the smallest relevant file before designing a subsystem (Stage-2,
vGIC, PSCI, virtio…). Specs: [ARM ARM](https://developer.arm.com/documentation/ddi0487/latest),
[GIC](https://developer.arm.com/documentation/ihi0069/latest),
[pKVM](https://source.android.com/docs/core/virtualization).
