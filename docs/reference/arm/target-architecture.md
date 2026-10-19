# 目标架构 / Target Architecture（ACRN 模型，M12–M15）

> **这不是当前实现。** 本文画的是路线图终点的形态——Service VM + 用户态 Device Model
> 的完整 ACRN 模型，对应 M12（Hypercall ABI + VM 生命周期）、M13（HSM 内核驱动 +
> io_req 环）、M14（Device Model + virtio 后端）、M15（RK3588 移植）。截至 M10，
> 下面图里的 Device Model、HSM、virtio 后端、SMMU 隔离**都还不存在**。
>
> **当前实现**见 [current-architecture.md](current-architecture.md)：M10 静态分区与
> 已完成的 arch 边界重构，**不包含 M11 调度器**。里程碑与编号映射见
> [CLAUDE.md](../../../CLAUDE.md#milestone-roadmap)。
>
> 本文的两张图原先是 `CLAUDE.md` 里的 ASCII 大图，2026-09-14 迁出并改用 Mermaid 重画
> （`CLAUDE.md` 要求架构文档配 Mermaid 图）。x86 基座的对位版本见
> [../x86/architecture-x86.md](../x86/architecture-x86.md) §1、§2。

---

## 1. 顶层系统图

三类 VM 横向并列，特权与职责自上而下分层。Service VM 是唯一持有 Device Model 的
特权域，User VM 的虚拟设备全部由它在用户态后端提供；RTOS VM 走直通、不依赖 DM。

```mermaid
flowchart TD
    subgraph APP["应用 / Workloads"]
        direction LR
        APP_S["Linux Apps<br/>管理 / 云 / UI"]
        APP_U["Android / Linux Apps<br/>IVI / HMI / 通用"]
        APP_R["RT Apps<br/>控制任务"]
    end

    subgraph SVM["Service VM（Linux / SOS）"]
        direction TB
        DM["Device Model（用户态 dm）<br/>创建 / 启动 VM<br/>模拟虚拟设备<br/>处理 VM exit / MMIO<br/>提供 virtio 后端"]
        HSM["HSM 内核驱动<br/>ioctl + io_req 共享环"]
        TOOLS["管理工具<br/>acrnctl / config / launch"]
        TOOLS --> DM
        DM <--> HSM
    end

    subgraph UVM["User VM（Linux / Android）"]
        direction TB
        UOS["Guest OS<br/>virtio 前端<br/>虚拟设备 / guest 驱动"]
    end

    subgraph RVM["RTOS VM"]
        direction TB
        RTOS["RTOS Kernel<br/>RT 任务"]
    end

    subgraph HV["Hypervisor（EL2，本仓库）"]
        direction TB
        VMM["VM 管理<br/>创建 / 销毁"]
        SCHED["vCPU 调度器<br/>vCPU 分发（M11）"]
        MEM["内存管理<br/>Stage-2 隔离"]
        EXIT["VM exit 处理<br/>转发 exit 给 DM"]
        IRQ["中断管理<br/>GIC IRQ 路由"]
        IOV["I/O 虚拟化<br/>MMIO / 设备直通"]
    end

    subgraph HW["ARM 硬件 / SoC"]
        direction LR
        CPU["ARM CPU Cores<br/>EL1 Guest / EL2 HV"]
        RAM["DDR / LPDDR"]
        DEV["物理设备<br/>UART / GPU / NPU / PCIe / 存储"]
    end

    APP_S --> SVM
    APP_U --> UVM
    APP_R --> RVM

    DM <-->|"virtio 前后端"| UOS
    SVM -->|"Hypercall / ioctl<br/>VM 生命周期控制"| HV
    UVM -->|"VM exit / trap<br/>MMIO / IRQ"| HV
    RVM -->|"VM exit / trap"| HV
    HV --> HW

    classDef svm fill:#e6f0ff,stroke:#06c,color:#000;
    classDef uvm fill:#ffe6e6,stroke:#c33,color:#000;
    classDef rvm fill:#e8f5e9,stroke:#2e7d32,color:#000;
    classDef hv fill:#fff4e5,stroke:#e67700,color:#000;
    class DM,HSM,TOOLS svm;
    class UOS uvm;
    class RTOS rvm;
    class VMM,SCHED,MEM,EXIT,IRQ,IOV hv;
```

**读图要点**

- **Service VM 的双层结构是 M13/M14 的核心**：`HSM` 是内核态驱动（仿 `acrn_hsm`），
  通过 io_req 共享环把 User VM 的 MMIO exit 递到用户态；`DM` 是用户态程序，真正
  完成模拟。EL2 只负责把 exit **转发**出去，不含 virtio 后端逻辑——这是
  「不写两遍后端」那条决策（见 [ADR-0016](../../adr/0016-acrn-model-roadmap-decisions.md) 第 3 条）。
- **RTOS VM 不连 DM**：它走直通路径，这也是 SMMU/DMA 隔离最终会落在哪的原因。
- **图中功能不等于已有目录**：当前 VM 编排在 `common/vm/`，MMIO 总线在
  `common/io/`；ARM exit 解码在 `arch/arm64/vmexit/`，Stage-2 / 中断实现在
  `arch/arm64/{mmu,irq,vgic}/`。调度器和向 Service VM 转发 exit 的链路尚未实现。
  当前 EL2 `dm/` 包含 vuart、console 和 hv_shell；未来用户态 DM 不等于这个目录。

---

## 2. 特权层次图

ARM64 的四级异常等级是一条线性阶梯，虚拟化扩展位于 EL2。

```mermaid
flowchart TD
    subgraph GUEST["Guest VM / Service VM / RTOS VM"]
        direction TB
        EL0["EL0：用户态应用"]
        EL1["EL1：Guest OS 内核<br/>Linux / Android / RTOS"]
        EL0 --> EL1
    end

    subgraph HYPER["Hypervisor（本仓库）"]
        EL2["EL2：Hypervisor Mode<br/>vCPU 调度<br/>Stage-2 翻译<br/>trap and emulate<br/>中断虚拟化<br/>设备直通控制"]
    end

    subgraph SECURE["可选安全世界"]
        EL3["EL3：Secure Monitor<br/>Trusted Firmware-A / PSCI"]
    end

    subgraph SOC["ARM SoC 硬件"]
        direction TB
        FEAT["ARM 虚拟化扩展 / EL2<br/>Stage-2 地址翻译<br/>GICv3 / GICv4<br/>SMMU / IOMMU（DMA 隔离）<br/>PSCI / 电源管理"]
        PHYS["ARM Cores | DDR | GICv3/v4 | SMMU | PCIe | MMIO 设备 | DMA"]
        FEAT --- PHYS
    end

    EL1 -->|"HVC / trap"| EL2
    EL2 -->|"eret"| EL1
    EL2 -->|"SMC"| EL3
    EL2 --> SOC

    classDef guest fill:#ffe6e6,stroke:#c33,color:#000;
    classDef hyper fill:#fff4e5,stroke:#e67700,color:#000;
    classDef secure fill:#ede7f6,stroke:#5e35b1,color:#000;
    classDef soc fill:#eceff1,stroke:#546e7a,color:#000;
    class EL0,EL1 guest;
    class EL2 hyper;
    class EL3 secure;
    class FEAT,PHYS soc;
```

**读图要点**

- **EL2 与 EL1 寄存器组分离不等于 vCPU 状态已隔离**：当前静态绑核让 EL1 状态留在
  所属 pCPU；M11 同核切换两个 vCPU 时，仍需保存 EL1 系统寄存器、FP/SIMD、timer
  和 vGIC 等状态。对照见 [../x86/architecture-x86.md](../x86/architecture-x86.md) §2。
- **guest PSCI 与平台 PSCI 分开**：`arch/arm64/vmexit/vpsci.c` 处理 guest 的 HVC，
  `arch/arm64/cpu/psci.c` 通过 SMC 调用平台 PSCI。QEMU 提供平台实现，不需要单独
  加载 TF-A；RK3588（M15）需对接实际固件，而不是把 guest 请求直接透传。
- **SMMU 目前是空格子**：所有 User VM 设备都由 DM 模拟，不需要 DMA 隔离；等 M15 真机
  直通时才排期（见 roadmap 的 Deferred 行）。

---

## 与当前实现的差距

| 图中元素 | 现状（M10） | 计划 |
| --- | --- | --- |
| Service VM / 特权域概念 | 无，两个 VM 对等 | M12 |
| Hypercall ABI（区别于 PSCI） | PSCI over HVC + `HC_GUEST_DONE` 测试钩子，无管理 ABI | M12 |
| HSM 内核驱动 + io_req 环 | 无 | M13 |
| Device Model（用户态） | 无；EL2 `dm/` 为 vuart + console + hv_shell | M14 |
| virtio 前后端 | 无，guest 用 vuart + initramfs | M14 |
| vCPU 调度器 | 无，1:1 静态绑核 | M11 |
| SMMU / DMA 隔离 | 无 | Deferred |
| 运行时 FDT 解析 | 静态板级配置（ADR-0008） | M15 |

---

## 延伸阅读

- [current-architecture.md](current-architecture.md) — **当前实现**的模块、对象与时序图。
- [2026-06-21-architecture-zoom-out.md](2026-06-21-architecture-zoom-out.md) — 单 VM 时代
  的历史快照，不作为当前代码导航。
- [../x86/architecture-x86.md](../x86/architecture-x86.md) — 同样两张图的 x86-64 VT-x 基座
  重画，附术语对照表（EL2↔VMX root、Stage-2↔EPT、vGICv3↔vLAPIC、PSCI↔ACPI）。
- [[../../adr/0014-multi-vm-static-partition-el2-console]] — M10 的多 VM 静态分区、
  per-VM Stage-2/vGIC、EL2 独占控制台。
- [CLAUDE.md](../../../CLAUDE.md#milestone-roadmap) — M0–M15 路线图；路线图不依赖仓库外的本地 skill。
- [ADR-0016](../../adr/0016-acrn-model-roadmap-decisions.md) — ACRN 模型的路线图排序决策。
- [ADR-0015](../../adr/0015-arch-boundary-core-vs-arch.md) — 已落地的 core/arch 边界。
