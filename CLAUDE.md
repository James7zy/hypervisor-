# CLAUDE.md

Guidance for Claude Code (claude.ai/code) in this repository.

Architecture docs must use Mermaid: sequence, class and module-relationship diagrams.

## Ponytail：最小完整改动

参考 [Ponytail 的 AGENTS.md](https://github.com/DietrichGebert/ponytail/blob/b088b2df6e08d4306c6a3c3d575fe38c2d2d2989/AGENTS.md)
按本工程特点整合以下常驻工作原则。
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

## 里程碑前置：限时与范围锁

一个里程碑正式开工前如果又需要"前置阶段"（重构、加固、补全缺口等），该前置阶段必须：

1. 范围锁定在开工前盘点出的清单里；过程中新发现的缺口记成后续已知缺口，不追加进当前前置阶段。
2. 定时间盒（参考相近规模里程碑的历史节奏估算，写进对应 ADR 或 spec），不能无限期。
3. 到期即止：做到哪算哪，未完成部分标成已知缺口，强制进入原计划的下一个里程碑；不因为"还不够干净"
   而继续留在前置阶段，不顺延。

**Why:** 2026-10-22 定下。arch-boundary 重构之后，M11 调度器前又叠了一层 EL2 加固 + PSCI/SMCCC
补全（ADR-0017），从 M10 完工到此时三个月里没有一行 M11 相关代码落地——不是路线图设计错了，是
前置任务会自我繁殖。这条规则管的是这个会反复出现的模式，不只是 M11 这一次；M12 的页分配器 +
动态 Stage-2 等前置切片同样适用。

## Project

A research Type-1 multi-architecture hypervisor: ARM64 (AArch64) first, on QEMU `virt` and later
RK3588, with AMD/Intel x86-64 (AMD SVM / Intel VT-x) as the second architecture. Only ARM64 runs
today; `arch/x86/` is a link-check stub. Inspired by ACRN, Xvisor and bao; the directory layout
mirrors ACRN's `hypervisor/`.

**Current state (M10, gate-verified 2026-07-20)**: two Linux VMs, two vCPUs each,
statically pinned 1:1 to pCPUs (2+2 on 4), EL2-owned console and shell; no scheduler
yet. The long-term target is the full ACRN model (Service VM + userspace Device
Model). The milestone roadmap and its execution tracking live in
[Linear](https://linear.app/jamestam/project/hypervisor-3b151b91cb63)
(阶段0 = M0–M10 completed; 阶段1–9 = M11 prerequisites → M16). When starting M11,
read the
[arch-boundary alignment](docs/superpowers/specs/2026-10-04-m11-arch-boundary-alignment.md)
together with the original scheduler design rather than restoring its old paths.

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

## Reference source trees (in `../`)

`../acrn-hypervisor` (x86; primary structural inspiration), `../xvisor` (ARM-first; Stage-2 and vCPU
design), `../bao-hypervisor`. Consult the smallest relevant file before designing a subsystem (Stage-2,
vGIC, PSCI, virtio…). Specs: [ARM ARM](https://developer.arm.com/documentation/ddi0487/latest),
[GIC](https://developer.arm.com/documentation/ihi0069/latest),
[pKVM](https://source.android.com/docs/core/virtualization).

