# M11 前置 B — PSCI/SMCCC 补全 Design

> **Status:** 已实施（2026-10-28，提交见 [plan](../plans/2026-10-28-m11-pre-b-psci-smccc.md)）。
> 范围锁定于 [ADR-0017](../../adr/0017-el2-hardening-before-scheduler-and-passthrough-milestone.md)
> 第 38 行，时间盒 1 周。它为 M11 的电源状态重写先把**接口面**定下来：M11 重写
> `CPU_OFF` 的粒度缺陷（现 VM 级、应为 vCPU 级）时，guest 看到的函数集合不再变。

## 背景与现状

`vpsci.c` 只实现 6 个函数（VERSION/CPU_ON/CPU_OFF/SYSTEM_OFF/SYSTEM_RESET/
FEATURES），`FEATURES` 也只认这 6 个。Linux 侧（本仓库启动的 6.12 内核 +
`arm,psci-1.0`/hvc DTB 节点）每次探测 `CPU_SUSPEND`/`AFFINITY_INFO`/
`MIGRATE_INFO_TYPE`/SMCCC 版本都会落到 "unknown fn" 打印 + NOT_SUPPORTED。
本阶段把这些函数补齐并给出 SMCCC 探测，**不改**既有电源状态语义（VM 级
power-down 模型保持，重写是 M11 的事）。

## 函数语义决定（每个函数一行：请求 → 行为 → 返回）

| 函数 | 行为 | 返回 |
| --- | --- | --- |
| `CPU_SUSPEND` (0x84000001/0xC4000001) | **KVM 先例**：虚拟机里电源状态是幻觉——standby 与 power_down 一律在 EL2 `wfi` 睡到有中断，不销毁调用者上下文；power_down 的 entry/context_id **忽略**（调用者上下文原样存活） | 醒后 `SUCCESS` |
| `AFFINITY_INFO` (0x84000004/0xC4000004) | 只跟踪 vCPU 粒度：`lowest_affinity_level != 0` → `-2`；target Aff0 做 CPU_ON 同款校验（高位须 0、Aff0 < VCPUS_PER_VM），不合法 → `-2`；合法 → 查 `percpu[pcpu].online && !m->off` | ON=0 / OFF=1 |
| `MIGRATE_INFO_TYPE` (0x84000006) | 无 trusted OS，多核安全 | `2`（TOS not present, MP；内核 `PSCI_0_2_TOS_MP`，bao `PSCI_TOS_NOT_PRESENT_MP` 同值） |
| `SYSTEM_RESET2` (0x84000012/0xC4000012) | `reset_type > 1` → `-2`（0=warm/1=cold，DEN0022）；合法则与 `SYSTEM_RESET` 同路径（现模型 = VM 级 power-down，M11 重写） | 不返回 |
| `PSCI_FEATURES` | 追加上述四组（32/64 编码都答 SUCCESS） | SUCCESS / NOT_SUPPORTED |
| `SMCCC_VERSION` (0x80000000) | 与 PSCI 1.1 配套 | `0x10001`（SMCCC v1.1） |
| `SMCCC_ARCH_FEATURES` (0x80000001) | 查询自身与 `SMCCC_VERSION` → 0（支持）；其余（TRNG、ARCH_SOC_ID 等）→ NOT_SUPPORTED | 0 / -1 |

`CPU_SUSPEND` 的决定依据：KVM（`virt/kvm/arm/psci.c`）对 suspend 一律按
"wfi + 原上下文返回" 实现，因为没有可信的下电/唤醒硬件路径；本仓库无调度器
时同样只有这一条诚实路径。**已知边界**：power_down 声明的 entry_point 语义
（醒后从新入口带 context_id 重启）未实现——guest 视角是 retention。若 M11
调度器引入真实的 vCPU 下电，此处随之重写；在此之前这是接口面正确、功耗语义
从简的决定。

## 实现落点

- `arch/psci.h`：补函数 ID、`PSCI_STATE_TYPE_POWERDOWN (1<<30)`、
  `PSCI_TOS_NOT_PRESENT_MP 2`、AFFINITY ON/OFF 值。
- `vmexit/vpsci.c`：`psci_cpu_suspend`（EL2 wfi）、`psci_affinity_info`、
  `psci_migrate_info_type`、`SYSTEM_RESET2` case、`psci_features` 扩表。
- `vmexit/vmexit.c` `handle_hvc`：在 PSCI 分派**之前**拦截两个 SMCCC 基础
  ID（svc 前缀同为 0x80，会被 `svc==0x84` 之外的 fallthrough 吞掉）。
  SMCCC 常量放 `arch/hypercall.h`（`SMCCC_NOT_SUPPORTED` 已在此）。

## Testing Decisions

- **新 SVM 用例 `psci`**（复用 vtimer 的中断骨架）：断言 SMCCC_VERSION ==
  0x10001、AFFINITY_INFO 合法/非法两路、MIGRATE_INFO_TYPE == 2、
  FEATURES(CPU_SUSPEND) == 0；先武装 vtimer 再调 `CPU_SUSPEND(standby)`——
  醒来即证明 EL2 真睡了且中断路径完好（vtimer PPI 走既有 HW-forward 路径）；
  power_down 路径断言"返回后上下文存活"（读一个局部变量）。
- **Linux gate 不应有任何行为变化**：本 DTB 无 `power-domains/idle-states` 节点，
  Linux 不会调 CPU_SUSPEND；boot 日志里 "PSCI: unknown fn=0x84000006"（探
  MIGRATE_INFO_TYPE）消失是预期变化。
- QEMU TCG 不模拟功耗；suspend 的"睡"在 TCG 下时间上不可观测，行为断言是
  "返回了、中断到了"。

## Out of Scope

- `CPU_OFF` 粒度重写（VM 级 → vCPU 级）与 ON_PENDING 状态机：M11。
- `MIGRATE` / `MIGRATE_INFO_UP_CPU`（无 TOS，永久 NOT_SUPPORTED）。
- 真实功耗管理、per-CPU idle state 的 DTS 暴露。
