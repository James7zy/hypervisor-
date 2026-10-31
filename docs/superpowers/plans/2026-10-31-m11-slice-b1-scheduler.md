# M11 片 B1 — 调度器与 WFI 让出（GPR 级切换）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `common/sched/` 调度器落地（纯逻辑 runqueue + 调度循环）+ WFI 协作让出，使共享亲和（ctxsw profile）下同 VM 两 vCPU 在一个 pCPU 上轮转——GPR/ELR 级正确性，EL1 全量与 vGIC/vtimer 运行期协议留给 B2。

**Architecture:** 不做深 C 栈切换：WFI 让出 = 推进 ELR + 状态→RUNNABLE + `hv_restore()` 解栈回本 pCPU 运行循环，循环改为 `sched_pick()` 取下一个（可能还是自己）再载入再 eret——复用片 A 建好的循环形状。`vcpu_arch_load` 保持 fresh-boot 语义，新增 `vcpu_arch_resume(v)`（仅 `vgic_restore`）服务已启动 vCPU 的再入；`launched` 标志区分两条路径。

**Tech Stack:** freestanding C11、host gcc 单测（sched_q 纯逻辑 RED→GREEN）、QEMU gate（ctxsw 转绿并进聚合，场景 7→8）。

**Spec:** `docs/superpowers/specs/2026-10-30-m11-vcpu-scheduler-design.md` §5 片 B（本片 = 其拆分预案的 **B1**：sched_q + 调度循环 + WFI 让出，detector 只断言 GPR/ELR 级；**B2** = EL1 全量 + vGIC/vtimer 协议 + block/wake，B1 关卡②后另写 plan。**拆分决定提请关卡①裁定**）

## Global Constraints

- **arch-neutral**：`sched_q.c` 只 include `<types.h>` 与 `<sched_q.h>`， opaque `struct vcpu *`，不含 vm.h/percpu/printk/锁——host 可测。
- host 纯逻辑 RED→GREEN commit 顺序可见（先例：片 A `vcpu_state`）。
- 新钩子（`vcpu_arch_put`/`vcpu_arch_resume`）同 commit 补 x86 stub。
- 每 Task 完成时 `make test` 全绿；1:1 配置行为**必须等价**（调度器对独占 pCPU 是退化的单元素队列）。
- `vcpu_arch_load` 的 fresh-boot 语义（vgic_init 清白 + EL1 中和）不得用于已启动 vCPU 的再入——`launched` 标志分叉，B2 扩为完整 put/load 协议。
- 状态转移仍走 `vcpu_state_step`；调度器不绕过状态机。
- Makefile 无头依赖：改头文件后 `make clean`。
- 新文档中文；架构图 Mermaid。

## Review Focus

| # | 失败模式 | 钉住它的测试/步骤 |
|---|---|---|
| 1 | 独占 pCPU 退化为空/丢 vCPU（1:1 配置回归） | 7 既有场景全绿（调度器不可见）+ host 测试单元素队列 |
| 2 | 让出后队列丢失唯一 vCPU → pCPU 空转挂死 | sched_q 空队/单元素重排 host 断言 + ctxsw 轮转可见 |
| 3 | WFI 让出未推进 ELR → 回到 wfi 死循环 | vmexit WFI 分支 `elr += 4` 保持 + ctxsw 轮转计数 |
| 4 | 已启动 vCPU 被当 fresh-boot 清白（vGIC/EL1 被抹） | `launched` 分叉 + ctxsw 两 vCPU 各自存活多轮 |
| 5 | CPU_ON 入队与 pick 竞态（kick 前 vCPU 不在队列） | 入队先于状态发布（同片 A 序）+ psci 用例回归 |

---

### Task 1: `sched_q` 纯逻辑模块 + host 测试（RED→GREEN）

**Files:**
- Create: `hypervisor/include/sched_q.h`、`hypervisor/common/sched/sched_q.c`、`tests/host/sched_q_test.c`
- Modify: `tests/host/host_test.h`（+原型）、`tests/host/runner.c`（+调用）、`Makefile`（HOST_TEST_SRCS + 依赖行 + `hypervisor/include/sched_q.h`）、`hypervisor/Makefile`（hv-objs += `common/sched/sched_q.o`）

