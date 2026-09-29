# 文档总览 / Docs

本目录存放**换一台机器、换一个人仍然成立**的项目知识(判定规则见 `CLAUDE.md` 的
"Where knowledge lives")。按"这份文档回答什么问题"分四类:

| 目录 | 回答的问题 | 会不会改 |
| --- | --- | --- |
| [`adr/`](adr/README.md) | **为什么**这么设计,否决了什么(Status / Decision / Consequences) | 不原地改;被推翻时新开编号并标 `Superseded` |
| [`superpowers/specs/`](superpowers/specs/)、[`plans/`](superpowers/plans/) | 某个里程碑当时**要建什么、怎么拆**(spec = 设计,plan = 执行步骤) | 历史记录,不改 |
| [`debug/`](#debug-调试实录) | 某次问题**是怎么定位的**,按时间顺序 | 历史记录,不改 |
| [`reference/`](reference/README.md) | 某个子系统**现在怎么工作**、某件事**怎么操作** | 随实现演进持续更新 |

拿不准放哪:写的是"为什么" → `adr/`;是"当时的过程" → `debug/`;是"现在的样子" →
`reference/`;是"下一个里程碑要做什么" → `superpowers/`。

> `superpowers/` 这个名字来自生成 spec/plan 的插件默认路径,并非内容分类名;
> 保留它是为了让插件新生成的文档继续落在同一处。

## 新读者从哪里读

1. [`CLAUDE.md`](../CLAUDE.md) — 项目现状、构建与验证、物理 GIC / vGIC 边界、已知缺陷。
2. `roadmap` skill(`.claude/skills/roadmap/SKILL.md`)— M0–M15 里程碑状态与
   2026-08-10 的编号重排对照表。**历史文档里的里程碑号多为旧编号**(如 M5 = 现在的
   M10),读前先查这张表。
3. [`reference/arm/target-architecture.md`](reference/arm/target-architecture.md) — 目标
   形态(ACRN 模型)与当前实现的差距。
4. [`adr/README.md`](adr/README.md) — 14 条架构决策的索引。

> `reference/arm/2026-06-21-architecture-zoom-out.md` 是 **M3 时代(单 VM)的历史快照**,
> 不反映当前多 VM 实现,计划在 M11 完成后重写。

## debug/ 调试实录

| 文件 | 内容 |
| --- | --- |
| [`debugging-m0.md`](debug/debugging-m0.md) | M0 调试手册 |
| [`m0-boot-internals.md`](debug/m0-boot-internals.md) | M0 启动内部机制详解 |
| [`m0-retrospective.md`](debug/m0-retrospective.md) | M0(Hello EL2)回顾总结 |
| [`debugging-m1.md`](debug/debugging-m1.md) | M1 调试手册 |
| [`m3-boot-verification.md`](debug/m3-boot-verification.md) | M3 第一次真实 QEMU 运行(2026-06-19)发现的三个 bug |
| [`m3-boot-debug-walkthrough.md`](debug/m3-boot-debug-walkthrough.md) | M3 启动到 shell 的完整调试过程(可重现教程) |

## 写新文档的规则

**语言**
- 新文档用**中文**;代码标识符、寄存器名、专业术语保留英文。
- 同一份内容**只维护一种语言**,不写翻译副本(副本很快会不同步)。
- 已有的英文文档(ADR、早期 spec/plan)不翻译。

**命名**

| 类别 | 新文件命名 | 理由 |
| --- | --- | --- |
| `debug/` | `YYYY-MM-DD-<主题>.md` | 某个时间点的记录 |
| `reference/` | `<主题>.md`,**不带日期** | 持续更新,日期会误导 |
| `adr/` | `NNNN-<主题>.md` | 见 [`adr/README.md`](adr/README.md) |
| `superpowers/specs/`、`plans/` | `YYYY-MM-DD-<里程碑或主题>[-design].md` | 插件约定 |

已有文件中不符合上述命名的(如 `debug/debugging-m0.md`、
`reference/arm/2026-06-21-*.md`)是历史遗留,**不改名**——路径已被大量交叉引用。

**图**:架构文档用 Mermaid 画时序图、类图、模块关系图;动笔前先看
[`reference/mermaid-gotchas.md`](reference/mermaid-gotchas.md)。
