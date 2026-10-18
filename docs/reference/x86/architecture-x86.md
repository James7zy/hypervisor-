# 架构对照：x86 基座版 / Architecture on an x86 Baseline

> **本文性质**：这是一份**对照文档**，不是实现计划。本仓库的代码基座是 ARM64 EL2
> （`hypervisor/arch/arm64/`），本文把目标 ACRN 架构的 ARM 底座换成
> **x86-64 VT-x** 基座重画，并给出 ARM↔x86 的术语与机制对照，用途是：
>
> 1. 读 `../acrn-hypervisor`（x86 Type-1，本仓库的结构灵感来源）时能把它的概念映射回来；
> 2. 判断本仓库某个设计是「体系结构无关的通用虚拟化结构」还是「ARM 特有的实现细节」——
>    凡是两栏能一一对上的，通常属于前者，抽象层就该切在那里。
>
> 本文不改变路线图。RK3588 是 M15，尚无 x86 移植里程碑。`arch/x86/` 目前只是链接检查用的
> stub（`make test-x86-stub`）：它证明通用代码能用 x86_64 编译和链接，不能运行。

配套阅读：[当前 ARM 架构](../arm/current-architecture.md)、
[目标架构](../arm/target-architecture.md)、
[ADR-0015](../../adr/0015-arch-boundary-core-vs-arch.md)。

---

## 0. 一句话对照

| | ARM64（本仓库） | x86-64（本文） |
| --- | --- | --- |
| Hypervisor 特权态 | **EL2**（独立异常级） | **VMX root operation, ring 0**（正交于 ring，不是新 ring） |
| Guest 内核态 | EL1 | VMX non-root, ring 0 |
| 进/出 guest | `eret` ↓ / 异常 ↑ | `VMLAUNCH`/`VMRESUME` ↓ / VM-exit ↑ |
| 二级地址翻译 | Stage-2（VTTBR_EL2） | **EPT**（Extended Page Tables, EPTP） |
| Guest 上下文存放 | 手写结构体 + 汇编 save/restore | **VMCS**（部分字段由硬件自动切换） |
| 中断虚拟化 | vGICv3 + 列表寄存器 ICH_LR<n> | vLAPIC + **APICv**（Posted Interrupt / virtual-APIC page） |
| 电源/固件接口 | PSCI over HVC | ACPI + **hypercall**（`VMCALL`） |
| DMA 隔离 | SMMU | **VT-d IOMMU** |

一句话：**结构完全同构，硬件把「上下文」这件事的分工划在了不同位置**——ARM 让软件显式
保存几乎所有状态，x86 把一大半塞进 VMCS 由硬件切换。这个差别是后文所有分歧的根源。

---

## 1. 顶层系统图（x86 基座）

对应 [目标架构](../arm/target-architecture.md) 的顶层图。ACRN 模型的形状不变——Service VM 里跑用户态
Device Model，User VM 用 virtio 前端——变的只是底座那一层。

