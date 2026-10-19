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
- 新增非平凡逻辑（分支、循环、解析、安全或状态更新等）应留下一个能验证行为的小测试或自检；它不能替代构建、回归和 QEMU 验收。
- 有已知限制的临时简化，用 `ponytail:` 注释写明适用上限和重新设计的触发条件，避免“以后再说”没有边界。
- 回复结尾用一两行说明未做或未验证的部分，以及用户需要知道的风险；未实际运行的检查不能写成已通过。

## Project

A research Type-1 multi-architecture hypervisor: ARM64 (AArch64) first, on QEMU `virt` and later
RK3588, with AMD/Intel x86-64 (AMD SVM / Intel VT-x) as the second architecture. Only ARM64 runs
today; `arch/x86/` is a link-check stub. Inspired by ACRN, Xvisor and bao; the directory layout
mirrors ACRN's `hypervisor/`.

**M10 is done**: two Linux VMs, two vCPUs each, statically pinned 1:1 to pCPUs (2+2 on 4),
EL2-owned console and shell. **Next: M11 (vCPU scheduler)**; the long-term target is the
full ACRN model (Service VM + userspace Device Model). Roadmap: [Milestone Roadmap](#milestone-roadmap).
For M11 also read the
[arch-boundary alignment](docs/superpowers/specs/2026-10-04-m11-arch-boundary-alignment.md).

**Design principle:** small, fast iterations; break work into the smallest practical,
clearly defined units; high cohesion, low coupling. Each milestone gets a spec in
`docs/superpowers/specs/` and a plan in `docs/superpowers/plans/`.

## Where knowledge lives

Would it still hold on another machine, for another person?
- **Yes** → the repo: lessons in `docs/debug/`, decisions in `docs/adr/`, standing rules here (`AGENTS.md` is a symlink to this file).
- **No** (local paths, personal workflow) → private memory under `~/.claude/`. No in-repo `memory/`.
- **New docs are written in Chinese** (identifiers and technical terms stay English); never keep the
  same content in two languages. Rules: [docs/README.md](docs/README.md).

## Architecture

Current modules, objects and boot/console sequences:
[current-architecture.md](docs/reference/arm/current-architecture.md).
[target-architecture.md](docs/reference/arm/target-architecture.md) is the future ACRN model, not implemented.

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

Roadmap ordering decisions (QEMU first, scheduler right after multi-VM, virtio backends written once
in the userspace DM, FDT deferred to M15): [ADR-0016](docs/adr/0016-acrn-model-roadmap-decisions.md).

## Reference source trees (in `../`)

`../acrn-hypervisor` (x86; primary structural inspiration), `../xvisor` (ARM-first; Stage-2 and vCPU
design), `../bao-hypervisor`. Consult the smallest relevant file before designing a subsystem (Stage-2,
vGIC, PSCI, virtio…). Specs: [ARM ARM](https://developer.arm.com/documentation/ddi0487/latest),
[GIC](https://developer.arm.com/documentation/ihi0069/latest),
[pKVM](https://source.android.com/docs/core/virtualization).

