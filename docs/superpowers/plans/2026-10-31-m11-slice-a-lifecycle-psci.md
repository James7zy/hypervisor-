# M11 片 A — vCPU 生命周期与 PSCI 语义 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 每 vCPU 运行状态机落地、`vm_config` per-vCPU 亲和取代结构性 1:1、`vm.off` 删除、PSCI 五函数按 vCPU 语义重写（接口面不变）、全 pCPU 启动 + idle——行为仍等价于 1:1 绑定，为片 B 调度器铺轨。

**Architecture:** 状态机抽成 arch-neutral 纯函数模块（host RED→GREEN）；亲和进配置数据 + 启动期校验（编译期断言删除）；secondary_main 改为"等授权→pick→运行循环"，CPU_ON 经 kick 而非 cpu_arch_power_on；停机语义由 `vm.off` 换成每 vCPU 状态 + kick 观察协议（dsb 先于 kick 不变量保持）。

**Tech Stack:** freestanding C11（EL2）、host gcc 单测、QEMU gate（7 场景 + 4 静态检查）、新独立场景 `test-qemu-ctxsw`（受控红，不进聚合）。

**Spec:** `docs/superpowers/specs/2026-10-30-m11-vcpu-scheduler-design.md`（合并版 spec，本片实现其 §5 片 A；裁定 9.1–9.3 已并入）

## Global Constraints

- **PSCI 接口面冻结**（前置 B，ADR-0017）：函数集合、SMCCC 版本、错误码不变；本片只改语义。
- **arch-neutral**：`vcpu_state` 模块只 include `<types.h>`；进 `HOST_TEST_SRCS` 的模块禁 ARM64 内联汇编/`arch/` 头/percpu/spinlock/printk。
- **host 纯逻辑 RED→GREEN commit 顺序可见**（loop 纪律，自本片生效）。
- 新钩子（`cpu_arch_idle`/`cpu_arch_kick`）同 commit 补 `arch/x86/stub/hooks.c` 空实现（`make test-x86-stub` 链接执法）。
- 每 Task 完成时：`make test` 全绿（4 静态 + 7 QEMU）+ `make host-test` 全绿；唯 Task 6 的 `test-qemu-ctxsw` 是**独立目标受控红**，绝不进聚合 `test`。
- 停机协议内存序不变量：**先写状态、`dsb ish`、后 kick**（沿用今日 `m->off` 协议的形状）。
- 新文档中文；架构图 Mermaid。

## Review Focus

| # | 失败模式 | 钉住它的测试/步骤 |
|---|---|---|
| 1 | CPU_ON 授权后 kick 丢失或状态未发布（序：kick 先于状态可见）→ 目标永不运行 | 代码序固定（状态→dsb→kick）+ psci 用例 vCPU1 join 断言（Task 5） |
| 2 | CPU_OFF 回归旧"整 VM 停车"语义 | psci 用例 "vCPU0 alive after sibling CPU_OFF"（Task 5） |
| 3 | ALREADY_ON 误判：`percpu[].online` 在新启动流下恒真（全 pCPU 预在线） | ALREADY_ON 改判 vCPU 状态；psci 用例对已 ON 目标二次 CPU_ON 断言 -4（Task 5） |
| 4 | 未授权 pCPU 收到 kick 时 `cur_vcpu` 仍为 NULL → EL2 空指针挂死 | secondary_main **先设 cur_vcpu 再进授权等待**（Task 4 构造固定 + 评审）；ctxsw profile 下全 pCPU 启动实测（Task 6） |
| 5 | OFF→再 ON 循环（Linux cpu hotplug）不可用：CPU_OFF 后 pCPU 永久失去服务能力 | 运行循环回到授权等待而非 halt（Task 4）+ 手工 Linux gate `cpu1/online` off→on（Task 7） |

---

### Task 1: `vcpu_state` 纯逻辑模块 + host 测试（首个 RED→GREEN commit 对）

**Files:**
- Create: `hypervisor/include/vcpu_state.h`、`hypervisor/common/vm/vcpu_state.c`、`tests/host/vcpu_state_test.c`
- Modify: `tests/host/host_test.h`（+`void test_vcpu_state(void);`）、`tests/host/runner.c`（+调用）、`Makefile`（HOST_TEST_SRCS 追加两文件；规则依赖行追加 `hypervisor/include/vcpu_state.h`——关卡②教训）