```
+--------------------------------------------------------------------------------+
|                         Applications / Workloads                               |
|                                                                                |
|   +----------------------+     +----------------------+     +----------------+ |
|   | Linux Apps           |     | Android / Linux Apps |     | RT Apps        | |
|   | Mgmt / Cloud / UI    |     | IVI / HMI / General  |     | Control Tasks  | |
|   +----------+-----------+     +----------+-----------+     +-------+--------+ |
|              |                            |                         |          |
+--------------|----------------------------|-------------------------|----------+
               |                            |                         |
               v                            v                         v
+-------------------------------+   +------------------------+   +--------------+
|          Service VM            |   |        User VM          |   |   RTOS VM    |
|       Linux / SOS VM           |   |  Linux / Android Guest  |   | RTOS Guest   |
|                                |   |                         |   |              |
| +----------------------------+ |   | +--------------------+  |   | +----------+ |
| |    Device Model, DM        | |   | | Guest OS           |  |   | | RTOS     | |
| |                            | |   | |                    |  |   | | Kernel   | |
| | - Create / start VM        | |   | | - VirtIO frontend  |  |   | |          | |
| | - Emulate virtual devices  | |<---->| - Virtual devices |  |   | | RT tasks | |
| | - Handle VM exits / PIO    | |   | | - Guest drivers    |  |   | +----------+ |
| | - Provide VirtIO backend   | |   | | - Applications     |  |   |              |
| +-------------+--------------+ |   | +--------------------+  |   |              |
|               |                |   +------------+-----------+   +------+-------+
| +-------------v--------------+ |                |                      |
| | HSM driver (acrn_hsm)      | |                |                      |
| | ioctl + io_req shared ring | |                |                      |
| +-------------+--------------+ |                |                      |
|               |                                |                      |
+---------------|--------------------------------|----------------------|---------+
                |                                |                      |
                | hypercall = VMCALL             | VM Exit              |
                | VM lifecycle control           | EPT viol / PIO / MSR |
                v                                v                      v
+--------------------------------------------------------------------------------+
|                       Hypervisor  (VMX root, ring 0)                           |
|                                                                                |
| +--------------------+  +--------------------+  +----------------------------+ |
| | VM Management      |  | vCPU Scheduler     |  | Memory Manager             | |
| | Create / destroy   |  | vCPU dispatch      |  | EPT: GPA -> HPA isolation   | |
| +--------------------+  +--------------------+  +----------------------------+ |
|                                                                                |
| +--------------------+  +--------------------+  +----------------------------+ |
| | VM Exit Handler    |  | Interrupt Manager  |  | I/O Virtualization         | |
| | Decode exit reason |  | vLAPIC / APICv /   |  | MMIO / PIO bitmap /        | |
| | Forward to DM      |  | Posted Interrupt   |  | MSR bitmap / passthrough   | |
| +--------------------+  +--------------------+  +----------------------------+ |
|                                                                                |
|        CPU / Memory / Interrupt / Device Isolation & Virtualization             |
+--------------------------------------------------------------------------------+
                |
                v
+--------------------------------------------------------------------------------+
|                              x86-64 Platform / SoC                             |
|                                                                                |
| +----------------------+  +----------------------+  +------------------------+ |
| | x86 Cores            |  | Memory               |  | Platform Devices       | |
| | Intel Core / Xeon    |  | DDR RAM              |  | UART 16550 / I2C / SPI | |
| | ring 0-3 x           |  |                      |  | GPU / USB / PCIe       | |
| | (root / non-root)    |  |                      |  | NIC / NVMe / Display   | |
| +----------------------+  +----------------------+  +------------------------+ |
|          |                         |                         |                 |
|          +-------------------------+-------------------------+                 |
|                                                                                |
|        Intel VT-x / VMX root & non-root operation                              |
|        EPT (Extended Page Tables) — second-level address translation           |
|        Local APIC / x2APIC / APICv / IOAPIC                                    |
|        VT-d (IOMMU) for DMA isolation                                          |
|        ACPI / power management                                                 |
+--------------------------------------------------------------------------------+
```

**与 ARM 版的差异只有三处**（其余逐行同构）：

1. **底座那一栏**：EL2 → VMX root；Stage-2 → EPT；GICv3/GICv4 → LAPIC/APICv/IOAPIC；
   SMMU → VT-d；PSCI → ACPI + VMCALL。
2. **"Handle VM exits / MMIO"** 变成 **"Handle VM exits / PIO"**：x86 有独立的 I/O 端口
   空间（`IN`/`OUT`），这是 ARM 完全没有的第二条 I/O 通路，DM 必须同时处理 MMIO 和 PIO。
3. **多出 MSR bitmap**：x86 的大量"系统寄存器"是 MSR，通过 `RDMSR`/`WRMSR` 访问，
   要按位决定哪些陷入。ARM 的对应物是 `HCR_EL2`/`CPTR_EL2` 等一组 trap 使能位——
   粒度粗得多（按功能组，而非按寄存器）。

