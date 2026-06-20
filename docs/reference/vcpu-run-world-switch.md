# vcpu_run 与 EL2↔EL1 世界切换（world switch）

本文解释 `hypervisor/arch/arm64/vmexit/vmexit_asm.S` 中三段汇编的职责，重点是
`vcpu_run` 到底在做什么，以及它在完整的 world-switch 闭环里的位置。

常见疑问：

1. `vcpu_run` 是干什么的？
2. 为什么 `eret` 之前**不能**解开 EL2 的 IRQ 屏蔽？
3. 进 guest（`vcpu_run`）、出 guest（`el1_sync_handler`）、彻底退出（`hv_restore`）三者怎么闭合？

---

## 1. 一句话概括

`vcpu_run(struct vcpu *vcpu)` 是整个 hypervisor 的核心动作：**从 EL2（hypervisor）
切换到 EL1（guest），让 guest 跑起来**。它是 world switch 的「进入 guest」这一半——
把当前 hypervisor 的执行现场存起来，把 guest 的寄存器 / 系统状态全部装载到 CPU 上，
然后 `eret` 跳进 guest 的 EL1 开始执行。

入参：`x0 = vcpu*`，指向 guest 的寄存器快照 `struct vcpu`（其首字段是 GPR 数组，
所以 `[x0 + 0x00]` 就是 guest x0）。

---

## 2. vcpu_run 的四步

### 第 1 步 —— 保存 hypervisor 自己的现场（26–37 行）

```asm
adrp/add x9, g_hv_ctx
stp x19, x20, [x9, #0x00]   ... str x30, [x9, #HV_LR]
mov x10, sp;  str x10, [x9, #HV_SP]
```

把 hypervisor 的 **callee-saved 寄存器（x19–x29）、返回地址 x30（LR）、栈指针 SP**
存进全局 `g_hv_ctx`。

为什么只存 callee-saved？因为 `vcpu_run` 是个普通 C 函数调用，按 AAPCS 调用约定，
caller-saved 的寄存器调用方早已处理；hypervisor 只需保证被恢复后能「接着原来的 C
代码往下跑」。这份现场后面由 `hv_restore()` 取回。

### 第 2 步 —— 配置 Stage-2 / trap 行为（39–42 行）

```asm
ldr x1, [x0, #VCPU_HCR_EL2];  msr hcr_el2, x1;  isb
```

把这个 vCPU 的 `HCR_EL2` 写进硬件。`HCR_EL2` 决定 guest 运行时的虚拟化行为——
VM 位（开 Stage-2 翻译）、IMO/FMO（IRQ/FIQ 路由到 EL2）、各种 trap 使能等。
`isb` 保证这个配置在 `eret` 前生效。

### 第 3 步 —— 装载 guest 的系统寄存器（44–50 行）

```asm
msr elr_el2,  guest PC      ← eret 之后从这里开始执行
msr spsr_el2, guest PSTATE  ← eret 之后恢复成这个处理器状态（含 EL1、中断屏蔽位等）
msr sp_el1,   guest SP
```

`ELR_EL2` 和 `SPSR_EL2` 是 `eret` 的两个关键：`eret` 会跳到 `ELR_EL2` 指的地址，
并把 `SPSR_EL2` 还原为 PSTATE（其中的 EL 字段决定落到 EL1）。

### 第 4 步 —— 装载 guest 的通用寄存器 x0–x30（52–69 行）

```asm
ldr x1, [x0, #0x08]
ldp x2, x3, [x0, #0x10]
...
ldr x0, [x0, #0x00]   /* x0 最后装 */
```

把 guest 上次的 31 个 GPR 全部恢复。**x0 故意最后装**——因为 x0 此刻还是「指向
vcpu 的指针」，要先用它把 x1–x30 都读出来，最后一步才用 `[x0 + 0x00]` 把 x0 自己
覆盖掉。

### 最后 —— `eret`（80 行）

跳进 guest。CPU 立即从 `ELR_EL2`（guest PC）在 EL1 继续执行，PSTATE 变成
`SPSR_EL2`。从此 hypervisor 不再占用 CPU，guest 在跑。

---

## 3. 为什么 eret 之前不能解开 EL2 IRQ 屏蔽

71–79 行的注释很关键：**绝不能在 `eret` 之前 `daifclr` 解开 EL2 的 IRQ 屏蔽**。

- `HCR_EL2.IMO=1` 时，guest（EL1）运行期间来的物理 IRQ **本来就会被送到 EL2**
  （走 Lower-EL IRQ 向量 `+0x480`），它只受 EL2 的 PSTATE.I 门控，而在 EL1 运行时
  那个门控不适用。所以定时器 PPI 照样会被取到，不需要也不应该提前解屏蔽。
- 若提前解开，就出现一条指令的窗口——IRQ 在 **EL2 当前级**被取到，而 `vectors.S`
  把 current-EL 的 IRQ 入口指向 `panic_vector`，直接 panic。

这正是 M3 首次真机启动时踩到的三个 bug 之一，详见
`docs/debug/m3-boot-debug-walkthrough.md`（Bug 3）。

---

## 4. 在完整 world-switch 循环里的位置

`vcpu_run` 只是「进 guest」这一半。它的调用方是 `common/vm/vm.c` 里的 `vm_run()`，
**一个永不退出的稳态引擎**：

```c
void vm_run(void)                 /* vm.c:58 */
{
    stage2_activate(&g_vm.vcpu);
    vgic_restore(&g_vm.vcpu);
    for (;;) {
        vcpu_run(&g_vm.vcpu);     /* 进 guest；某些 exit 后会 return 回这里，循环再进 */
    }
}
```