**Interfaces:**
- Produces: `enum vcpu_state { VCPU_OFF=0, VCPU_ON_PENDING, VCPU_RUNNABLE, VCPU_RUNNING, VCPU_BLOCKED }`；`enum vcpu_event { VCPU_EV_CPU_ON, VCPU_EV_PICK, VCPU_EV_LOAD, VCPU_EV_BLOCK, VCPU_EV_WAKE, VCPU_EV_OFF }`；`int vcpu_state_step(enum vcpu_state *s, enum vcpu_event e)`（合法转移原地推进返回 0，非法返回 -1）。Task 3/4 消费。

转移表（唯一合法边，其余 5×6 矩阵格全部 -1）：

| 事件 | 边 |
|---|---|
| CPU_ON | OFF→ON_PENDING |
| PICK | ON_PENDING→RUNNABLE |
| LOAD | RUNNABLE→RUNNING |
| BLOCK | RUNNING→BLOCKED |
| WAKE | BLOCKED→RUNNABLE |
| OFF | RUNNING/RUNNABLE/BLOCKED/ON_PENDING→OFF（ON_PENDING→OFF 供 SYSTEM_OFF 清扫，spec 状态机图未画、此处定死并注释） |

- [ ] **Step 1: 写失败测试** `tests/host/vcpu_state_test.c`：每条合法边一个 `TEST_ASSERT(vcpu_state_step(&s, EV)==0 && s==目标)`；非法边用双循环遍历全部 (状态,事件) 组合减去合法边，各断言 `==-1` 且 `s` 不变。接线（host_test.h 原型、runner 调用、Makefile 两处）。
- [ ] **Step 2: RED 验证** — `make host-test`：编译/链接失败 `undefined reference to vcpu_state_step`（或隐式声明报错），**观察后落 RED commit**（仅测试+接线，无实现，不进 hv-objs）：
  `git commit -m "test: vcpu_state transition matrix (RED)"`
- [ ] **Step 3: 最小实现** `vcpu_state.h`（枚举+原型，仅 `<types.h>`）+ `vcpu_state.c`（switch 或 6 行表驱动）。
- [ ] **Step 4: GREEN** — `make host-test` 全绿；**GREEN commit**（实现 + `hypervisor/Makefile` hv-objs 追加 `common/vm/vcpu_state.o`；交叉侧尚无调用者，链接入固件为 Task 3/4 备好）：`git commit -m "feat: vcpu_state pure-logic state machine (GREEN)"`
- [ ] **Step 5: 回归** — `make test` 全绿（模块已入 hv-objs，x86 stub 链接过）。

### Task 2: `cpu_arch_idle` / `cpu_arch_kick` 钩子

**Files:**
- Modify: `hypervisor/include/cpu.h`（+两原型，注释含"x86: hlt 循环 / IPI"可移植性一行）、`hypervisor/arch/arm64/cpu/cpu.c`（+实现）、`hypervisor/arch/x86/stub/hooks.c`（+空实现）

**Interfaces:**
- Produces: `void cpu_arch_idle(void)`——arm64 = 单次 `wfi` 后**返回**（调用方循环复查；不是死循环，片 B 调度循环复用）；`void cpu_arch_kick(u32 pcpu)`——arm64 = `gic_kick_pcpu(pcpu)`（cpu.c 已可经 `<gic_v3.h>` 取到）。

- [ ] **Step 1:** cpu.h 原型 + arm64 实现 + x86 stub（同一 commit，stub 缺失即 test-x86-stub 链接红）。
- [ ] **Step 2:** `make test` 全绿（钩子尚无调用者）。Commit: `feat: cpu_arch_idle/cpu_arch_kick hooks (M11 slice A)`。

### Task 3: 亲和表取代 `pcpu_base`（数据模型，行为等价切换）

