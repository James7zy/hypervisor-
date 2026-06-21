# 架构鸟瞰 / Architecture Zoom-Out（2026-06-21）

本文是一次「拉高一层」的整体梳理：从顶层调用链 → GIC 子系统 → 一次键盘输入端到端
trace，三层逐步放大，用项目术语把各模块和调用者串成一张图。面向「不熟悉某块代码、想
先看清它在大局里的位置」的读者。

配套精读：[vcpu-run-world-switch.md](vcpu-run-world-switch.md)、
[handle-exit-dispatch.md](handle-exit-dispatch.md)、[stage2.md](stage2.md)、
[vgic-injection.md](vgic-injection.md)；决策依据见 `docs/adr/`。

---

## 0. 一句话定位

单 VM、单 vCPU（UP）的 Type-1 ARM64 hypervisor。全局只有一个 guest（`g_vm`）、一个
`vcpu`，跑在 EL2，把一个未修改的 Linux 引导到 EL1 的 busybox shell。整套系统围绕**一个
循环**组织：**进 guest → 取一次 exit → 模拟 → 再进 guest**。

---

## 1. 顶层骨架（调用链）

```mermaid
flowchart TD
    HEAD["head.S _start (EL2)"] --> MAIN["hypervisor_main(dtb)<br/>boot/main.c"]
    MAIN --> UART["uart_init()<br/>debug/uart_pl011.c（PL011 直通/earlycon）"]
    MAIN --> GIC["gic_init()<br/>irq/gic_v3.c（物理 GICv3）"]
    MAIN --> VT["vtimer_init()<br/>timer/vtimer.c"]
    MAIN --> VINIT["vm_init()<br/>common/vm/vm.c（构建 g_vm）"]
    MAIN --> VRUN["vm_run()<br/>common/vm/vm.c"]

    VINIT --> S2["stage2_init() Stage-2 页表"]
    VINIT --> VG["vgic_init() 虚拟 CPU 接口"]
    VINIT --> VGM["vgicv3_mmio_init() GICD/GICR 模拟，注册到总线"]
    VINIT --> VC["virtio_console_init() → virtio_mmio → virtqueue"]

    VRUN --> ACT["stage2_activate() / vgic_restore()"]
    VRUN --> LOOP["for(;;) vcpu_run(vcpu) ← 世界切换"]
```

关键术语 **MMIO trap-and-emulate 总线**（`vmexit/mmio.c`，ADR-0006）：一张固定 8 槽的
表。每个被模拟的设备在 init 时调用 `mmio_bus_register(base, len, handler, ctx)`。Stage-2
data abort 触发时，总线用 `FAR_EL2`+`HPFAR_EL2` 重建 IPA、解析 ISS（访问宽度/寄存器/方向）、
查到 region、调它的 handler，再把 `ELR_EL2` 推过出错指令。

---

## 2. 世界切换 + 退出分发（心脏）

```mermaid
sequenceDiagram
    autonumber
    participant VR as vm_run 循环
    participant ASM as vmexit_asm.S
    participant G as Guest EL1
    participant H as handle_exit vmexit.c
    participant BUS as MMIO 总线 mmio.c

    VR->>ASM: vcpu_run(vcpu)
    ASM->>G: 保存 HV ctx, 恢复 guest, eret
    G-->>ASM: 异常 → VBAR_EL2
    ASM->>H: handle_exit 按 ESR.EC 分流
    alt EC 0x16 HVC
        H->>H: handle_hvc → PSCI / 调试 hypercall
    else EC 0x24 Data Abort
        H->>BUS: mmio_handle_data_abort
        BUS->>BUS: 按 IPA 查 region → GICD/GICR 或 virtio handler
    end
    H-->>G: eret 回 guest，多数同步 exit 不返回 C
```

运行期实际会见到的三类 exit：

1. **Data abort（EC 0x24）** —— guest 访问被模拟的 MMIO（GIC 或 virtio）→ 总线 → 设备
   handler。**最常见**。
2. **HVC（EC 0x16）** —— guest 调 PSCI（真实 Linux）或 M2 调试 hypercall。
3. **Timer IRQ** —— 唯一会返回 C 并在 `vm_run` 里循环的 exit；PPI 经 vgic 重新注入。

---

## 3. 放大：GIC 子系统

GIC 工作拆成**三个互不混淆**的关注点：

