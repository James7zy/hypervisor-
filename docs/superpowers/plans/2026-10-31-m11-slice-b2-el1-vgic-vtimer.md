# M11 片 B2 — EL1 全量、vGIC/vtimer 运行期协议、block/wake 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 切换从 GPR 级升到**EL1 全量**（含 per-vCPU vtimer 与 VMPIDR 身份），vGIC 影子获得运行期协议（publisher-wins 合并），WFI 获得 block/wake 语义（有竞争让出、独处阻塞），`ICH_VTR_EL2` 探测替换 LR 数硬编码假设——共享 pCPU 上可跑会编程 CNTV/用中断的用例。

**Architecture:** B1 的 put/resume 钩子是全部生长点：put 扩为 vGIC 合并读 + CNTV 影子 + EL1 批量存；resume 对称恢复 + VMPIDR 重编。WFI 三分：pending vIRQ→直续、队列有他者→yield（B1 路径保留）、独处→BLOCK+出队+解栈，注入路径经 `sched_wake` 唤醒。双循环先上提为公共函数（关卡②账单 1），后续所有任务只改一处。

**Tech Stack:** freestanding C11、host gcc 单测、QEMU gate（8 场景 + 扩展）、`../xvisor`、`../bao-hypervisor` 对照（Q12：卡壳一天才读）。

**Spec:** `docs/superpowers/specs/2026-10-30-m11-vcpu-scheduler-design.md` §5-B 余项、§4（两层划分）、§8.1/8.2（风险）、D4/D13；ADR-0018 待决两项随本片裁定。

## Global Constraints

- **Q9/Q12 纪律**：EL1 清单先自行推导、detector 验证、再对照 bao/xvisor 记差异（`docs/debug/`）；**半清单故意失败实验 = 工作树内验收操作**，不留红 commit。
- arch-neutral 约束照旧（host 模块零 arch 头）；新钩子同 commit x86 stub。
- 每 Task 后 `make test` 8 场景全绿（新场景落地后为 9）；改头文件 → `make clean`。
- PSCI 接口面冻结不变；CPU_SUSPEND retention 契约（返回 SUCCESS + 上下文完整）保持。
- R5 边界**随协议落地逐项解除**（解除项须在新用例/注释中显式声明：CNTV ✓ 中断 ✓；共享跑 Linux 仍留待手工 gate 验证后）。
- gate 绿后才 commit（两次教训在案）；新文档中文。

## Review Focus

| # | 失败模式 | 钉住它的测试/步骤 |
|---|---|---|
| 1 | put 的活 LR 读覆盖远端刚发布的影子（vuart RX 打到已换下的 vCPU） | publisher-wins 合并代码 + 注入-唤醒跨让出场景（Task 6/新场景） |
| 2 | 共享 pCPU 的 CNTV 互踩（两 vCPU 各自的 timer 互相清零） | per-vCPU CNTV 影子存取 + ctxsw 计时轮转扩展（Task 3） |
| 3 | BLOCK 后无人唤醒 → vCPU 永失（注入路径漏挂 wake） | sched_wave 挂到全部三个注入入口 + vtimer 唤醒场景（Task 2） |
| 4 | EL1 清单漏项 → 特定 guest 寄存器跨切换静默腐坏 | detector 全量盖章段恢复（SCTLR/TTBR/MAIR/VBAR）+ 半清单实验记录（Task 4） |
| 5 | resume 不重编 VMPIDR → 身份漂移（关卡②账单 2） | resume 重编 + ctxsw 各 vCPU 读 MPIDR 断言（Task 4） |

---

### Task 1: 双循环上提为公共调度循环（账单 1，机械）

**Files:**
- Create: 无新文件——`common/sched/sched.c` 增 `void sched_loop(u32 pcpu)`（把 vm.c:CPU0 循环与 secondary.c 循环的公共 ~40 行逐行搬入；`percpu[pcpu]` 参数化）
- Modify: `hypervisor/include/sched.h`（+原型）、`hypervisor/common/vm/vm.c`（vm_run 尾部调 `sched_loop(0)`）、`hypervisor/arch/arm64/cpu/secondary.c`（调 `sched_loop(id)`；保留其 EL2 初始化/在线发布/printk 前奏与 cur_vcpu 种子）