**Files:**
- Modify: `hypervisor/include/vm_config.h`（`u8 pcpu_base` → `u8 vcpu_pcpu[VCPUS_PER_VM]`，注释：per-vCPU 亲和；**1:N 合法**——多 vCPU 可亲和同一 pCPU，共享执行自片 B 起，片 A 运行最低 idx）、`hypervisor/common/vm/vm_configs.c`（svm/linux 四表项改 `.vcpu_pcpu = {0,1}` / `{2,3}`）、`hypervisor/include/vm.h`（删 `NR_VMS*VCPUS_PER_VM<=NR_CPUS` 断言及其注释块；`struct vcpu` 追加 `enum vcpu_state state;` 并 include `<vcpu_state.h>`）、`hypervisor/common/vm/vm.c`（删结构性 pcpu_base 检查；加启动期校验：每 VM 每 i，`vcpu_pcpu[i] >= NR_CPUS` → BUG printk + `cpu_arch_halt`；vm_init 里全部 vCPU `state=VCPU_OFF`，各 VM `vcpu[0]` 在 `vcpu_arch_reset` 后置 `VCPU_RUNNABLE`——hypervisor 即 boot vCPU 的授权者，guest PSCI 授权 vcpu[1+]）、`hypervisor/arch/arm64/vmexit/vpsci.c` 与 `hypervisor/arch/arm64/vgic/vgic_sgi.c`（`pcpu_base + idx` → `vcpu_pcpu[idx]`，机械替换）、`hypervisor/common/vm/vm.c` vm_run 的 VM1 启动行（`config->pcpu_base` → `config->vcpu_pcpu[0]`）、`tests/check_offsets.c`（+`VCPU_STATE` 偏移断言：先用一个 /tmp 探针 TU 打印 `offsetof(struct vcpu, state)`，把字面值钉进宏；注释注明这是通用区字段、非 asm 可见）。

**Interfaces:**
- Consumes: Task 1 的 `enum vcpu_state`。
- Produces: `cfg->vcpu_pcpu[]`（Task 4/6 消费）；`vcpu->state` 字段。

- [ ] **Step 1:** 上述全部机械修改（一次成形；数据仍是 1:1，行为零变化）。
- [ ] **Step 2:** `make test` + `make host-test` 全绿；`grep -rn pcpu_base hypervisor/` 零命中（vm.h 注释里的历史叙述除外——一并清理）。Commit: `refactor: per-vCPU pCPU affinity table replaces pcpu_base (behavior-equal)`。

### Task 4: 启动流、运行循环与 PSCI 语义重写；`vm.off` 删除（原子行为变更）

**Files:**
- Modify: `hypervisor/include/vm.h`（删 `volatile u32 off` 字段及注释；+`struct vcpu *vcpu_for_pcpu(u32 pcpu)` 原型）、`hypervisor/common/vm/vm.c`（+`vcpu_for_pcpu`：遍历全部 VM 的全部 vCPU，返回**最低 (vmi,idx) 字典序**且 `vcpu_pcpu[]==pcpu` 的 vCPU，无则 NULL；vm_run：改为启动**全部** pCPU 1..NR_CPUS-1（`cpu_arch_power_on` 循环），CPU0 进入下述运行循环）、`hypervisor/arch/arm64/cpu/secondary.c`（secondary_main 重写，见下）、`hypervisor/arch/arm64/vmexit/vpsci.c`（重写四个语义函数）、`hypervisor/arch/arm64/irq/irq_handler.c`（kick 停机分支改判 `current_vcpu()->state == VCPU_OFF`）、`hypervisor/dm/hv_shell.c`（vm_list STATE：`m->off` → "任一 vCPU `state != VCPU_OFF` 则 on，否则 halted"；注释同步改）、`hypervisor/arch/arm64/include/arch/psci.h`（+`#define PSCI_AFF_LEVEL_ON_PENDING 2ULL`）。

**secondary_main(id) 新形状**（保留既有 EL2/GICR/CNTVOFF 初始化不动）：

```c
struct vcpu *v = vcpu_for_pcpu(id);
percpu[id].cur_vcpu = v;            /* 先设——kick 分支读它，防 NULL 窗口 */
percpu[id].cpu_id = id;  dmb;  percpu[id].online = 1;  dsb;
if (!v) { printk("[hv] pCPU%u idle (no vCPU)\n", id); for (;;) cpu_arch_idle(); }
printk("[hv] pCPU%u online\n", id);
for (;;) {
    while (v->state == VCPU_OFF)    /* 等授权；也承接 CPU_OFF 后的再 ON */
        cpu_arch_idle();
    if (v->state == VCPU_ON_PENDING) vcpu_state_step(&v->state, VCPU_EV_PICK);
    if (v->state == VCPU_RUNNABLE) { vgic_init(v); vcpu_arch_load(v); vcpu_state_step(&v->state, VCPU_EV_LOAD); }
    vcpu_arch_run(v);               /* 返回 = 需要本循环决策的出口 */
}
```

