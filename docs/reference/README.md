# 知识库 / Reference

跨里程碑、可长期查阅的参考资料与操作配方。区别于其它文档:

- `docs/adr/` — **架构决策**(为什么这么设计,Status/Decision/Consequences)。
- `docs/debug/` — **调试实录**(某次定位过程,按时间顺序)。
- `docs/superpowers/{specs,plans}/` — **历史规格与计划**(当时怎么拆解执行,不改)。
- `docs/reference/`(本目录)— **稳定的子系统说明与操作配方**,随实现演进更新。

## 索引

- [stage2.md](stage2.md) — Stage-2 页表与 IPA 范围解析:granule/level 位拆分、
  两个静态 1 GB block 描述符的由来与 IPA→PA 推导。配套 [[adr-0004]]
  (`docs/adr/0004-stage2-static-1gb-block-mapping.md`)。
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

> 实测启动到 shell 的完整过程见 `docs/debug/m3-boot-debug-walkthrough.md`。
