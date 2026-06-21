# Stage-2 punch-hole for GICD/GICR — design

> **Date:** 2026-06-21 **Status:** Approved (brainstorming) **Scope:** single file `stage2.c`

## 背景与动机

2026-06-21 的一次实证 boot（见 `docs/reference/2026-06-21-gicd-gicr-trap-investigation.md`）
证明 ADR-0012 描述的 vGICv3 影子模拟**当前不生效**：

- `stage2.c` 的 `l1_table[0]` 把 IPA `0x00000000–0x3FFFFFFF` identity-map 成一个**有效的
  1 GB Device block**，GICD(`0x08000000`)/GICR(`0x080A0000`)落在其中。
- QEMU `-machine virt,gic-version=3` 在那段 PA 上有真实物理 GICv3。
- 于是客户机访问 GICD/GICR **直通物理硬件**——两个 `vgic_v3_mmio.c` handler 一次都没命中
  （boot 日志 `[VERIFY]` 命中数 = 0，而 `GICv3: CPU0: found redistributor` 是客户机直接
  读真实 GICR 的铁证）。

`vgicv3_mmio_init()` 只往软件 MMIO 总线注册了 handler，**没有在 Stage-2 给 GICD/GICR
打洞**。没有 fault，总线上的 handler 形同虚设。本设计补上这一步。

## 目标

让客户机对 GICD/GICR 的内存映射访问真的触发 Stage-2 translation fault → EC 0x24 →
`mmio_handle_data_abort` → `mmio_bus_lookup` → `vgic_v3_mmio.c` 影子模拟，**不再落到物理 GIC**，
使 ADR-0012 的隔离模型如实成立。

非目标：不改 vgic 模拟逻辑本身、不改 MMIO 分发框架、不动 RAM 映射、不涉及 SMP/多 GICR。

## 关键事实（已实测核对）

- VTCR `T0SZ=25`（39 位 IPA）、`SL0=1`（L1 起步）、4KB granule → **L1 每 entry = 1 GB，
  L2 每 entry = 2 MB**。
- `0x08000000 >> 21 = 64`、`0x080A0000 >> 21 = 64` → **GICD 与 GICR 同落 L2 entry #64**
  （2 MB 区间 `0x08000000–0x081FFFFF`）。GICR 末址 `0x080BFFFF < 0x081FFFFF`，确实在 #64 内。
- PL011 `0x09000000 >> 21 = 72` → 在 entry #72，**不受影响**，仍 Device 直通。
- guest `qemu_virt.dts` 在 `0x08000000–0x081FFFFF` 这 2 MB 内**只声明了 gic**（GICD+GICR），
  无其他需直通设备 → punch 整个 entry #64 无副作用。

结论：**只需把 1 个 L2 entry(#64)设为 invalid**，其余 511 个仍是 2 MB Device block 直通。

## 设计

### 架构

把 `l1_table[0]` 从「1 GB Device block」改为「table descriptor 指向新静态表 `l2_dev[512]`」。
`l2_dev` 默认整张是 2 MB Device block 直通（identity，输出 PA = `i << 21`），唯独 entry #64
设为 0（invalid）。

```
l1_table[0]  ── table desc ──▶  l2_dev[512]
                                  l2_dev[0..63]   = 2MB Device block (identity, 直通)
                                  l2_dev[64]      = 0  (invalid → GICD+GICR fault)
                                  l2_dev[65..511] = 2MB Device block (含 PL011 @ #72)
l1_table[1]  ── 1GB block ─────▶ guest RAM (PA 0x80000000)   ← 不变
```

### 改动点（单文件 `hypervisor/arch/arm64/mmu/stage2.c`）

1. 新增宏 `#define S2_TABLE 0x3ULL`（table descriptor：bit[1:0]=0b11）。
2. 新增 `static u64 l2_dev[512] __attribute__((aligned(4096)));`。
3. `stage2_init` 内：
   - 循环把 `l2_dev[i]` 填为 2 MB Device block：
     `((u64)i << 21) | S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN`。
   - 用 `BOARD_GIC_DIST_BASE >> 21` 算 punch 索引（避免硬编码 64），断言 GICD 与 GICR
     同属一个 2 MB entry：`(BOARD_GIC_DIST_BASE >> 21) == (BOARD_GIC_RDIST_BASE >> 21)`；
     不满足则 `printk` 告警（地址布局变更的早期信号）。
   - 把该 entry 设 0（invalid）。
   - `l1_table[0] = (u64)(uintptr_t)l2_dev | S2_TABLE;`（原为 block，改 table desc）。
4. `l1_table[1]`（RAM）与 `vcpu->vttbr_el2` 赋值**不变**。

### 不变量与风险

- **TLB**：`stage2_activate` 已有 `dsb ish` / `isb`，且建表发生在 `eret` 进 guest 之前，
  无活跃 stale 翻译，无需额外 TLBI。
- **MemAttr**：punch 的页是 invalid（不涉及 attr）；直通页保持 Device-nGnRE，与原 1 GB
  block 同语义。
- **对齐**：`l2_dev` 4 KB 对齐（table desc 的输出地址低 12 位须为 0）。
- **`-mstrict-align` / `-mgeneral-regs-only`**：纯整数数组写，无影响。
- **依赖**：`stage2.c` 需能见到 `BOARD_GIC_DIST_BASE`/`BOARD_GIC_RDIST_BASE`（board.h，
  已在 include 路径）。

## 验证

沿用 2026-06-21 实证手法：

1. `make` 零警告（`-Werror`）。
2. 临时在 `vgicd_mmio_handler` / `vgicr_mmio_handler` 入口加一次性 `[VERIFY]` printk。
3. 真实 boot：`LINUX_IMAGE=…/linux-6.12.93/Image make run`（QEMU virt, gic-version=3, -smp 1, -m 2G）。
4. **预期**：日志出现 `[VERIFY] vgicd_mmio_handler HIT` 与/或 `vgicr_mmio_handler HIT`
   （上次为 0 次）；`GICv3: CPU0: found redistributor` 仍出现，但此时由**影子模型**应答。
5. 撤掉 instrumentation，重新 `make` 干净。

通过判据：两个 handler 至少各被命中一次（证明 GICD 与 GICR 都走影子模拟，不再直通物理 GIC）。

## 后续

- 本改动使 ADR-0012「从不落到真实硬件」如实成立；ADR-0012 文字无需再订正，但可加一句
  指向本 spec / reference 文档说明 punch-hole 的实现位置。
- M3.5(SMP)会引入多个 GICR frame，届时 punch 范围与 `l2_dev` 的 per-PE 处理需重访。