（vgic_init/load 在每次 OFF→ON 重入时重做 = PSCI 再 ON 的 fresh-boot 语义。）

**vm_run 的 CPU0 循环同形状**（其 vCPU 恒为 `&vm[0].vcpu[0]`，启动即 RUNNABLE）。

**vpsci.c 语义：**
- `psci_cpu_on_guest`：合法性同今（Aff0、越界）；ALREADY_ON 改判 `v->state != VCPU_OFF`；授权 regs 同今（elr/x0/spsr/hcr）；然后 `vcpu_state_step(&v->state, VCPU_EV_CPU_ON)`；`printk("[hv] PSCI: CPU_ON vCPU%u → ON_PENDING (pCPU%u)\n", ...)`（供 expect 断言，同步确定性）；**`dsb ish` 后 `cpu_arch_kick(pcpu)`**；立即返回 SUCCESS（KVM 先例； bounded-wait 删除）。
- `psci_power_down` 拆三：`CPU_OFF` → 仅本 vCPU `VCPU_EV_OFF` + printk + `hv_restore()`（解回本 pCPU 运行循环 → 授权等待，可再 ON）；`SYSTEM_OFF/RESET(2)` → 本 VM 全部 vCPU（含 ON_PENDING）`VCPU_EV_OFF`、`dsb ish`、kick 本 VM 其余在线 pCPU（复用现 kick 辅助，遍历改 `vcpu_pcpu[]`）、`hv_restore()`；own-vCPU 先行置 OFF 再 kick 远端（序不变量）。
- `psci_cpu_suspend`：`VCPU_EV_BLOCK` → `wfi`（保持现 HW-forward 唤醒路径）→ `VCPU_EV_WAKE` → SUCCESS（可观察行为与今日相同）。
- `psci_affinity_info`：改判 vCPU 状态——ON_PENDING→`PSCI_AFF_LEVEL_ON_PENDING`，RUNNABLE/RUNNING/BLOCKED→ON，OFF→OFF。
- 删除 `psci_kick_vm_other_pcpus` 对 `m->off` 的注释依赖（kick 分支已改状态判据）。

- [ ] **Step 1:** 实现上述全部（一次原子变更；`grep -rn '\->off' hypervisor/` 零命中收口）。
- [ ] **Step 2:** `make host-test` 绿；`make test` 全绿——psci/dual/shell/vspi 场景是本步回归网（现 psci 期望行全部应保持：AFFINITY_INFO(ON/-2)、SUSPEND 两条、done；vm_list 输出 "on"）。
- [ ] **Step 3:** Commit: `feat(m11-a): per-vCPU lifecycle, PSCI semantics, all-pCPU boot; vm.off removed`。

### Task 5: psci 用例扩展（vCPU 粒度断言）

**Files:**
- Modify: `tests/svm/cases/psci.c`（+vCPU1 stub 与断言组）、`tests/svm/expect/psci.txt`（+期望行）

vCPU1 stub：naked 入口按 arm64 secondary boot 协议**自设 SP**（静态 4 KiB 栈数组，`mov sp` 后跳 C 体——今日无先例，svm_lib 无栈助手；C 体：置共享 flag、`svm_puts("SVM: vCPU1 alive")`、`svm_hvc(PSCI_CPU_OFF)` 自停）。vCPU0 断言组（在现用例尾部追加）：

```
CPU_ON(vCPU1, entry=stub, ctx=0xCAFE) == 0        → "SVM: CPU_ON(vCPU1) == SUCCESS"
（bounded 自旋等 flag，超时打 FAIL 行）
二次 CPU_ON(vCPU1) == -4                           → "SVM: CPU_ON(vCPU1) again == ALREADY_ON"
（bounded 等 flag==2：stub OFF 前再置一次）
AFFINITY_INFO(vCPU1) == 1 (OFF)                    → "SVM: AFFINITY_INFO(vCPU1) == OFF after CPU_OFF"
本 vCPU 仍活，打印存活行                            → "SVM: vCPU0 alive after sibling CPU_OFF"
svm_done(0)
```

