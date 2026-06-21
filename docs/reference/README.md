# 知识库 / Reference

跨里程碑、可长期查阅的参考资料与操作配方。区别于其它文档:

- `docs/adr/` — **架构决策**(为什么这么设计,Status/Decision/Consequences)。
- `docs/debug/` — **调试实录**(某次定位过程,按时间顺序)。
- `docs/superpowers/{specs,plans}/` — **历史规格与计划**(当时怎么拆解执行,不改)。
- `docs/reference/`(本目录)— **稳定的子系统说明与操作配方**,随实现演进更新。

## 索引

- [mermaid-gotchas.md](mermaid-gotchas.md) — Mermaid 避坑清单:如何用
  `mermaid.parse()`+jsdom 本地验证图,以及三个实测坑(participant id 撞关键字如
  `Loop`、`as` 别名加双引号、`box` 泳道在本地 `mermaid@11` 报 `Option is not defined`)。
  写 ADR/架构图前先过一眼。
- [2026-06-21-architecture-zoom-out.md](2026-06-21-architecture-zoom-out.md) —
  架构鸟瞰:一次「拉高一层」的整体梳理,从顶层调用链 → GIC 子系统 → 一次键盘输入
  端到端 trace,三层逐步放大,把各模块与调用者串成一张图。新读者入口。配套
  [[adr-0012]](物理 GICv3 独占)。
- [2026-06-21-gicd-gicr-trap-investigation.md](2026-06-21-gicd-gicr-trap-investigation.md) —
  一次从提问到实证的查证:「客户机访问 GICD/GICR 怎么到 EL2?」带 instrumentation 的
  真实 boot 证明 ADR-0012「触发 Stage-2 abort → 影子模拟 → 从不碰硬件」**不成立**——
  客户机经 `l1_table[0]` 的 identity Device block **直通物理 GICR**(`found redistributor`
  为铁证),两个 vgic handler 命中 0 次。含两处被推翻的错误解释及修法建议。配套
  [[adr-0012]]、[[stage2]]、[[handle-exit-dispatch]]。
- [stage2.md](stage2.md) — Stage-2 页表与 IPA 范围解析:granule/level 位拆分、
  两个静态 1 GB block 描述符的由来与 IPA→PA 推导。配套 [[adr-0004]]
  (`docs/adr/0004-stage2-static-1gb-block-mapping.md`)。
- [stage2-l1-to-l2.md](stage2-l1-to-l2.md) — Stage-2 L1→L2 映射转换:为什么以及怎样把
  一个 1 GB 的 L1 block 拆成 L2 table + 512 个 2 MB entry,从而能单独给 GICD/GICR 那
  2 MB「挖洞」(invalid → fault → vgic 影子模拟)而让同 1 GB 内的 PL011 继续直通。含 L1
  vs L2 粒度/描述符/权限对比、两组「转换前/后」例子与 Mermaid 图。**注**:punch-hole
  代码尚未落地,文档第 9 节如实区分「现状/TODO」。续 [[stage2]]、配套 [[adr-0012]]。
- [guest-initramfs.md](guest-initramfs.md) — 用户自备的 busybox initramfs 配方
  (静态 aarch64 busybox + `/init` → 交互式 shell);地址布局、`console=ttyAMA0`
  vs `hvc0` 的取舍。M3.4 启动到 shell 用。
- [vcpu-run-world-switch.md](vcpu-run-world-switch.md) — `vcpu_run` 与 EL2↔EL1
  世界切换:进 guest(`vcpu_run`)/ 出 guest(`el1_sync_handler`)/ 彻底退出
  (`hv_restore`)三段汇编如何闭合;为何 `eret` 前不能解开 EL2 IRQ 屏蔽。
- [handle-exit-dispatch.md](handle-exit-dispatch.md) — `handle_exit` 退出分发:
  按 `ESR_EL2` 的 EC 分流 HVC(hypercall / PSCI)与 Data Abort;MMIO
  trap-and-emulate 如何解析 ISS、查 MMIO 总线、替 guest 模拟一条 load/store。
  续 [[vcpu-run-world-switch]]。
- [vgic-injection.md](vgic-injection.md) — vGIC 中断注入:注入 = 写 `ICH_LR<n>_EL2`
  列表寄存器;`vgic_inject_sw` / `_hw` / `_spi` 三者区别(HW 位、pINTID、LR0/LR1
  分配);`el2_irq_handler` 按 INTID 选注入方式,以及为何 HW 转发只 drop 不
  deactivate。配套 [[adr-0001]]。
- [vgic-debug-logging.md](vgic-debug-logging.md) — 内置 vGIC 调试日志开关
  `CONFIG_DEBUG_VGIC`(默认关、零开销):怎么开、4 个打印点(GICD/GICR 寄存器影子
  访问 + timer PPI / PL011 SPI 注入)的字段含义、典型 boot 片段怎么读、怎么据此判断
  punch-hole / 注入链是否正常。续 [[vgic-injection]]、配套 [[adr-0012]]。

> 实测启动到 shell 的完整过程见 `docs/debug/m3-boot-debug-walkthrough.md`。