| 关注点 | 文件 | 拥有什么 |
| --- | --- | --- |
| **物理 GICv3**（host 侧） | `irq/gic_v3.c` | 真实 distributor/redistributor + EL2 CPU 接口。hypervisor 独占。 |
| **vGIC CPU 接口**（硬件虚拟化） | `irq/vgic.c` + `vgic.h` | `ICH_*` 列表寄存器机制，向 guest **呈现**虚拟 IRQ。 |
| **vGIC distributor/redistributor**（模拟） | `irq/vgic_v3_mmio.c` | 影子 GICD/GICR 寄存器——纯 trap-and-emulate，从不碰真实硬件。 |

核心框架：**guest 看到的是全虚拟 GIC**。它的 `ICC_*` CPU 接口访问被硬件经
`ICC_SRE_EL2` 重定向到 `ICV_*`；它的 GICD/GICR **内存**访问以 Stage-2 data abort 陷入，命中
影子模型。只有**一个真实中断**穿透：vtimer PPI（INTID 27），硬件转发。详见
[[adr-0012]]（物理 GICv3 独占 + 逐寄存器说明）与 [[adr-0010]]（vGIC 影子范围）。

三种注入（关键区别是 HW 位与用哪条 LR）：

| 函数 | LR | HW 位 | 调用者 | 为什么 |
| --- | --- | --- | --- | --- |
| `vgic_inject_hw(vintid, pintid)` | **LR0** | HW=1 | `el2_irq_handler`（vtimer 27） | guest deactivate vIRQ 经 LR 链路释放物理定时器线（ADR-0001） |
| `vgic_inject_spi(intid)` | **LR1** | HW=1 | `el2_irq_handler`（PL011 33）、virtio-console | LR1 避免被 LR0 每 tick 的 vtimer 覆盖 |
| `vgic_inject_sw(vintid, prio)` | LR0 | HW=0 | `handle_hvc`（M2 调试钩子） | 纯软件注入，无物理线 |

> 文档/代码漂移记录：`vgic.h` 注释把 `vgic_inject_spi` 描述为 `vgic_inject_sw` 的薄封装，
> 但实现是独立的 HW=1/LR1 路径。该头注释已过时。

---

## 4. 端到端 trace：一次 `ttyAMA0` 键盘输入

前提：guest 控制台是 `console=ttyAMA0`——**直通**的物理 PL011（`0x09000000`）。hypervisor
**不**模拟该 UART；它在 Stage-2 把 UART 以 Device-nGnRE 恒等映射，guest 的读写直接命中真实
设备。hypervisor 只截获**中断**以路由进 guest 的 vGIC。三个地址锚点：PL011 基址
`0x09000000`；PL011 RX 是 **SPI 33**（DTS `<0 1 4>` → 32+1）；SPI 33 已在 `gic_init` 时
登记进物理 GICD（Group 1、prio 0xA0、路由 Aff=0、enable）。

参与者前缀标注了运行的特权级：`〔HW〕`=硬件、`〔EL2〕`=hypervisor、`〔EL1〕`=客户机。
**注意 `el1_irq_handler_asm` / `el2_irq_handler` 都运行在 EL2**——函数名里的 `el1` 指
「来自 EL1 的异常」（Lower EL），不是它的运行级别。

> 这里本想用 `box` 泳道分组直接画出 EL2/EL1 分层，但本地 `mermaid@11` 对 `box` 报
> `Option is not defined`，故改用别名前缀。详见 [mermaid-gotchas.md](mermaid-gotchas.md) 坑 3。