---

## 2. 特权层次图（x86 基座）

对应 [目标架构](../arm/target-architecture.md) 的特权层次图。**这是两个体系结构差异最大的一张图**：ARM 的 EL0–EL3
是一条线性的特权阶梯，hypervisor 住在比 guest 内核**更高的一级**；x86 的 VMX root/non-root
是**正交于 ring 0–3 的一个新维度**，hypervisor 和 guest 内核都在 ring 0，靠"哪一侧"区分。

```
+--------------------------------------------------------------------------------+
|                    x86-64 Privilege: two orthogonal axes                       |
|                                                                                |
|                 ring 3  (user)          |          ring 0  (kernel)            |
|   -----------------------------------------------------------------------------|
|                                          |                                     |
|   VMX non-root   Guest applications      |   Guest OS kernel                    |
|   (guest side)   Linux / Android / RTOS  |   Linux / Android / RTOS             |
|                  user space              |   - drivers, virtio frontends        |
|                                          |   - traps out on VM exit             |
|   -----------------------------------------------------------------------------|
|                                          |                                     |
|   VMX root       (Service VM userspace   |   Hypervisor                         |
|   (host side)     DM runs here, but as   |   - vCPU scheduling                  |
|                   ring 3 of the Service  |   - EPT (GPA -> HPA)                 |
|                   VM, i.e. still         |   - VM exit decode & emulate         |
|                   non-root — see note)   |   - interrupt virtualization         |
|                                          |   - device passthrough control       |
|                                          |                                     |
+--------------------------------------------------------------------------------+
|                    Optional firmware / security layer                          |
|                    SMM (System Management Mode) — orthogonal again;            |
|                    UEFI / ACPI provide the PSCI-equivalent power interface     |
+--------------------------------------------------------------------------------+
|                              x86-64 Hardware                                   |
|                                                                                |
|  x86 Cores | DDR Memory | LAPIC/x2APIC/IOAPIC | VT-d | PCIe | MMIO+PIO | DMA    |
+--------------------------------------------------------------------------------+
```

> **易错点**：ACRN 的 Device Model 是 Service VM 里的**用户态**程序，所以它在
> `VMX non-root, ring 3`——它并不比 User VM 更特权。它的权力来自 hypervisor 认可它的
> **hypercall 权限**（M12 里的 "Service VM privilege concept"），不来自 CPU 特权级。
> ARM 侧同理：Service VM 的 DM 在 EL0，靠 hypervisor 的 VM 角色标记而非 EL 取得特权。
> **这一点是体系结构无关的**，两边都成立。

对照 ARM 侧的线性阶梯：

```mermaid
flowchart LR
    subgraph ARM["ARM64：线性特权阶梯"]
        direction TB
        A0["EL0 用户态"] --> A1["EL1 Guest 内核"]
        A1 --> A2["EL2 Hypervisor 本仓库"]
        A2 --> A3["EL3 Secure Monitor / TF-A"]
    end
    subgraph X86["x86-64：ring x root/non-root 正交"]
        direction TB
        X1["non-root ring 3 Guest 用户态"] --> X2["non-root ring 0 Guest 内核"]
        X2 -->|"VM exit"| X3["root ring 0 Hypervisor"]
        X3 -->|"VMRESUME"| X2
    end
```

**为什么这个差别对代码有影响**：ARM 侧 hypervisor 有一整套**属于自己的**系统寄存器
（`VBAR_EL2`、`SP_EL2`、`TTBR0_EL2`……），guest 的 EL1 寄存器在硬件上是分离的一组，
但这不提供同核两个 vCPU 之间的隔离。当前 `vcpu_arch_run` 只搬运 GPR 和少量返回状态，
没有 FP/SIMD 保存；M11 还必须切换 EL1 系统寄存器、timer、vGIC 等状态。x86 侧
hypervisor 和 guest **共用同一组**架构寄存器（`RIP`、`RSP`、`CR3`、段寄存器……），
只能靠 VMCS 的 host-state / guest-state area 让硬件在每次 VM entry/exit 时整体换掉。