expect/psci.txt 追加（含 hypervisor 侧确定性行）：`[hv] PSCI: CPU_ON vCPU1 → ON_PENDING (pCPU1)`、上述四行 SVM 行、vCPU1 的 alive 行。

- [ ] **Step 1:** 写用例与期望行 → `make test-qemu-psci`：新旧行全 PASS（红即 Task 4 真 bug，停下修，不许改期望凑绿）。
- [ ] **Step 2:** `make test` 全绿。Commit: `test(m11-a): psci case vCPU-granularity assertions`。

### Task 6: `ctxsw` detector 用例（受控红）+ `svm_ctxsw` profile

**Files:**
- Modify: `hypervisor/common/vm/vm_configs.c`（`#ifdef CONFIG_SVM_SHARED_PCPU` → VM0 `.vcpu_pcpu = {0, 0}`，注释：片 A ctxsw detector 专用 profile，共享**执行**自片 B）、`Makefile`（HV_GUEST 链加 `svm_ctxsw` 分支：`-DCONFIG_GUEST_SVM=1 -DCONFIG_NR_VMS=1 -DCONFIG_SVM_SHARED_PCPU=1`；`SVM_CASES` 追加 `ctxsw`；新目标 `test-qemu-ctxsw`——独立 BUILD_DIR 构建 + `run_svm_test.sh expect/ctxsw.txt`；**不进** `test` 聚合，注释写明受控红与转绿条件（片 B））
- Create: `tests/svm/cases/ctxsw.c`、`tests/svm/expect/ctxsw.txt`

ctxsw.c（vCPU0）：读并存 SCTLR_EL1/TTBR0_EL1/MAIR_EL1/VBAR_EL1 原值 → 写入四枚可辨识常数 → `CPU_ON(vCPU1, entry=stub)`（stub 置 flag 后自旋）→ **CNTV 定时上限等待**（CNTFRQ/CNTV_TVAL 模式，同 psci 用例先例，≈1s，防挂死）flag → 超时后：校验四寄存器仍等于所写常数 → 打印 `CTXSW: vCPU1 never ran — no scheduler yet (slice A controlled red)` 与 `CTXSW: EL1 stamps intact (clobber check begins at slice B)` → `svm_done(0)`。expect/ctxsw.txt 断言：banner、ON_PENDING printk、上述两行、done。

- [ ] **Step 1:** profile + 用例 + 目标 → `make test-qemu-ctxsw`：expect 全 PASS（受控红本身即期望输出）。
- [ ] **Step 2:** `make test` 全绿（聚合未变，7 场景）。Commit: `test(m11-a): ctxsw detector case, controlled red (independent target)`。

### Task 7: 手工双 VM Linux gate + 收尾

- [ ] **Step 1:** 手工 gate（记忆工作流：FIFO 驱动、关 VM1 不关 VM0；`LINUX_IMAGE=… scripts/run-qemu.sh`）：双 VM 到 `~ #`；Ctrl-T 切换两 shell；VM0 `echo 0 > /sys/devices/system/cpu/cpu1/online` → CPU0 shell 仍活；`echo 1 > …/online` → cpu1 回来（Review Focus #5）。输出摘录进当日报告。镜像缺失则如实记"未执行 + 原因"，不装作跑了。
- [ ] **Step 2:** 本 plan 状态行 `Executed（2026-10-31，见合并提交）`；里程碑 plan 骨架片 A 的 DoD 勾选 + commit 哈希。Commit: `docs: m11 slice A close-out`。

---

## 执行方式（协议已定）

主会话 `superpowers:executing-plans` 自实现（Day 2 先例）；测试专家独立复跑 + 架构师关卡②后 `--no-ff` 合回 develop。分支：`feature/m11-a-lifecycle-psci`。

## 已知边界（片 A 记录在案）

- 共享亲和（1:N 数据）合法但**不执行**：pCPU 只跑字典序最低 vCPU，其余 ON_PENDING 挂起（ctxsw 受控红即此形态的可观测化）。
- CPU_ON 返回 SUCCESS 不再保证目标已运行（KVM 先例）；需强同步的消费者用 bounded join（psci 用例先例）。
- `cpu_arch_power_on` 仍用于**启动**物理 pCPU；guest CPU_ON 不再触它。
