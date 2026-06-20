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

> 实测启动到 shell 的完整过程见 `docs/debug/m3-boot-debug-walkthrough.md`。