---

## 3. 世界切换：ARM `vcpu_arch_run` vs x86 `VMRESUME`

对应 [vcpu-run-world-switch.md](../arm/vcpu-run-world-switch.md)。本仓库 `vcpu_arch_run` 的三段式
（进 guest 汇编 / 出 guest 异常向量 / 彻底退出）在 x86 上塌缩成**一条指令边界**。

```mermaid
sequenceDiagram
    autonumber
    participant VR as 〔root〕vcpu_run C 代码
    participant HW as 〔HW〕VMCS + VMX 微码
    participant G as 〔non-root〕Guest 内核
    participant EH as 〔root〕vmexit_handler

    VR->>HW: VMWRITE 更新 guest-state 脏字段
    VR->>VR: 手动恢复 GPR 通用寄存器 VMCS 不管这些
    VR->>HW: VMRESUME
    HW->>G: 硬件加载 guest-state 区 RIP CR3 段 等
    Note over G: guest 正常执行 直到触发退出条件
    G->>HW: EPT violation / IN OUT / RDMSR / VMCALL / 外部中断
    HW->>EH: 硬件保存 guest-state 加载 host-state 回到 root
    EH->>EH: 手动保存 GPR
    EH->>EH: VMREAD exit_reason exit_qualification guest_rip
    EH->>EH: 按 exit_reason 分发 模拟
    EH->>VR: 推进 guest RIP 越过出错指令 返回循环
```

**逐项对照本仓库的实现**：

| 本仓库（ARM） | x86 等价物 | 差异要点 |
| --- | --- | --- |
| `eret` 进 guest | `VMLAUNCH`（首次）/ `VMRESUME`（后续） | x86 区分首次与后续；ARM 不区分 |
| `VBAR_EL2` 向量表接住 exit | 无向量表，VM exit 统一回到 VMCS 的 host `RIP` | x86 只有**一个**退出入口，分发全靠软件读 `exit_reason` |
| `ESR_EL2` 的 EC 字段分流 | `VMCS.exit_reason`（基本原因）+ `exit_qualification`（细节） | 同构：都是"硬件给一个编码，软件查表分发" |
| `FAR_EL2` + `HPFAR_EL2` 重建 IPA | `guest_physical_address` 字段（VMCS 直接给 GPA） | **x86 更省事**：不用拼接，硬件直接给出错的 GPA |
| ISS 解码访问宽度/寄存器/方向 | **无**——x86 必须**软件反汇编** guest 指令 | **x86 更麻烦**：变长指令集，ARM 的定长指令让硬件能把这些填进 ISS |
| `ELR_EL2 += 4` 推过出错指令 | `guest_rip += VMCS.exit_instruction_length` | 同构，但 x86 长度由硬件给（指令变长） |
| 当前仅保存 GPR/返回状态，完整 vCPU 状态待 M11 | VMCS 自动切换一部分，GPR/扩展状态仍需软件处理 | 静态绑核不保存 FP/SIMD 的假设不能直接用于同核调度；x86 也需处理 `XSAVE` 等状态 |

> **当前边界**：相似的流程不等于共同接口。`handle_exit` 解释 ARM EC/sysreg/PSCI，
> 仍在 `arch/arm64/vmexit/vmexit.c`；ISS 解码在同目录 `data_abort.c`。
> 只有 `mmio_access` 和 `common/io/mmio.c` 总线是架构无关的。x86 需要自己的退出
> 解码器，再把设备访问交给通用总线，不应让通用代码认识 ESR 或 VMCS 字段。

---

## 4. 二级地址翻译：Stage-2 vs EPT

对应 [stage2.md](../arm/stage2.md)、[stage2-l1-to-l2.md](../arm/stage2-l1-to-l2.md)、ADR-0004。