**Interfaces:** Produces `sched_loop(u32 pcpu)`（noreturn 语义）。两调用方保留各自的前奏；循环内行为零变化。

- [ ] Step 1 搬运（纯移动，逐行对齐；secondary 侧注释并入）；`make test` 8× 全绿 + host 165；commit。

### Task 2: block/wake——WFI 三分与 sched_wake

**Files:**
- Modify: `hypervisor/common/sched/sched.c`（+`void sched_wake(struct vcpu *v)`：锁内 `sched_enqueue`；状态 BLOCKED→RUNNABLE 经 `vcpu_state_step`，非 BLOCKED 幂等返回；RUNNABLE 转移后 `cpu_arch_kick`）、`hypervisor/include/sched.h`（+原型）、`hypervisor/arch/arm64/vmexit/vmexit.c`（WFI 三分：`vgic_has_pending(v)`（新 arch 内联，读 ICH_LRx EL2 活状态或 `v->arch` pending 位——实现取最小：扫 ich_lr[N] state 位）为真→仅推进 ELR 解栈（prev 仍 RUNNING 走快路径直续）；队列有他者→B1 yield 路径不变；否则 `VCPU_EV_BLOCK`+不入队+解栈）、`hypervisor/arch/arm64/vgic/{vgic.c,vgic_sgi.c}` 与 `irq_handler.c` 的三个注入入口（`vgic_inject_spi/sgi/hw` 成功后调 `sched_wake(target)`——arch 调 generic，无新钩子）、`hypervisor/arch/arm64/vgic/vgic.h`（+`vgic_has_pending` 原型）

**裁定（ADR-0018 待决项落定，Task 8 记入 ADR-0019）**：EV_YIELD 边**保留**（有竞争路径），BLOCK 边补齐（独处路径）；两语义互补非替代。

**Interfaces:** Produces `sched_wake(struct vcpu *v)`；`vgic_has_pending(struct vcpu *v)`。CPU_SUSPEND 改造（Task 7）复用 BLOCK 路径。

- [ ] Step 1 sched_wake + WFI 三分 + 三注入入口挂 wake；`make test` 8× 全绿（既有场景无 BLOCK 触发——Linux idle wfi 独处时阻塞等 timer PPI：`vgic_inject_hw` 的 wake 唤醒之；手工 gate 必须仍 12/12）。
- [ ] Step 2 手工 gate 12/12（Linux idle 阻塞路径的首次真实验证）；commit。

### Task 3: per-vCPU vtimer（D4——首个会编程 CNTV 的共享用例）

**Files:**
- Modify: `hypervisor/arch/arm64/include/arch/vm.h`（`struct vcpu_arch` 尾部 +`u64 cntvoff, cntv_ctl, cntv_cval;`——追加在 asm 前缀之后，check_offsets_target 钉偏移）、`hypervisor/arch/arm64/guest/vm.c`（put：存 `CNTV_CTL_EL0`（先 mask）与 `CNTV_CVAL_EL0` 到影子，`CNTVOFF_EL2` 存入 `cntvoff` 并**关断** guest timer（CTL=mask）；load：写 per-vCPU `CNTVOFF_EL2` + 恢复 CTL/CVAL；resume 同；reset：cntvoff=0、ctl=mask）、`hypervisor/arch/arm64/timer/vtimer.c` 与 `cpu/secondary.c`（两处 `CNTVOFF_EL2=0` 常量写**删除**——初始化职责并入 load；vtimer_init 保留 CNTHCTL）、`tests/check_offsets_target.c`（钉三个新偏移）
- Modify: `tests/svm/cases/ctxsw.c`（扩展：两 vCPU 各自 arm CNTV（不同期限）→ wfi 轮转 → 各自 IRQ handler 各自 disarm 并打印 `CTXSW: t0/t1 timer fired`——**解除 R5 的 CNTV 禁令**并钉住互不踩；需要 vGIC？——PPI 27 走 `vgic_inject_hw`，需 guest 开 vGIC：**同时解除共享 profile 的 vGIC 禁令**（B2 协议已就位），用例做 `svm_gic_el1_init` + DAIF 受控开/关）、`tests/svm/expect/ctxsw.txt`（+两行 timer fired）

