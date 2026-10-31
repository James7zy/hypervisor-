# M11 片 B2 — EL1 全量、vGIC/vtimer 运行期协议、block/wake 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 切换从 GPR 级升到**EL1 全量**（含 per-vCPU vtimer 与 VMPIDR 身份），vGIC 影子获得运行期协议（publisher-wins 合并），WFI 获得 block/wake 语义（有竞争让出、独处阻塞），`ICH_VTR_EL2` 探测替换 LR 数硬编码假设——共享 pCPU 上可跑会编程 CNTV/用中断的用例。

**Architecture:** B1 的 put/resume 钩子是全部生长点：put 扩为 vGIC 合并读 + CNTV 影子 + EL1 批量存；resume 对称恢复 + VMPIDR 重编。WFI 三分：pending vIRQ→直续、队列有他者→yield（B1 路径保留）、独处→BLOCK+出队+解栈，注入路径经 `sched_wake` 唤醒。双循环先上提为公共函数（关卡②账单 1），后续所有任务只改一处。

**Tech Stack:** freestanding C11、host gcc 单测、QEMU gate（8 场景 + 扩展）、`../xvisor`、`../bao-hypervisor` 对照（Q12：卡壳一天才读）。

**Spec:** `docs/superpowers/specs/2026-10-30-m11-vcpu-scheduler-design.md` §5-B 余项

> **关卡①:** 2026-10-31 APPROVE_WITH_CHANGES（R1–R6 已并入）。关键裁决：R1 block 点必须
> 走与 put 相同的保存路径（不做切换的保存）；R2 **BLOCK 路径不关断物理 timer**（rot 才
> quiesce——锁死唯一唤醒源；xvisor save-then-IMASK 先例），共享 pCPU 上自定时器唤醒的
> 已知缺口记 ponytail 注 + ADR-0019（触发条件：首个依赖自身 timer 的共享阻塞场景）；
> R3 `vgic_has_pending` 读**活** ICH_LRx（影子与硬件发散——HW=1 LR 与 guest EOI 均无影子
> 更新）；R4 SGI 唤醒场景宿主到 psci 的 1:1 profile（共享 profile 上 BLOCK 分支不可达且
> `sgi_pending` 按 pCPU 键控误投），共享 profile 跨 vCPU SGI = 已知缺口（触发：首个
> intra-VM 共享 SGI 消费者）；R5 账单 5（快路径不变式复审）入 T2、`switch_to` 措辞裁定
> 入 T8（宣布被 put/load/resume 三元组取代）；R6 第四注入入口 `vgic_inject_sw` 同挂
> wake、场景数保持 8。、§4（两层划分）、§8.1/8.2（风险）、D4/D13；ADR-0018 待决两项随本片裁定。

## Global Constraints

- **Q9/Q12 纪律**：EL1 清单先自行推导、detector 验证、再对照 bao/xvisor 记差异（`docs/debug/`）；**半清单故意失败实验 = 工作树内验收操作**，不留红 commit。
- arch-neutral 约束照旧（host 模块零 arch 头）；新钩子同 commit x86 stub。
- 每 Task 后 `make test` 8 场景全绿（扩展落在既有场景内，无新增聚合目标）；改头文件 → `make clean`。
- PSCI 接口面冻结不变；CPU_SUSPEND retention 契约（返回 SUCCESS + 上下文完整）保持。
- R5 边界**随协议落地逐项解除**（解除项须在新用例/注释中显式声明：CNTV ✓ 中断 ✓；共享跑 Linux 仍留待手工 gate 验证后）。
- gate 绿后才 commit（两次教训在案）；新文档中文。

## Review Focus