```mermaid
flowchart TD
    subgraph S2["ARM64 Stage-2 本仓库"]
        GVA["Guest VA"] -->|"Stage-1 guest 自己的页表 TTBR0_EL1"| IPA["IPA 中间物理地址"]
        IPA -->|"Stage-2 VTTBR_EL2 EL2 掌控"| PA["PA 物理地址"]
        IPA -.->|"无效描述符"| AB["Stage-2 data abort → EL2"]
    end
    subgraph EPT["x86-64 EPT"]
        GVA2["Guest VA"] -->|"guest CR3 页表"| GPA["GPA 客户物理地址"]
        GPA -->|"EPT EPTP hypervisor 掌控"| HPA["HPA 宿主物理地址"]
        GPA -.->|"权限不足或不存在"| EV["EPT violation → VM exit"]
    end
```

**术语对照**：

| ARM64 | x86-64 | 说明 |
| --- | --- | --- |
| IPA（Intermediate Physical Address） | GPA（Guest Physical Address） | 完全同义 |
| PA | HPA（Host Physical Address） | 完全同义 |
| `VTTBR_EL2`（含 VMID） | `EPTP`（EPT pointer） | 页表基址寄存器 |
| VMID | 无直接对应；用 **VPID** 标记 TLB 项 | ARM 的 VMID 同时管 Stage-2 TLB 与 ASID 空间；x86 把这拆成 EPT 缓存 + VPID 两件事 |
| Stage-2 data abort | **EPT violation**（VM exit reason 48） | 触发 trap-and-emulate 的同一个机制 |
| L1 block（1 GB）/ L2 block（2 MB）/ L3 page（4 KB） | 1 GB / 2 MB / 4 KB EPT 页 | **粒度完全一致**（都是 4 级 4 KB granule 页表） |
| "punch hole"：把 1 GB block 拆成 L2 table 再置某项无效 | 同样操作：拆 1 GB EPT 项为 2 MB 表，把目标项权限清空 | **ADR-0004 的整套推导在 x86 上逐字成立** |

> **可复用的是职责，不是寄存器接口**：页表格式、TLB 失效与激活流程需要各 arch
> 实现。当前通用代码经 `vm_arch_init()` / `vcpu_arch_load()` 进入 ARM Stage-2，
> 不直接调用 `stage2_*`；本仓库没有通用 `stage2_map()` API。移植成本尚未实证。

---

## 5. 中断虚拟化：vGICv3 vs vLAPIC + APICv

对应 [vgic-injection.md](../arm/vgic-injection.md)、ADR-0001、ADR-0012。**这是移植成本最高的
子系统**——两边的硬件模型分歧最大。

```mermaid
sequenceDiagram
    autonumber
    participant DEV as 〔HW〕物理设备
    participant IC as 〔HW〕中断控制器
    participant HV as 〔root〕Hypervisor
    participant G as 〔non-root〕Guest 内核

    Note over DEV,G: ARM 稳态 timer 路径（PL011 不走 HW 转发）
    DEV->>IC: timer PPI 27 到 GICv3
    IC->>HV: 物理 IRQ 陷入 EL2 el2_irq_handler
    HV->>HV: vgic_inject_hw 写 ICH_LR n EL2 列表寄存器
    HV->>G: eret 进 guest 虚拟 IRQ 由 LR 呈现
    G->>G: guest 读 ICC_IAR1_EL1 EOI 全程不退出

    Note over DEV,G: x86 路径 APICv 开启时
    DEV->>IC: MSI / IOAPIC 到 LAPIC
    IC->>HV: 外部中断 VM exit 或 直接投递
    HV->>HV: 写 virtual-APIC page 的 VIRR 位 或 发 Posted Interrupt
    HV->>G: VMRESUME 硬件按 VIRR 自动注入
    G->>G: guest 读写 虚拟 APIC 寄存器 APICv 下不退出
```