**Interfaces:**
- Produces:
```c
#define SCHED_Q_MAX 8                    /* 每 pCPU 队列容量（= VCPUS 上限粒度，够 1:N 数据） */
struct sched_q { struct vcpu *slots[SCHED_Q_MAX]; u8 head, count; };
void    sched_q_init(struct sched_q *q);
int     sched_q_enqueue(struct sched_q *q, struct vcpu *v);   /* 0 / -1 满 */
struct vcpu *sched_q_dequeue(struct sched_q *q);              /* 头部弹出，空返 NULL */
struct vcpu *sched_q_peek(const struct sched_q *q);           /* 看头不弹 */
int     sched_q_remove(struct sched_q *q, struct vcpu *v);    /* 0 / -1 不在 */
bool    sched_q_empty(const struct sched_q *q);
u32     sched_q_count(const struct sched_q *q);
```
- 语义：FIFO；enqueue 已在队列的 vCPU 返回 0 幂等（计数不变）——防 yield 路径重复入队。RR = dequeue+enqueue 或 peek+rotate，由 sched.c 组合，sched_q 只做队列原语。

- [ ] **Step 1** 写失败测试：init 后 empty/count==0；enqueue 三个伪指针（&(u64[]){1,2,3} 类 opaque）顺序 dequeue 一致；dequeue 空返 NULL；enqueue 重复同指针幂等；remove 中间元素保序；容量上限第 9 个 -1。接线三处。**RED commit**（header+tests+wiring，无 .c 实现，链接失败）。
- [ ] **Step 2** 最小实现（环形/线性皆可，count==SCHED_Q_MAX 即满）。**GREEN commit**（含 hv-objs 接入，交叉侧暂无调用者）。
- [ ] **Step 3** `make test` 全绿回归。

### Task 2: `vcpu_arch_put`/`vcpu_arch_resume` 钩子 + `launched` 分叉

**Files:**
- Modify: `hypervisor/include/vm.h`（+`bool launched;` 进 struct vcpu 通用尾；两钩子原型，注释写明 fresh-boot vs resume 契约）、`hypervisor/arch/arm64/guest/vm.c`（`vcpu_arch_resume(v)` = `vgic_restore(v)` 仅此；`vcpu_arch_put(v)` = `vgic_save(v)` + `dsb ish`，B1 最小体，B2 扩 EL1 全量）、`hypervisor/arch/x86/stub/hooks.c`（+两空实现）、`tests/check_offsets_target.c`（state 后新增 launched 偏移断言，探针法）、`hypervisor/arch/arm64/vmexit/vpsci.c`（CPU_ON 授权处置 `v->launched = false`；首次 LOAD 后置 true——LOAD 点在运行循环，改两循环）

**Interfaces:**
- Produces: `void vcpu_arch_put(struct vcpu *v)`（从本 pCPU 卸下并保存）；`void vcpu_arch_resume(struct vcpu *v)`（已启动 vCPU 再入：仅恢复 vGIC 影子，**不** vgic_init、**不** EL1 中和）。
- 消费方（Task 3/4）：运行循环按 `v->launched` 选 `vcpu_arch_load`（false）或 `vcpu_arch_resume`（true）。

- [ ] Step 1 实现 + stub + 断言 + 探针值钉入；`make test` + `make host-test` 全绿；commit。

### Task 3: 运行循环接入调度器（1:1 行为等价）

**Files:**
- Create: `hypervisor/include/sched.h`、`hypervisor/common/sched/sched.c`
- Modify: `hypervisor/common/vm/vm.c`（vm_init：每 RUNNABLE vCPU 按 `vcpu_pcpu[]` 入对应队列；两处运行循环的 `v = 固定` 换 `v = sched_pick(pcpu)`；LOAD 分叉 launched）、`hypervisor/arch/arm64/cpu/secondary.c`（同上分叉）、`hypervisor/arch/arm64/vmexit/vpsci.c`（CPU_ON：授权后 `sched_enqueue(target)` 先于状态发布，再 kick）、`hypervisor/Makefile`（hv-objs += sched.o）

**Interfaces:**
- Produces（sched.h，唯一公开面收窄）:
```c
void sched_init(void);                          /* vm_init 末尾建 NR_CPUS 个队列并装填 */
struct vcpu *sched_pick(u32 pcpu);              /* 空→NULL；非空轮转出下一个（dequeue） */
void sched_yield_cur(void);                     /* 当前 vCPU 重新入队（RUNNABLE 侧已由调用方置态） */
void sched_enqueue(struct vcpu *v);             /* CPU_ON/wake 用（幂等） */
extern struct sched_q sched_q[NR_CPUS];         /* 仅 sched_q 测试窥视用可省——保持私有更佳，改用计数断言 */
```
- `sched.c` 可 include vm.h/percpu/printk（粘合层不受 host 约束）。

