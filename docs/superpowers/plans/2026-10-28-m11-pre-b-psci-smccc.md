# M11 前置 B — PSCI/SMCCC 补全 Implementation Plan

> **Status:** In progress. **时间盒:** 1 周（ADR-0017），到期即止。
> Full design: `docs/superpowers/specs/2026-10-28-m11-pre-b-psci-smccc-design.md`。

**Goal:** 补齐 PSCI 四组函数 + SMCCC 版本/特性探测，接口面为 M11 电源状态
重写定型。

**Observable DoD:** `make test` 全绿（含新 `psci` 用例）；Linux 双 VM gate
全部不回归；boot 日志中 `PSCI: unknown fn=0x84000006` 消失。

**分支:** `m11-pre-b-slice1-psci` → `m11-pre-b-slice2-smccc-test`（一
切片一分支，gate 过了合 master）。

---

## Slice 1 — PSCI 函数补全 (`m11-pre-b-slice1-psci`)

### Task 1.1 — `arch/psci.h` 常量

- [ ] 函数 ID：`PSCI_CPU_SUSPEND_32/64`、`PSCI_AFFINITY_INFO_32/64`、
  `PSCI_MIGRATE_INFO_TYPE`、`PSCI_SYSTEM_RESET2_32/64`；语义常量
  `PSCI_STATE_TYPE_POWERDOWN (1<<30)`、`PSCI_TOS_NOT_PRESENT_MP 2`、
  `PSCI_AFF_LEVEL_ON 0 / _OFF 1`。

### Task 1.2 — `vpsci.c` 四个函数 + FEATURES 扩表

- [ ] `psci_cpu_suspend`：EL2 `wfi`（两型同路径），醒后 SUCCESS；注释写明
  KVM 先例与 entry/context_id 忽略的边界（M11 重写）。
- [ ] `psci_affinity_info`：level!=0 或 Aff0 校验失败 → -2；否则按
  `percpu[pcpu].online && !m->off` 返回 0/1。
- [ ] `psci_handle` 加 `MIGRATE_INFO_TYPE → 2`、`SYSTEM_RESET2`（x1>1 →
  -2，否则 `psci_power_down("SYSTEM_RESET2")`）、SUSPEND/AFFINITY 两组 case
  （32/64 合并）。
- [ ] `psci_features` 追加四组 SUCCESS。

### Task 1.3 — Slice-1 gate

- [ ] `make clean && make` 零警告；`make test` 全绿（无新用例，行为不变）。
- [ ] Linux 双 VM gate（gate 脚本全套）：不回归；日志确认
  `unknown fn=0x84000006` 消失。
- [ ] Commit: `feat(m11-pre-b): PSCI CPU_SUSPEND/AFFINITY_INFO/MIGRATE_INFO_TYPE/SYSTEM_RESET2 (slice 1)`

---

## Slice 2 — SMCCC 探测 + psci 用例 (`m11-pre-b-slice2-smccc-test`)

### Task 2.1 — `arch/hypercall.h` + `vmexit.c` SMCCC 基础层

- [ ] 常量：`SMCCC_VERSION 0x80000000`、`SMCCC_ARCH_FEATURES 0x80000001`、
  `SMCCC_VERSION_1_1 0x10001`。
- [ ] `handle_hvc` 在 PSCI svc 分派前拦截：VERSION → 0x10001；
  ARCH_FEATURES(x1) → 自身与 VERSION 返回 0，其余 NOT_SUPPORTED。

### Task 2.2 — SVM 用例 `psci`

- [ ] `tests/svm/cases/psci.c`（vtimer 骨架：`svm_gic_el1_init` +
  vtimer IRQ handler）：按 spec Testing Decisions 断言六个标记。
- [ ] `tests/svm/expect/psci.txt`；Makefile `SVM_CASES += psci`、
  `test-qemu-psci` 目标并入 `test:` 与 `.PHONY`。

### Task 2.3 — Slice-2 gate（= 本阶段整体 gate）

- [ ] `make clean && make` 零警告；`make test` 全绿（11 项）。
- [ ] Linux 双 VM gate 全套不回归。
- [ ] Commit: `feat(m11-pre-b): SMCCC version/arch_features probe + psci SVM case (slice 2)`

---

## 时间盒到期处理

未完成部分记入 ADR-0017 已知缺口，直接进 M11。状态行更新规则同前置 A 的
plan（到期统一更新，切片合入不勾选）。