| # | 失败模式 | 钉住它的测试/步骤 |
|---|---|---|
| 1 | put 的活 LR 读覆盖远端刚发布的影子（vuart RX 打到已换下的 vCPU） | publisher-wins 合并代码 + 注入-唤醒跨让出场景（Task 6/新场景） |
| 2 | 共享 pCPU 的 CNTV 互踩（两 vCPU 各自的 timer 互相清零） | per-vCPU CNTV 影子存取 + ctxsw 计时轮转扩展（Task 3） |
| 3 | BLOCK 后无人唤醒 → vCPU 永失（注入路径漏挂 wake（四入口：spi/sgi/hw/sw）） | sched_wake 挂到全部四个注入入口 + vtimer 唤醒场景（Task 2） |
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
- Modify: `hypervisor/common/sched/sched.c`（+`void sched_wake(struct vcpu *v)`：仅当状态为 BLOCKED 时 `vcpu_state_step(WAKE)` + 锁内 `sched_enqueue` + `cpu_arch_kick`（其他状态幂等返回——**仅从 BLOCKED 唤醒**，防止把 RUNNING/RUNNABLE 重复入队）；快路径不变式复审注记：sched_wake 成为 CPU_ON 之外第二个跨 pCPU 入队点，五条不变式（静态亲和/cur_vcpu 即亲和者/RUNNING 永不入队/无迁移/wake 仅自 BLOCKED）重新走查并记 ADR-0019——关卡②账单 5）、`hypervisor/include/sched.h`（+原型）、`hypervisor/arch/arm64/vmexit/vmexit.c`（WFI 三分：`vgic_has_pending(v)`（**读活 `ICH_LR0..3_EL2` 的 state 位**——影子与硬件发散：HW=1 LR 的 pending 来自物理线、guest IAR/EOI/DIR 推进 LR 状态均无影子更新，影子不可作代理）为真→**先推进 ELR 再解栈**（快路径重入 prev RUNNING 不会再看 ELR）；队列有他者→B1 yield 路径不变；否则 **block 助手**：`VCPU_EV_BLOCK` + 不入队 + **与 put 相同的保存**（关卡① R1：不做切换的保存——共享 pCPU 上兄弟的 load 会覆写本 vCPU 的活 vGIC/CNTV/EL1 状态，醒来 resume 恢复的必须是已存的影子）+ 解栈）、`hypervisor/arch/arm64/vgic/vgic.c` + `vgic_sgi.c` + `irq_handler.c` 的**四个**注入入口（`vgic_inject_spi/sgi/hw/sw`——R6：sw 是早 SMP VENG1=0 兜底也会 pends 一个可能阻塞的 vCPU——成功后调 `sched_wake(target)`）、`hypervisor/arch/arm64/guest/vm.c`（block 助手落地处，CPU_SUSPEND 复用）

**裁定（ADR-0018 待决项落定，Task 8 记入 ADR-0019）**：EV_YIELD 边**保留**（有竞争路径），BLOCK 边补齐（独处路径）；两语义互补非替代。

**Interfaces:** Produces `sched_wake(struct vcpu *v)`；`vgic_has_pending(struct vcpu *v)`。CPU_SUSPEND 改造（Task 7）复用 BLOCK 路径。

- [ ] Step 1 sched_wake + WFI 三分 + 三注入入口挂 wake；`make test` 8× 全绿（既有场景无 BLOCK 触发——Linux idle wfi 独处时阻塞等 timer PPI：`vgic_inject_hw` 的 wake 唤醒之；手工 gate 必须仍 12/12）。
- [ ] Step 2 手工 gate 12/12（Linux idle 阻塞路径的首次真实验证）；commit。

### Task 3: per-vCPU vtimer（D4——首个会编程 CNTV 的共享用例）