- [ ] Step 1 sched_init/pick/yield/enqueue 实现 + 两循环改造 + CPU_ON 入队。1:1 下 pick 恒返回该 pCPU 唯一 vCPU（yield 让出自己→重新入队→pick 又是自己→resume 再入）——**行为等价性由此构造保证**。
- [ ] Step 2 `make test` 全绿（7 场景 = 回归网）+ host 85；commit。

### Task 4: WFI 让出路径

**Files:**
- Modify: `hypervisor/arch/arm64/vmexit/vmexit.c`（`handle_wfx` 拆分：WFE 维持放行；WFI → `elr += 4` 保持 + `sched_yield_cur()`（Task 3 接口）：当前 vCPU 经 `VCPU_EV_YIELD` 置 RUNNABLE + enqueue + `hv_restore()` 解栈）、`hypervisor/include/vcpu_state.h` 与 `common/vm/vcpu_state.c`（**新增事件 `VCPU_EV_YIELD`：RUNNING→RUNNABLE**，协作让出专用边；host 测试矩阵同 commit 更新为 RED→GREEN 补对）

- [ ] Step 1 状态机加 YIELD 边：host 测试先 RED（新断言合法/非法边）→ GREEN（两 commit）。
- [ ] Step 2 WFI 分支接 yield + 解栈；`make test` 全绿（现 guest 群不共享 pCPU，WFI 让出后 pick 回自己 = 与放行等价，7 场景仍绿）；commit。

### Task 5: ctxsw 用例转绿进聚合（场景 7→8）

**Files:**
- Modify: `tests/svm/cases/ctxsw.c`（重写为 GPR 级轮转断言：vCPU0 设 x19-x28 canary + 自有栈标记 → CPU_ON vCPU1（共享 pCPU0）→ vCPU1 打印并 WFI 让出 → 轮转 N 轮双方计数递增 → 终验 x19-x28 canary 不变；EL1 盖章段注释降级为"B2 扩展"，VBAR/CPACR/TPIDRRO 断言**移除**——B1 切换不换 EL1 sysreg，共享 profile 的两 vCPU 各自重设这些寄存器属预期）、`tests/svm/expect/ctxsw.txt`（轮转行、双方 alive 计数、canary 行）、`Makefile`（`test` 聚合 += `test-qemu-ctxsw`；目标注释更新"已于 B1 转绿"）
- vCPU1 侧：入口 stub 自设 SP 后进入 C 体，循环 `{ print "CTXSW: vCPU1 round %d"; wfi; }`；vCPU0 同样 `{ print round; wfi; }` N 轮后校验 canary 并 done。

- [ ] Step 1 重写用例与期望 → `make test-qemu-ctxsw` ALL PASS（真实轮转 = B1 的头条声明可执行化）。
- [ ] Step 2 `make test` 全绿（**8 场景**）+ host；commit。

### Task 6: 收尾

- [ ] ADR-0018《协作式先行与 WFI 让出点》（Decision：调度循环 = 运行循环内 pick，让出 = 状态转移 + 解栈复入，不做 C 栈切换；否决项：长跳转式 switch_to、立即抢占）。落 `docs/adr/0018-*.md`，中文，含 Mermaid。
- [ ] 手工双 VM Linux gate（`sh /tmp/m11a-gate.sh`）12 项回归 + 输出摘录进日报（1:1 不受调度器影响的主张的实证）。
- [ ] 里程碑 plan 骨架 B1 勾选（B2 行拆出）+ 本 plan 状态行 + devlog/日报。

---

## 执行方式（协议已定）

主会话 executing-plans；测试专家独立验收 + 架构师关卡②后 `--no-ff` 合回 develop。分支：`feature/m11-b1-scheduler`。粗估 3–4 loop 日。

## B2 展望（本 plan 不实现，关卡②后另写）

EL1 批量保存/恢复清单（先推导→ctxsw 盖章段恢复→对照 bao/xvisor 记 docs/debug/）、`vcpu_arch_put` 扩全量、vGIC 运行期保存协议重设计（影子 vs 活 LR 谁赢、wake 合并；`vgic_save` 第一个真实调用点）、`ICH_VTR_EL2` LR 数探测替换 `ich_lr[4]`、per-vCPU vtimer（CNTVOFF/CNTV 影子 + PPI27 投递纪律）、block/wake（WFI→BLOCKED + 注入唤醒）、"挂起虚拟中断跨让出存活"断言、ADR-0019（两层状态划分）。
