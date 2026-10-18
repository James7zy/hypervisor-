# 知识库 / Reference

跨里程碑、可长期查阅的参考资料与操作配方,随实现演进更新。与 `adr/`、`debug/`、
`superpowers/` 的分工、语言与命名规则见 [docs/README.md](../README.md)。

## 目录组织

按**体系结构**分子目录,根目录只放跨体系结构的通用资料:

| 路径 | 内容 |
| --- | --- |
| `arm/` | **ARM64 / EL2** — 本仓库的实现基座。所有子系统精读与操作配方。 |
| `x86/` | **x86-64 / VT-x** — 对照文档。用于读 `../acrn-hypervisor` 时映射概念,不是移植计划。 |
| 根目录 | 与体系结构无关的通用资料(如 Mermaid 避坑清单)。 |

---

## 通用(根目录)

- [superpowers.md](superpowers.md) — Claude Code 本地 skills 与 pi 项目包的
  Superpowers 安装、使用、验证与更新；Claude 不使用插件，不全局启用。
- [mermaid-gotchas.md](mermaid-gotchas.md) — Mermaid 避坑清单:如何用
  `mermaid.parse()`+jsdom 本地验证图,以及三个实测坑(participant id 撞关键字如
  `Loop`、`as` 别名加双引号、`box` 泳道在本地 `mermaid@11` 报 `Option is not defined`)。
  写 ADR/架构图前先过一眼。

## ARM64(`arm/`)— 实现基座

- [arm/current-architecture.md](arm/current-architecture.md) — **当前实现入口**：M10 双 VM
  与 arch 边界重构；模块关系图、VM/vCPU 类图、启动和跨核控制台时序。
  说明已落地的钩子、board CPU 数量单一来源及仍未实现的 M11 能力。
- [arm/2026-06-21-architecture-zoom-out.md](arm/2026-06-21-architecture-zoom-out.md) —
  架构鸟瞰:一次「拉高一层」的整体梳理,从顶层调用链 → GIC 子系统 → 一次键盘输入
  端到端 trace,三层逐步放大,把各模块与调用者串成一张图。配套
  [[adr-0012]](物理 GICv3 独占)。**历史快照(M3 时代,单 VM)**,不反映 M10 多 VM
  现状，保留原文；当前入口见 `current-architecture.md`。
- [arm/target-architecture.md](arm/target-architecture.md) — **目标架构**(ACRN 模型,
  M12–M15):Service VM + 用户态 Device Model 的终点形态,两张 Mermaid 图(顶层系统图、
  特权层次图)+「与当前实现的差距」对照表。原为 `CLAUDE.md` 里的两块 ASCII 大图,
  2026-09-14 迁出重画。**非当前实现**——现状看 `arm/current-architecture.md`；
  x86 对位版本见 [[x86/architecture-x86]] §1、§2。
- [arm/2026-06-21-gicd-gicr-trap-investigation.md](arm/2026-06-21-gicd-gicr-trap-investigation.md) —
  一次从提问到实证的查证:「客户机访问 GICD/GICR 怎么到 EL2?」带 instrumentation 的
  真实 boot 证明 ADR-0012「触发 Stage-2 abort → 影子模拟 → 从不碰硬件」**不成立**——
  客户机经 `l1_table[0]` 的 identity Device block **直通物理 GICR**(`found redistributor`
  为铁证),两个 vgic handler 命中 0 次。含两处被推翻的错误解释及修法建议。配套
  [[adr-0012]]、[[stage2]]、[[handle-exit-dispatch]]。
- [arm/stage2.md](arm/stage2.md) — 页表粒度与 IPA→PA 推导；当前 per-VM L1/L2、GIC/
  PL011 hole、Linux RAM backing、静态内存分配，以及 HCR/VTCR/VTTBR 的设置位置。
- [arm/stage2-l1-to-l2.md](arm/stage2-l1-to-l2.md) — Stage-2 L1→L2 映射转换:为什么以及怎样把
  一个 1 GB 的 L1 block 拆成 L2 table + 512 个 2 MB entry,从而能单独给 GICD/GICR 那
  2 MB「挖洞」（invalid → fault → 影子模拟）。保留最初 GIC-only 的教学对照；
  **当前 GIC 与 PL011 hole 均已落地**，第 9 节给出重构后的代码链路。
- [arm/guest-initramfs.md](arm/guest-initramfs.md) — 静态 aarch64 busybox + 仓库维护的
  `/init`；双 VM 加载地址、`hv.vm=` 提示符与 emulated PL011 控制台。
- [arm/vcpu-run-world-switch.md](arm/vcpu-run-world-switch.md) — `vcpu_arch_load/run`、
  per-pCPU `g_hv_ctx`、同步异常与 IRQ 直接 `eret`、调试 HVC 返回 C 的区别；
  `eret` 前保持 EL2 IRQ mask 的原因。
- [arm/handle-exit-dispatch.md](arm/handle-exit-dispatch.md) — HVC / sysreg / Data Abort
  分发；arch 解码与 common MMIO 总线的职责边界、错误处理和当前限制。
- [arm/vgic-injection.md](arm/vgic-injection.md) — 四种注入与 LR0/1/2 分工；timer
  HW 转发、SPI 锁与 pending 发布、远程 kick；为什么现有 save/restore 不能直接用于调度。
- [arm/vgic-debug-logging.md](arm/vgic-debug-logging.md) — `CONFIG_DEBUG_VGIC` 的开启、
  当前 3 个打印点（GICD/GICR + timer HW 注入）及诊断限制；没有 SPI/SGI 日志点。

## x86-64(`x86/`)— 对照参考

- [x86/architecture-x86.md](x86/architecture-x86.md) — 架构对照：把目标 ACRN 架构
  换成 **x86-64 VT-x 基座**重画（顶层系统图、特权层次图），外加 6 张 Mermaid
  对照图与逐项术语表(EL2↔VMX root、Stage-2↔EPT、vGICv3↔vLAPIC/APICv、PSCI↔ACPI+
  VMCALL、SMMU↔VT-d)。用途是读 `../acrn-hypervisor` 时能把概念映射回来,以及判断
  本仓库某个设计属于「通用虚拟化结构」还是「ARM 特有细节」——第 8 节给出 arch/common/dm
  三层在 ADR-0015 下的当前边界。**非移植计划**，也不是多架构可移植性证明。

> 实测启动到 shell 的完整过程见 `docs/debug/m3-boot-debug-walkthrough.md`。