**Files:**
- Modify: `hypervisor/arch/arm64/include/arch/vm.h`（`struct vcpu_arch` 尾部 +`u64 cntvoff, cntv_ctl, cntv_cval;`——追加在 asm 前缀之后，check_offsets_target 钉偏移）、`hypervisor/arch/arm64/guest/vm.c`（**rot/put 路径**：存 `CNTV_CTL_EL0`（先 IMASK）与 `CNTV_CVAL_EL0` 到影子，`CNTVOFF_EL2` 存入 `cntvoff`（xvisor save-then-IMASK 先例：期限已过则 resume 恢复未掩 CTL 时电平 PPI 重新升起）；**block 路径（关卡① R2）：存影子但物理 timer 保持 LIVE（armed/unmasked 原样）**——block 是独处路径，pCPU 落 cpu_arch_idle，PPI 到 EL2 → `vgic_inject_hw` 打到 cur_vcpu==阻塞者 → sched_wake 重入队 → 快路径直入（活状态未动，R1 的保存是共享 pCPU 后续 resume 的保险）；load：写 per-vCPU `CNTVOFF_EL2` + 恢复 CTL/CVAL；resume 同；reset：cntvoff=0、ctl=mask）、`hypervisor/arch/arm64/timer/vtimer.c`（**已知缺口 ponytail 注**：阻塞 vCPU 的 pCPU 后来跑了别的 vCPU 时，其自身 timer 的 PPI 会误投 cur_vcpu——EL2 背景定时器重设计（KVM/bao 式 deadline 模拟）的触发条件 = 首个依赖自身 timer 的共享阻塞场景）、`cpu/secondary.c`（`CNTVOFF_EL2=0` 常量写**删除**——并入 load）、`tests/check_offsets_target.c`（钉三个新偏移）
- Modify: `tests/svm/cases/ctxsw.c`（扩展：两 vCPU 各自 arm CNTV（不同期限）→ wfi 轮转 → 各自 IRQ handler 各自 disarm 并打印 `CTXSW: t0/t1 timer fired`——**解除 R5 的 CNTV 禁令**并钉住互不踩；需要 vGIC？——PPI 27 走 `vgic_inject_hw`，需 guest 开 vGIC：**同时解除共享 profile 的 vGIC 禁令**（B2 协议已就位），用例做 `svm_gic_el1_init` + DAIF 受控开/关）、`tests/svm/expect/ctxsw.txt`（+两行 timer fired）

**Interfaces:** vtimer 状态全在 `vcpu_arch`；PPI 27 投递纪律注释落 vtimer.c（per-vCPU 到期只对 owning vCPU pending）。

- [ ] Step 1 影子字段 + put/load/resume 存取 + 常量写删除 + 偏移钉；`make test` 8× 全绿（1:1 路径 vtimer 行为不变：cntvoff=0 + 恢复原值）。
- [ ] Step 2 ctxsw 计时段 + expect（**过渡期安全声明（关卡① A1）**：本步解除共享 profile 的 CNTV/vGIC 禁令时 publisher-wins 合并（T5）尚未落地——安全依据 = 单 pCPU 上全部发布者皆本地 + 用例期间无控制台输入 + put 的 CTL 掩蔽使 PPI 27 只在已载入的 owner 上触发；T5 落地后此声明作废）；`make test-qemu-ctxsw` ALL PASS；`make test` 全绿（8 场景）；commit。

### Task 4: EL1 全量清单（Q9 纪律全程）+ VMPIDR 入 resume

**Files:**
- Modify: `hypervisor/arch/arm64/include/arch/vm.h`（`struct vcpu_arch` 尾 +`struct el1_state el1;`：推导清单 = SCTLR_EL1、TTBR0_EL1、TTBR1_EL1、TCR_EL1、MAIR_EL1、VBAR_EL1、CPACR_EL1、TPIDR_EL1、TPIDRRO_EL0、CONTEXTIDR_EL1、ESR_EL1、FAR_EL1——推导依据写注释：MMU/翻译、异常基址、trap 控制、线程指针、异常语料）、`hypervisor/arch/arm64/guest/vm.c`（put：el1 批量存（SYSREG_READ 循环展开为宏）+ `dsb`；resume：批量恢复 + `isb` + **重编 `VMPIDR_EL2`**（账单 2）+ tlbi vmalle1？——裁定：同 VM 切换 Stage-1 可不刷（同 VMID 同 Stage-2，但两 vCPU 的 Stage-1 TLB 同 VMID 可区分吗——**不可**！同 VMID 的两个 vCPU 共享 TLB 标签，切换必须 `tlbi vmalle1`——写死在 resume，注释引 ARM TLB 语料）；load：reset 值组（SCTLR=0 已有，其余 reset 值/0））、`tests/check_offsets_target.c`（钉 el1 块首偏移与 sizeof）
- Modify: `tests/svm/cases/ctxsw.c`（**盖章段全量恢复**：vCPU0 stamp SCTLR/TTBR0/MAIR/VBAR（值经 PTW 安全化：SCTLR 只动 M=0 假设下写 stamp 再回写原值？——不：盖章=写入可辨识值并跨切换校验；SCTLR stamp 必须保持 M 位为当前值（stamp=orig|0xC0DE<<32）以不关 MMU；TTBR0/MAIR 同理保位）+ 各 vCPU 读 `MPIDR_EL1` 断言各自 Aff0）

