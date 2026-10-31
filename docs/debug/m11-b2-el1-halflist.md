# M11 B2 半清单实验（Q9 纪律）——EL1 保存清单的故意残缺验证

> 2026-10-31（Day 5，片 B2 Task 4）。工作树内验收操作，未落红 commit；本档为实验记录。

## 实验

1. 实现 `el1_save/restore` **故意只存 SCTLR_EL1 与 TCR_EL1**（12 寄存器清单的 2 项）。
2. ctxsw 用例双方各盖 6 枚 EL1 canary（互异值）：
   - 惰性数据寄存器全值 canary：`TPIDR_EL1` / `TPIDRRO_EL0` / `CONTEXTIDR_EL1`
   - MMU-off 下安全 stamp：`TTBR0_EL1` / `MAIR_EL1` / `VBAR_EL1`
   - vCPU0 = 0xE1A../0xE1B../0xE1C../0xE1D... 系，vCPU1 = 0xF1A... 系。
3. 轮转 + 计时段后双方各自回读校验。

## 观察到的失败模式（QEMU 实测）

**4/4 组 CHECK FAILED，0 组 intact**——对称、确定性：

- vCPU1 的 6 枚 canary 全部回读出 vCPU0 的值（或反之）：未保存的 EL1 寄存器在
  切换后**保留的是最后驻留者的值**——bare-metal 下双方都写过自己的值，谁最后跑
  谁的值留下。
- 半清单里"已保存"的 SCTLR/TCR 未盖章——本实验只证明缺失项；SCTLR 的保位 stamp
  （`orig|canary<<32`）在 TCG 上可能被 RES0 掩位（vacuous canary），全值惰性
  寄存器才是"有牙齿"的哨兵（A2 裁定的实证：TPIDR/TPIDRRO/CONTEXTIDR 三枚最强）。

## 结论

1. 12 寄存器清单不可裁剪至 2：TPIDR 族与 TTBR0/MAIR/VBAR 族都会跨切换互踩。
2. 探测器形态确认：惰性寄存器全值 stamp + 互异值双盖 + 静默握手（v1_final）后
   独占校验——并发打印会字符级交织吃掉结论行（本实验先踩到：无握手时校验行消失）。
3. 完整清单落地后同用例全绿（见当日提交）。

## 对照 bao/xvisor（Q12：先推导后对照；差异表）

| 寄存器 | 本仓（推导+实证） | bao（vcpu_arch.c） | xvisor（cpu_vcpu_switch.S / regs） | 差异处置 |
|---|---|---|---|---|
| SCTLR/TTBR0/TTBR1/TCR/MAIR | 保存（MMU 状态） | 保存 | 保存 | 一致 |
| VBAR_EL1 | 保存 | 保存 | 保存 | 一致 |
| CPACR_EL1 | 保存（trap 控制） | 保存 | 保存 | 一致 |
| TPIDR_EL1 / TPIDRRO_EL0 | 保存（线程指针） | 保存 | 保存 | 一致 |
| CONTEXTIDR_EL1 | 保存 | 保存 | 保存 | 一致 |
| ESR_EL1 / FAR_EL1 | 保存（异常语料） | 保存 | 保存 | 一致 |
| AMAIR_EL1 | 未保存 | 保存 | 保存 | **差异**：仅影响访问 AMAIR 的调试路径，Linux 默认不读；记为已知缺口（首个消费者出现时补） |
| PAC 密钥（APIA/APIB/APDA/APDB/APGA） | 未保存 | 保存 | 保存 | **差异**：SVM 用例无 PAuth；Linux gate 在 1:1 不切换；片 C（Linux 共享）前必须补——ADR-0019 已记 |
| CNTKCTL_EL1 | 未保存 | 保存 | 保存 | **差异**：CNTHCTL 已由 EL2 掌管；EL1 视图在共享场景可能互踩，与 vtimer 重设计（已知缺口）合并处理 |

已知缺口三件（AMAIR/PAC/CNTKCTL_EL1）挂 ADR-0019 已知缺口条目，触发条件分别为：
首个 AMAIR 消费者 / 片 C 共享 Linux / EL2 背景定时器重设计。