配套的三段汇编：

- **`vcpu_run`**（25 行）：EL2 → EL1，进 guest。
- **`el1_sync_handler`**（86 行）：guest 触发**同步异常**（HVC、Stage-2 data
  abort、MMIO trap 等）时，硬件经 `VBAR_EL2` 跳到这里；它把 guest 寄存器存回
  `g_vm.vcpu.regs`，再调用 C 函数 `handle_exit()` 做分发。
- **`hv_restore`**（160 行）：从 `g_hv_ctx` 取回第 1 步存的 hypervisor 现场，`ret`
  回到**最近一次** `vcpu_run` 调用点（即 `vm_run` 的 for 循环体内）。

### 4.1 三条「回 EL2 / 续跑」路径

退出 guest 后**到底走哪条路回去**，取决于这次是什么 exit。一共三条：

| 路径 | 触发 | 落点 | 是否回 C 的 `for` 循环 |
|------|------|------|----------------------|
| **A. eret 回 guest** | MMIO data abort、PSCI、未知 HVC 等同步异常 | `el1_sync_handler` 末尾 `eret`（128–154 行）→ 直接续跑 guest | **否**，全程在汇编里，不回 `vm_run` |
| **B. return 回 for** | 定时器 IRQ exit（物理 IRQ → EL2） | `vcpu_run` 正常返回 → `for` 体末尾 → 循环再 `vcpu_run` 进 guest | **是**，靠 for 重新进 guest，让连续 PPI 推进 guest 时间 |
| **C. hv_restore** | `HC_GUEST_DONE`（M2 调试 HVC） | `ret` 回到**最近一次** `vcpu_run` 调用点 = for 体内 → 循环再进 guest | **是**（落点同 B） |

```mermaid
sequenceDiagram
    participant C as vm_run() for(;;) (C, EL2)
    participant R as vcpu_run (asm)
    participant G as Guest (EL1)
    participant H as el1_sync_handler (asm, EL2)
    participant E as handle_exit() (C, EL2)
    participant I as el2_irq_handler (IRQ, EL2)
    participant V as hv_restore (asm)

    C->>R: vcpu_run(vcpu)
    R->>R: 存 HV 现场 → g_hv_ctx
    R->>G: eret (进 EL1)

    alt 路径 A：同步异常（MMIO / PSCI / 未知 HVC）
        G->>H: VBAR_EL2 +0x400
        H->>E: handle_exit(regs, esr)
        E-->>H: 返回（已推进 ELR）
        H->>G: eret 回 guest 续跑（不回 C）
    else 路径 B：定时器 IRQ
        G->>I: VBAR_EL2 +0x480（物理 IRQ）
        I->>I: 注入 vtimer PPI + priority-drop
        I-->>R: 返回
        R-->>C: vcpu_run return
        Note over C: for 循环再次 vcpu_run（回到顶部）
    else 路径 C：HC_GUEST_DONE（M2 遗留钩子）
        G->>H: HVC
        H->>E: handle_exit → handle_hvc
        E->>V: hv_restore()（不返回）
        V-->>C: ret 到最近 vcpu_run 调用点 = for 体内
        Note over C: for 循环再次 vcpu_run（落点同 B）
    end
```

### 4.2 这个 for 会退出吗？CPU 会被抢走吗？

**当前（UP，单 VM，无调度器，ADR-0002）：`for` 不会退出，CPU 不会被抢走。**

- 路径 A 全程在汇编里 `eret` 回 guest，根本不碰 `for`。
- 路径 B / C 都落回 `for` 体内，循环立刻再进 guest。
- 没有抢占、没有时间片、没有别的 vCPU 来抢 CPU——CPU 永远在「guest 跑(EL1)」⇄
  「EL2 处理一次 exit 再回 guest」之间围着这唯一的 guest 转。
- `main.c:36` 的 `for(;;) cpu_wfi()` 只是 `vm_run` 万一返回时的兜底，**当前永不触达**。

⚠️ **两处与代码现状的偏差，留作 M3.5 清理**：

1. `vm.c` 注释说 `hv_restore`「longjmps past this loop and out of vm_run」——**与实现
   不符**。`hv_restore` 恢复的 `g_hv_ctx` 每次 `vcpu_run` 入口都被覆盖，存的就是 for
   体内那次调用的 SP/LR，所以 `ret` 落点在 for 体内，并不真的跳出 `vm_run`。
2. 路径 C 的 `HC_GUEST_DONE` 是 M2 的调试 HVC 钩子，**真实 Linux guest 不会发它**
   （见 [handle-exit-dispatch.md](handle-exit-dispatch.md) §4），所以这条路径跑 Linux
   时压根不触发。

「退出 `vm_run` 去切到别的 vCPU」要等 **M3.5（SMP + 调度器）** 才成为真实路径。

---

## 5. 一个值得注意的设计细节（UP → SMP）

`el1_sync_handler` 保存现场用的是 `g_vm`（全局单 VM 结构），而 `vcpu_run` 入口接收的
是 `x0 = vcpu*` 参数。当前 M3 是单核单 guest（UP），`g_vm.vcpu` 就是那唯一的 vcpu，
两者指向同一份数据，能闭合。

等到 **M3.5（SMP / 多 vCPU）** 时，这个对 `g_vm` 的硬编码就需要改成 per-pCPU 的
current-vcpu 指针，否则多 vCPU 会互相踩。

---

> 配套阅读：Stage-2 翻译见 [stage2.md](stage2.md)；中断硬件转发的来龙去脉见
> ADR-0001（`docs/adr/0001-*`）；首次真机启动的三个 bug 见
> `docs/debug/m3-boot-debug-walkthrough.md`。