**盖章值安全集（关卡① A2）**：全值 canary 用惰性数据寄存器 TPIDR_EL1/TPIDRRO_EL0/CONTEXTIDR（最强哨兵）；RES0 高位 stamp（SCTLR/TTBR0/VBAR 用 `orig|0xC0DE<<32`）**依赖 QEMU 存储 RES0 写入——TCG 可能掩位使 canary 空转（两 vCPU 都存恢复 0）**：盖章时先写-读回验证可见性，"有牙齿"的 canary 清单记入 docs/debug；盖章窗内 SCTLR.M 必须 0（用例为裸金属 MMU-off——本就满足）且不得发生 EL1 异常（VBAR stamp 是垃圾向量基址）。

**半清单实验（Q9，工作树内）**: 先实现**故意半份**清单（只存 SCTLR/TCR）→ 跑 ctxsw → 观察盖章断言点名失败寄存器（预期 TTBR0/MAIR/VBAR canary 被另一 vCPU 覆盖或回读错值）→ 还原 → 完整实现。失败模式与截屏记 `docs/debug/m11-b2-el1-halflist.md`。

- [ ] Step 1 el1 块 + 半清单（工作树）→ ctxsw 红（点名缺失寄存器）→ 记 docs/debug/ → 还原半清单为观察记录。
- [ ] Step 2 完整清单 + resume 重编 VMPIDR + tlbi 纪律 + 偏移钉；ctxsw 全量盖章 + MPIDR 断言绿；`make test` 9 场景全绿 + 手工 gate 12/12；commit。
- [ ] Step 3 **对照 bao/xvisor**（Q12 时限后）：读 bao 的 vcpu_arch put/load 清单与 xvisor 的 regs save 集，差异表追加进 docs/debug/（只记录，不回改——差异需架构师裁决才吸收）。

### Task 5: vGIC 运行期协议——publisher-wins 合并（§8.1/D13，#1 风险）

**Files:**
- Modify: `hypervisor/arch/arm64/vgic/vgic.c`（`vgic_save` 改名语义化拆分：`vgic_save_merge(struct vcpu *v)`——LR1：`spi_shadow_pending==true` → **保影子弃活读**（远端刚发布者赢）；否则读活。LR0/2/3 与 HCR/VMCR：活读（本 pCPU 独有）。锁：整个 merge 在 `spi_lock` 内）、`hypervisor/arch/arm64/guest/vm.c`（put 调 `vgic_save_merge`）、`vgic.c` 注释重写（B1 的"无发布者"限定解除，协议生效）

**验证（关卡① R4 重塑——宿主 psci 用例的 1:1 profile）**: 共享 profile 上 BLOCK 分支不可达（唯一 wfi 者即运行者）且 `sgi_pending` 按 pCPU 键控会误投 cur_vcpu——场景改宿主 **psci 用例**（svm profile {0,1}）：vCPU1 在 pCPU1 上独处 wfi→block（保存走 R1 助手）→ pCPU0 的 vCPU0 经 ICC_SGI1R 向 vCPU1 发 vSGI → `vgic_sgi_trap` 发布 + kick → pCPU1 唤醒 → vCPU1 resume 并打印 `SVM: SGI woke blocked vCPU1`——端到端验证 R1 的 block-保存 + resume 路径（ctxsw 无法覆盖此路径）。**SGI 路由裁定（关卡②账单"B2 决策"项落定）**：B2 维持 pCPU 键控位图（全部 1:1 profile 上跨 vCPU SGI 端到端正确）；共享 profile 跨 vCPU SGI = 已知缺口（误投当前 vCPU），`vgic_sgi.c` ponytail 注 + ADR-0019 条目；per-vCPU LR 投递的触发条件 = 首个 intra-VM 共享 SGI 消费者（片 C 的 2×2-on-2 每 VM 仍 1:1，触发在超配/共享 SVM profile）。