| ARM64（本仓库） | x86-64 | 差异要点 |
| --- | --- | --- |
| GICv3 Distributor（GICD） | **IOAPIC**（+ MSI/MSI-X 直接寻址 LAPIC） | 都做"外设中断 → 路由到某个 CPU" |
| GICv3 Redistributor（GICR，每 CPU 一个） | **LAPIC**（每 CPU 一个） | 逐个对应，含 per-CPU 私有中断 |
| PPI（每 CPU 私有，如 timer） | LAPIC 本地中断源（LVT timer 等） | 逐个对应 |
| SPI（共享外设中断） | IOAPIC 引脚 / MSI | 逐个对应 |
| SGI（软件生成，核间） | **IPI**（Inter-Processor Interrupt，写 ICR） | 逐个对应；M9 的 SGI 虚拟化在 x86 = IPI 虚拟化 |
| **列表寄存器 `ICH_LR<n>_EL2`**（本仓库靠它注入） | **virtual-APIC page 的 VIRR 位图** | **模型不同**：ARM 是"最多 N 个待注入槽位"（本仓库固定用 LR0/LR1/LR2），x86 是"256 位的完整待决位图"——**x86 没有槽位耗尽问题** |
| HW 位 + pINTID（guest deactivate 释放物理 Active） | Posted Interrupt 可把中断直接投给运行中的 guest | 不能直接等同：当前 ARM timer 仍先陷入 EL2，HW 链接主要管理完成时机 |
| `vgicv3_mmio_init()` 陷入模拟 GICD/GICR | vLAPIC 陷入模拟（APICv 关时）/ APIC-access page（APICv 开时） | 同构：都是 trap-and-emulate 一组寄存器 |

> **当前接口**：`vgic_inject_sw/hw/spi/sgi` 都是 ARM 内部实现，LR0/1/2 分配不跨
> arch 边界。vuart 只请求 `vcpu_arch_inject_irq(target, irq)`；当前 arm64 只实现
> vuart SPI 用途。未来 x86 实现需要定义该请求如何交付，不能照搬 HW/pINTID 语义。

---

## 6. 电源与固件接口：PSCI vs ACPI + hypercall

guest 服务端在 `hypervisor/arch/arm64/vmexit/vpsci.c`；平台 SMC 客户端在
`arch/arm64/cpu/psci.c`。通用代码通过 `cpu_arch_power_on()` 启动物理核，不认识 PSCI。

| ARM64 | x86-64 | 说明 |
| --- | --- | --- |
| PSCI over `HVC`（`PSCI_VERSION`/`CPU_ON`/`CPU_OFF`/`SYSTEM_OFF`） | 无统一等价物：拆成 **ACPI**（S 状态、`_S5` 关机）+ **INIT-SIPI-SIPI**（启动 AP 核） | **x86 更零碎**：ARM 用一个 SMC/HVC 接口统一了"开核/关核/关机/重启" |
| `CPU_ON`（M9 用它拉起第二个 vCPU） | guest 发 **INIT-SIPI-SIPI** IPI 序列，hypervisor 陷入并模拟 | 同一目的，但 x86 要模拟一个三步握手时序 |
| `SYSTEM_OFF` / `SYSTEM_RESET` | guest 写 ACPI PM1a 控制端口（PIO 陷入）或三重故障 | x86 走 PIO 陷入路径 |
| HVC 作为 hypercall 载体（计划 M12 的 hypercall ABI） | **`VMCALL`** | 完全同构；ACRN 的 hypercall 就走 `VMCALL` |

> **已知缺陷的对照**：`CLAUDE.md` 记录的 `CPU_OFF` 缺陷（`PSCI_CPU_OFF` 误用
> `psci_power_down()` 把整个 VM 拉下线，应只停调用者 vCPU）在 x86 上是**同一个 bug 类**——
> "per-vCPU 关断状态 vs per-VM 关断状态"是体系结构无关的状态建模问题。M11 重写状态管理时
> 该按 per-vCPU 建模，这个结论两边通用。