```mermaid
sequenceDiagram
    autonumber
    participant K as 〔HW〕键盘/QEMU PL011
    participant GICp as 〔HW〕物理 GICv3
    participant ASM as 〔EL2〕el1_irq_handler_asm
    participant HND as 〔EL2〕el2_irq_handler
    participant VG as 〔EL2〕vgic_inject_spi
    participant G as 〔EL1〕Guest pl011 ISR

    K->>GICp: 按键 → 字节入物理 RX FIFO, 拉高 SPI 33
    Note over GICp: HCR_EL2.IMO=1 → 物理 IRQ 路由到 EL2，不进 EL1
    GICp->>ASM: 陷入 EL2，跳 VBAR_EL2+0x480 Lower-EL IRQ 向量
    ASM->>ASM: 保存 guest 帧 → g_vm.vcpu.regs
    ASM->>HND: bl el2_irq_handler
    HND->>GICp: gic_ack_irq → ICC_IAR1_EL1 = 33
    HND->>VG: vgic_inject_spi(33)
    VG->>VG: 写 ICH_LR1_EL2，PENDING HW G1, vINTID=pINTID=33
    HND->>GICp: gic_priority_drop(33) → ICC_EOIR1_EL1，只 drop 留 Active
    HND-->>ASM: 返回, 恢复帧, eret，eret 前不解 EL2 IRQ 屏蔽
    ASM->>G: eret 回 EL1，虚拟 SPI 33 经 ICV_* 呈现
    Note over ASM,G: 此后 guest 收到的是【虚拟】SPI 33，物理中断全程未触达 EL1
    G->>K: 读 DR@0x09000000，Stage-2 恒等不陷入 → 取走字节, 线 de-assert
    G->>GICp: ICV_EOIR1/DIR deactivate vIRQ
    Note over G,GICp: HW=1 LR 链路 → 硬件自动 deactivate 物理 SPI 33，线释放无风暴
```

**值得记住的非对称性：** **输出**（guest 打印字符）是对直通 `DR` 的普通 MMIO store——
从不陷入、与 hypervisor 无关。**输入**（本 trace）是 hypervisor 唯一介入的方向，且只为
**中断**——数据本身仍在设备↔guest 间直连流动。hypervisor 在这里是纯中断路由器，这正是
ADR-0005 对控制台 UART 选直通而非模拟的原因。

`gic_priority_drop` 但**不** deactivate（EOImode=1 拆分）是为防止 level-sensitive RX 线
立即 re-pend 风暴 EL2（与 vtimer 同理，ADR-0001）；HW=1 的 LR 链路保证 guest 处理完后物理
线被干净释放。

---

## 5. 模块速查表

| 模块 | 角色 | 调用者 / 依据 ADR |
| --- | --- | --- |
| `boot/main.c`, `boot/head.S` | EL2 bring-up、引导序列 | reset 向量 |
| `common/vm/vm.c`（`g_vm`） | 单 VM + run loop | `main.c`；ADR-0002 |
| `vmexit/vmexit.c`（`handle_exit`） | 按 ESR.EC 分流 | `vmexit_asm.S`；ADR-0007 |
| `vmexit/mmio.c` | MMIO 总线 | 各模拟设备；ADR-0006 |
| `arch/.../mmu/stage2.c` | Stage-2 GPA→PA 隔离 | `vm_init`；ADR-0004 |
| `arch/.../irq/gic_v3.c` | 物理 GICv3（host） | `main.c`；ADR-0012 |
| `arch/.../irq/vgic.c` | vGIC CPU 接口（ICH_*、LR） | `vm_init`、`vmexit.c`、timer |
| `arch/.../irq/vgic_v3_mmio.c` | GICD/GICR 模拟 | 注册到 MMIO 总线；ADR-0010 |
| `arch/.../timer/vtimer.c` | 虚拟定时器、PPI 注入 | `main.c`、IRQ handler；ADR-0001 |
| `common/psci/psci.c` | PSCI 电源管理 | `handle_hvc` |
| `dm/virtio_mmio.c`+`virtqueue.c`+`virtio_console.c` | 设备模型：virtio-mmio v2、virtqueue、console | `vm_init`、MMIO 总线；ADR-0011 |
| `dm/gpa.c` | GPA（guest-physical）访问器 | virtio 缓冲区 |
| `debug/uart_pl011.c` | PL011 驱动（直通 earlycon） | `main.c`；ADR-0005 |

---

## 延伸阅读

- 世界切换汇编：[vcpu-run-world-switch.md](vcpu-run-world-switch.md)
- 退出分发：[handle-exit-dispatch.md](handle-exit-dispatch.md)
- Stage-2 / vGIC：[stage2.md](stage2.md)、[vgic-injection.md](vgic-injection.md)
- 决策：ADR-0001（HW 转发）、ADR-0005（直通 vs 模拟）、ADR-0006（MMIO 总线）、
  ADR-0010（vGIC 范围）、ADR-0012（物理 GICv3 独占 + 逐寄存器）
- 实测启动：`docs/debug/m3-boot-debug-walkthrough.md`