- [ ] Step 1 merge 实现 + SGI 唤醒段 + expect；全套绿；commit。

### Task 6: `ICH_VTR_EL2` 探测

**Files:**
- Modify: `hypervisor/arch/arm64/vgic/vgic.c`（init 时 `SYSREG_READ(ICH_VTR_EL2) >> 0` ListRegs 字段（[4:0]，值为 LR 数−1）；`<4` → BUG halt（结构体按 4 钉死，硬件更少即不兼容）；`>4` → printk 提示"使用前 4 个 LR（QEMU=4；RK3588 再议扩容）"）——**硬编码变契约**。

- [ ] Step 1 探测 + 校验 + 日志；全套绿；commit。

### Task 7: CPU_SUSPEND → block（账单 3；retention 契约不变）

**Files:**
- Modify: `hypervisor/arch/arm64/vmexit/vpsci.c`（`psci_cpu_suspend`：**先写 `regs->x[0]=SUCCESS`**（DEN0022 retention：醒时 x0=SUCCESS、其余上下文原样——关卡① A5 核验，HVC 的 ELR 已指向 hvc 之后）→ **共享 block 助手**（Task 2 的 R1 保存路径；非空队列时按 put 式 quiesce timer——R2：retention 允许任意中断唤醒）→ `hv_restore` 解栈；唤醒后循环重拾 → resume → eret）、`tests/svm/cases/psci.c`（两段 CPU_SUSPEND 断言语义不变——实证契约保持）

- [ ] Step 1 实现 + psci 场景绿 + 手工 gate（Linux 无 idle-states 不会调，行为兜底不变）；全套绿；commit。

### Task 8: ADR-0019 + 收尾

- [ ] ADR-0019《两层状态划分与 EL1 清单》：Decision = 汇编每异常存 GPR 帧（不变）；C 每切换存 el1 块 + vGIC merge + CNTV 影子（清单全文）；EV_YIELD 保留为有竞争路径（ADR-0018 待决一落定）；**`switch_to` 措辞裁定**（ADR-0018 待决二落定）：spec §4"switch_to 唯一公开入口"术语**宣布被 put/load/resume 三元组取代**（无 switch_to 符号存在，循环即调度点）；**快路径不变式集**（静态亲和/cur_vcpu 即亲和者/RUNNING 永不入队/无迁移/wake 仅自 BLOCKED——关卡②账单 5 复审结论）；**已知缺口条目**：共享 pCPU 自 timer 唤醒（触发=首个依赖自身 timer 的共享阻塞）、共享 profile 跨 vCPU SGI（触发=intra-VM 共享 SGI 消费者）、PAC 密钥不入清单（片 C/Linux 共享前再议）；tlbi vmalle1 的准确理由（同 VM 两 vCPU 共享 mm/ASID 时为保守冗余；免失效条件=RK3588 实测成本 + §8.3 VMID 隔离）；否决项：惰性 EL1（CPTR 陷阱法）、全动态 LR 数。含 Mermaid。
- [ ] psci 用例加 vCPU1 CPU_OFF→再 CPU_ON 往返段（账单 6 自动化）；ctxsw/psci/手工 gate 全绿；里程碑骨架 B2 勾选；本 plan 状态行；devlog + 日报。

---

## 执行方式（协议已定）

主会话 executing-plans；测试专家 + 关卡②后 `--no-ff` 合回。分支：`feature/m11-b2-el1-vgic-vtimer`。粗估 3–5 loop 日。**Q12 逃生舱**：Task 4/5 任一卡壳超一个工作日即读 bao/xvisor 对应实现（spec §执行纪律）。