---

## 7. 控制台与 I/O：一处 x86 独有的复杂度

本仓库 M10 的决策（ADR-0014）是：**EL2 独占物理 PL011，每个 VM 看到一个
trap-and-emulate vuart；`Ctrl-T` 进入 EL2 shell，用 `vm_console` 选择 RX 焦点**。这套设计在 x86 上**整体成立**，但有一个必须加的分支：

```mermaid
flowchart TD
    G["Guest 访问串口"] --> Q{"x86 走哪条路"}
    Q -->|"MMIO 映射的 UART"| M["EPT violation → VM exit<br/>与 ARM 完全同构"]
    Q -->|"传统 PIO 0x3F8 COM1"| P["IN OUT 指令 → I/O bitmap 陷入<br/>ARM 没有这条路"]
    M --> V["vuart 模拟层"]
    P --> V
    V -->|"TX，与焦点无关"| TX["写物理 UART"]
    RX["物理 UART RX"] --> F["console 仲裁：shell 或焦点 VM"]
    F -->|"guest 输入"| BUF["焦点 VM 的 RX FIFO"]
    BUF --> V
```

**PIO 是 x86 的第二 I/O 空间**：guest 用 `IN`/`OUT` 访问 `0x3F8`（COM1）这类端口，
不经过 EPT，靠 VMCS 的 **I/O bitmap** 按端口号决定是否陷入。ARM 侧没有任何对应物——
所有设备寄存器都是 MMIO，因此 Stage-2 一条路管完。

> **设备与架构分开**：`dm/vuart.c` 模拟 PL011 的 DR/FR/IMSC 等寄存器，焦点属于
> `dm/console.c`。若 x86 guest 仍看 PL011，可复用其设备语义；若改成 16550，就要
> 实现另一套寄存器模型。PIO 解码/总线是将来的需求，当前仓库没有该实现。

---

## 8. 模块关系：以已落地的 ADR-0015 为准

```mermaid
flowchart TD
    subgraph CORE["当前 arch 外代码"]
        VM["common/vm<br/>VM 对象与静态启动"]
        BUS["common/io<br/>MMIO 注册与分发"]
        DM["dm<br/>PL011 vuart / console / hv_shell"]
        HOOK["通用头文件<br/>cpu_/vm_/vcpu_arch_* + arch 类型入口"]
    end
    subgraph ARM["当前 arm64 实现"]
        CTX["vcpu_arch_run 汇编<br/>arch_regs / vcpu_arch"]
        MEM["Stage-2"]
        EXIT["handle_exit / data_abort / vpsci"]
        IRQ["GIC / vGIC"]
    end
    VM --> HOOK
    DM --> HOOK
    HOOK -. "实现" .-> CTX
    HOOK -. "实现" .-> MEM
    HOOK -. "实现" .-> IRQ
    EXIT --> BUS
    BUS --> DM
    BUS --> IRQ
    X86["未来 x86 需实现钩子与私有状态<br/>VMCS / EPT / APIC / exit 解码<br/>当前尚无实现"] -. "实现同一契约，仍需验证" .-> HOOK
```

`handle_exit` 与 PSCI 没有搬到 common；中性电源状态机属于 M11 后续工作。
调度器（M11）和用户态 virtio 后端（M14）也不是当前已有模块。检查脚本能阻止源码 /
头文件越界，不能证明不同架构的语义契约完全一致。

---

## 9. 里程碑对照：本仓库路线图在 x86 上的等价物