**Interfaces:** vtimer 状态全在 `vcpu_arch`；PPI 27 投递纪律注释落 vtimer.c（per-vCPU 到期只对 owning vCPU pending）。

- [ ] Step 1 影子字段 + put/load/resume 存取 + 常量写删除 + 偏移钉；`make test` 8× 全绿（1:1 路径 vtimer 行为不变：cntvoff=0 + 恢复原值）。
- [ ] Step 2 ctxsw 计时段 + expect；`make test-qemu-ctxsw` ALL PASS；`make test` 全绿；commit（场景升 9 的聚合变更并入本步）。

### Task 4: EL1 全量清单（Q9 纪律全程）+ VMPIDR 入 resume

**Files:**
- Modify: `hypervisor/arch/arm64/include/arch/vm.h`（`struct vcpu_arch` 尾 +`struct el1_state el1;`：推导清单 = SCTLR_EL1、TTBR0_EL1、TTBR1_EL1、TCR_EL1、MAIR_EL1、VBAR_EL1、CPACR_EL1、TPIDR_EL1、TPIDRRO_EL0、CONTEXTIDR_EL1、ESR_EL1、FAR_EL1——推导依据写注释：MMU/翻译、异常基址、trap 控制、线程指针、异常语料）、`hypervisor/arch/arm64/guest/vm.c`（put：el1 批量存（SYSREG_READ 循环展开为宏）+ `dsb`；resume：批量恢复 + `isb` + **重编 `VMPIDR_EL2`**（账单 2）+ tlbi vmalle1？——裁定：同 VM 切换 Stage-1 可不刷（同 VMID 同 Stage-2，但两 vCPU 的 Stage-1 TLB 同 VMID 可区分吗——**不可**！同 VMID 的两个 vCPU 共享 TLB 标签，切换必须 `tlbi vmalle1`——写死在 resume，注释引 ARM TLB 语料）；load：reset 值组（SCTLR=0 已有，其余 reset 值/0））、`tests/check_offsets_target.c`（钉 el1 块首偏移与 sizeof）
- Modify: `tests/svm/cases/ctxsw.c`（**盖章段全量恢复**：vCPU0 stamp SCTLR/TTBR0/MAIR/VBAR（值经 PTW 安全化：SCTLR 只动 M=0 假设下写 stamp 再回写原值？——不：盖章=写入可辨识值并跨切换校验；SCTLR stamp 必须保持 M 位为当前值（stamp=orig|0xC0DE<<32）以不关 MMU；TTBR0/MAIR 同理保位）+ 各 vCPU 读 `MPIDR_EL1` 断言各自 Aff0）

**半清单实验（Q9，工作树内）**: 先实现**故意半份**清单（只存 SCTLR/TCR）→ 跑 ctxsw → 观察盖章断言点名失败寄存器（预期 TTBR0/MAIR/VBAR canary 被另一 vCPU 覆盖或回读错值）→ 还原 → 完整实现。失败模式与截屏记 `docs/debug/m11-b2-el1-halflist.md`。

- [ ] Step 1 el1 块 + 半清单（工作树）→ ctxsw 红（点名缺失寄存器）→ 记 docs/debug/ → 还原半清单为观察记录。
- [ ] Step 2 完整清单 + resume 重编 VMPIDR + tlbi 纪律 + 偏移钉；ctxsw 全量盖章 + MPIDR 断言绿；`make test` 9 场景全绿 + 手工 gate 12/12；commit。
- [ ] Step 3 **对照 bao/xvisor**（Q12 时限后）：读 bao 的 vcpu_arch put/load 清单与 xvisor 的 regs save 集，差异表追加进 docs/debug/（只记录，不回改——差异需架构师裁决才吸收）。

### Task 5: vGIC 运行期协议——publisher-wins 合并（§8.1/D13，#1 风险）

**Files:**
- Modify: `hypervisor/arch/arm64/vgic/vgic.c`（`vgic_save` 改名语义化拆分：`vgic_save_merge(struct vcpu *v)`——LR1：`spi_shadow_pending==true` → **保影子弃活读**（远端刚发布者赢）；否则读活。LR0/2/3 与 HCR/VMCR：活读（本 pCPU 独有）。锁：整个 merge 在 `spi_lock` 内）、`hypervisor/arch/arm64/guest/vm.c`（put 调 `vgic_save_merge`）、`vgic.c` 注释重写（B1 的"无发布者"限定解除，协议生效）

**验证**: 新 SVM 场景 `ctxwspi`（或 ctxsw 第三段）：共享 pCPU 两 vCPU，**第三 pCPU 的 EL2 shell 不可用——发布者 = hypervisor 自身**：用例通过 HVC 私有命令触发对已让出 vCPU 的 `vgic_inject_spi`（宿主侧新 debug HVC？——**ponytail 裁决：不加 HVC**；改用 SGI 路径：vCPU0 经 ICC_SGI1R 向**已阻塞的 vCPU1** 发 vSGI → `vgic_sgi_trap` 发布 + kick → vCPU1 唤醒并断言收到——这正是 story 19"挂起中断跨让出存活"的可执行化，且全走既有路径）。ctxsw 加一段：vCPU1 block（wfi 独处前先自我让出）→ vCPU0 发 SGI → vCPU1 打印 `CTXSW: sgi woke blocked v1`。

- [ ] Step 1 merge 实现 + SGI 唤醒段 + expect；全套绿；commit。

### Task 6: `ICH_VTR_EL2` 探测

**Files:**
- Modify: `hypervisor/arch/arm64/vgic/vgic.c`（init 时 `SYSREG_READ(ICH_VTR_EL2) >> 0` ListRegs 字段（[4:0]，值为 LR 数−1）；`<4` → BUG halt（结构体按 4 钉死，硬件更少即不兼容）；`>4` → printk 提示"使用前 4 个 LR（QEMU=4；RK3588 再议扩容）"）——**硬编码变契约**。

- [ ] Step 1 探测 + 校验 + 日志；全套绿；commit。

### Task 7: CPU_SUSPEND → block（账单 3；retention 契约不变）

**Files:**
- Modify: `hypervisor/arch/arm64/vmexit/vpsci.c`（`psci_cpu_suspend`：`VCPU_EV_BLOCK`→不入队→`hv_restore` 解栈（替代 EL2 原地 wfi——共享 pCPU 不再被停）；唤醒后循环重拾 → resume → eret 回 guest → `x0=SUCCESS`——**返回路径变形**：suspend 的 SUCCESS 在 guest 恢复执行前由谁写 `regs.x[0]`？——阻塞前先写 `regs->x[0]=SUCCESS`（retention 语义：醒来时 x0 即 SUCCESS），再 BLOCK 解栈）、`tests/svm/cases/psci.c`（两段 CPU_SUSPEND 断言语义不变——实证契约保持）

- [ ] Step 1 实现 + psci 场景绿 + 手工 gate（Linux 无 idle-states 不会调，行为兜底不变）；全套绿；commit。

### Task 8: ADR-0019 + 收尾

- [ ] ADR-0019《两层状态划分与 EL1 清单》：Decision = 汇编每异常存 GPR 帧（不变）；C 每切换存 el1 块 + vGIC merge + CNTV 影子（清单全文）；EV_YIELD 保留为有竞争路径（ADR-0018 待决落定）；resume 的 VMPIDR/tlbi 纪律；否决项：惰性 EL1（CPTR 陷阱法）、全动态 LR 数。含 Mermaid。
- [ ] psci 用例加 vCPU1 CPU_OFF→再 CPU_ON 往返段（账单 6 自动化）；ctxsw/psci/手工 gate 全绿；里程碑骨架 B2 勾选；本 plan 状态行；devlog + 日报。

---

## 执行方式（协议已定）

主会话 executing-plans；测试专家 + 关卡②后 `--no-ff` 合回。分支：`feature/m11-b2-el1-vgic-vtimer`。粗估 3–5 loop 日。**Q12 逃生舱**：Task 4/5 任一卡壳超一个工作日即读 bao/xvisor 对应实现（spec §执行纪律）。