| 本仓库里程碑 | ARM 侧做的事 | x86 侧等价物 | 难度变化 |
| --- | --- | --- | --- |
| M0 Hello EL2 | `_start` 检查 `CurrentEL==0b1000`，进 EL2 | 检查 `CPUID` VMX 位 → 设 `CR4.VMXE` → `VMXON` | **x86 更繁琐**（要分配 VMXON region、对齐、revision id） |
| M1 裸机 guest | Stage-2 + 最小 vCPU | EPT + VMCS 初始化（约 100 个字段） | **x86 明显更繁琐**：VMCS 字段多且校验严，配错直接 VM-entry failure |
| M2 PSCI | HVC 陷入模拟 PSCI | `VMCALL` 陷入 + ACPI PIO 模拟 | 相当 |
| M3 vGIC 软件注入 | 写 `ICH_LR0_EL2` | 置 virtual-APIC page 的 VIRR 位 | **x86 略简单**（无槽位管理） |
| M4 物理 timer + HW 转发 | timer PPI → EL2 → HW 位转发 | LAPIC timer 陷入 / preemption timer | 相当 |
| M5–M8 Linux 启动 | arm64 boot protocol + DTB | x86 boot protocol（`boot_params`）或直接 ELF | 相当 |
| M9 SMP | PSCI `CPU_ON` + SGI 虚拟化 | INIT-SIPI-SIPI 模拟 + IPI 虚拟化 | **x86 略难**（三步握手时序） |
| M10 多 VM | per-VM Stage-2 / VMID / vGIC / vuart | per-VM EPT / VPID / vLAPIC / vuart(MMIO+PIO) | 相当，x86 多一条 PIO 路 |
| M11 调度器 | 上下文切换含 FP/SIMD | 上下文切换含 `XSAVE` 状态区 | 相当（都要处理"扩展状态不保存"这个假设） |
| M12–M14 hypercall/HSM/DM | HVC ABI + io_req ring + 用户态 DM | `VMCALL` ABI + io_req ring + 用户态 DM | **几乎一字不差**——这正是 ACRN 的原型 |
| M15 硬件移植 | RK3588（要运行时 FDT 解析） | 任一 Intel 平台（要 ACPI 解析） | 相当 |

> **值得注意的一点**：M12–M14（ACRN 模型的核心链路）在两个体系结构上**近乎完全相同**。
> 这不是巧合——`CLAUDE.md` 的 ACRN-model 策略之所以能从 x86 的 ACRN 借鉴到 ARM，
> 正是因为那一层已经在硬件差异之上了。反过来说，M0–M9 那些**最像"学 EL2 核心技术"
> 的部分，恰恰是最不可移植的部分**——这与本仓库把"学习 EL2 核心技术"列为首要目标是
> 自洽的。

---

## 10. 小结：三条可带走的结论

1. **同构的部分比想象中多**。顶层系统图、Service VM + DM 模型、MMIO 总线、VM 对象、
   退出分发骨架、电源状态机——这些在两个体系结构上逐行对应。ACRN 的结构能直接借鉴到
   ARM，原因就在这里。
2. **分歧集中在三处**：①上下文的软硬件分工（手动 save/restore vs VMCS 自动切换）；
   ②中断注入模型（LR 槽位 vs VIRR 位图）；③I/O 空间数量（只有 MMIO vs MMIO + PIO）。
   本仓库任何跨 arch 抽象，接口都不该泄漏这三处的细节。
3. **边界重构已落地，移植证明尚未完成**。vGIC 已被通用注入钩子隔离，MMIO 总线已
   与 ARM 解码分离；M11 新增状态和钩子仍需检查其它架构能否实现，不能把通过 grep /
   预处理检查当成 x86 支持。

---

## 参考

- Intel® 64 and IA-32 Architectures Software Developer's Manual, **Volume 3C**
  （VMX 全部内容：VMCS 布局、VM exit reason 表、EPT、APICv）
- Intel® Virtualization Technology for Directed I/O（VT-d，DMA 隔离）
- `../acrn-hypervisor` — 本文所有 x86 侧结论的对照实现；重点看
  `hypervisor/arch/x86/guest/`（vCPU/VMCS/vLAPIC）与 `hypervisor/dm/`（设备模型）
- ARM 侧对照见 [当前架构](../arm/current-architecture.md) 与 [目标架构](../arm/target-architecture.md)
