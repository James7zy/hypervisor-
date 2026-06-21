# 客户机访问 GICD/GICR 怎么到 EL2？——一次从提问到实证的查证（2026-06-21）

本文记录一个看似简单的提问，如何一路追到 ADR-0012 与实际运行**不符**的实证结论。
保留中途两处被推翻的解释，因为「为什么它们错」本身就是这份文档的价值。

> **TL;DR**：ADR-0012 第 16-17 行称客户机的 GICD/GICR 内存映射访问「触发 Stage-2
> data abort → 影子模型 `vgic_v3_mmio.c` 模拟 → 从不落到真实硬件」。**一次带
> instrumentation 的真实 QEMU boot 证明这不成立**：客户机访问 **直通到了物理
> GICD/GICR**，两个 vgic handler **一次都没被命中**。配套 [[adr-0012]]、[[stage2]]、
> [[handle-exit-dispatch]]。

---

## 0. 最初的提问

> ADR-0012 说「它的 GICD/GICR 内存映射访问触发 Stage-2 data abort」。我对这里有疑问：
> 怎么做到访问 GICD/GICR 就能到 EL2 处理？

这是个好问题，触及 ARM 虚拟化最核心的硬件机制。

---

## 1. 第一层回答：两套机制，别混淆

ADR-0012 把 GIC 一分为二，**到 EL2 的路径也是两套不同机制**，问题问的是第二行：

| 客户机访问的东西 | 是什么 | 怎么到 EL2 |
| --- | --- | --- |
| `ICC_*_EL1`（CPU interface 系统寄存器） | `msr/mrs` 指令 | **不陷入** EL2！硬件经 `ICC_SRE_EL2` 透明重定向到 `ICV_*` |
| GICD/GICR（`0x08000000` 等**内存地址**） | 内存 load/store 指令 | （声称）**Stage-2 data abort** 陷入 EL2 |

### 当时给出的因果链（后被实证推翻其前半）

客户机每一个内存访问都走两级翻译：

```
客户机 VA ──Stage-1（客户机页表 TTBR*_EL1）──▶ IPA ──Stage-2（hv 页表 VTTBR_EL2）──▶ PA
```

当时的解释是：**hypervisor 在 Stage-2 里故意不给 GICD/GICR 的 IPA 建有效映射**，
于是访问即 fault，`HCR_EL2` 把 EL1 的 Stage-2 异常路由到 EL2。

> ⚠️ **这个「unmapped 所以 fault」的解释，后来被源码与实测推翻**（见 §3、§5）。
> 真实情况是 `l1_table[0]` 把 IPA 0–1GB **identity-map 成有效 Device block**，
> GICD/GICR 落在其中，访问直通物理硬件。

`ICC_*` 那一行——经 `ICC_SRE_EL2` 重定向到 `ICV_*`、根本不陷入——这部分始终正确。

---

## 2. 进 EL2 之后：trap-and-emulate（这部分实证为真）

陷入后到达 EL2 sync 向量，handler：

1. 读 `ESR_EL2`，确认 `EC == 0x24`（Data Abort from lower EL）。
2. 读 `ESR_EL2.ISS`：read/write、访问宽度、目标寄存器 `Rt`（SRT）。
3. 由 `HPFAR_EL2[43:4]`（页号）+ `FAR_EL2[11:0]`（页内偏移）拼出**出错 IPA**。
4. 按 IPA 范围 dispatch：GICD/GICR → `vgic_v3_mmio.c`；virtio → virtio 后端。
5. 模拟完写回 `Rt`（读时）、`ELR_EL2 += 4` 跳过指令、`eret` 回客户机。

源码逐项坐实（详见 [[handle-exit-dispatch]]）：

| 步骤 | 代码 | 位置 |
| --- | --- | --- |
| EC==0x24 判定 | `case 0x24: ... mmio_handle_data_abort()` | `vmexit/vmexit.c:48` |
| ISS 解码 | `ISS_SAS/ISS_SRT/ISS_WNR` 宏 | `vmexit/mmio.c:47-51` |
| 算 IPA | `((hpfar & ...) << 8) \| (far & 0xFFF)` | `vmexit/mmio.c:60-70` |
| IPA dispatch | `mmio_bus_lookup()` 线性查注册表 | `vmexit/mmio.c:36-44` |
| handler 注册 | `mmio_bus_register(BOARD_GIC_DIST_BASE…)` | `irq/vgic_v3_mmio.c:296-309` |
| 读回写 Rt | `regs->x[srt] = acc.data` | `vmexit/mmio.c:107-109` |
| 跳过指令 | `regs->elr_el2 += 4` | `vmexit/mmio.c:113` |

**这条 decode+emulate 链完全正确。** 问题不在「trap 之后怎么处理」，而在「trap 到底有没有发生」。

---

## 3. 查源码：Stage-2 建表反而**不会**让 GICD/GICR fault

`hypervisor/arch/arm64/mmu/stage2.c` → `stage2_init()` 整张 L1 表（512 entry）**只填 2 项**：

```c
/* stage2.c:38-39 —— l1_table[0]：IPA 0x0–0x3FFFFFFF identity → Device block */
l1_table[0] = 0x00000000UL |
              S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;

/* stage2.c:50-51 —— l1_table[1]：IPA 0x40000000–0x7FFFFFFF → PA 0x80000000，1GB RAM */
l1_table[1] = (ram_pa & 0xFFFFC0000000UL) |
              S2_BLOCK | S2_MEMATTR_NORM | S2_S2AP_RW | S2_SH_ISH | S2_AF;
```

VTCR `T0SZ=25`（39 位 IPA）、`SL0=1` 从 L1 起、4KB granule → L1 每 entry 覆盖 1 GB。
客户机眼里的 GIC 地址（用 **IPA**，board.h:15-16）：

- GICD `0x08000000`、GICR `0x080A0000` → 都落在 `[0, 0x40000000)` → **`l1_table[0]`**。

`l1_table[0]` 是个**有效的、identity-map 的 Device block**（`S2_BLOCK`、`S2_AF` 都置位），
把 IPA `0x08000000` → PA `0x08000000`。而 `run-qemu.sh` 用
`-machine virt,gic-version=3`，**QEMU 在 PA `0x08000000` 上实例化了真实物理 GICDv3**。

> **矛盾浮现**：按这份建表，客户机访问 GICD/GICR 会**直通到物理 GIC**，既不 fault、
> 也到不了 `vgic_v3_mmio.c`。这与 ADR-0012「从不落到真实硬件」**对不上**。

### 一个被推翻的「圆场」解释

查证过程中一度有人推断：「Stage-2 翻译成功 → PA 无设备背书 → 同步 external abort →
仍 EC 0x24 到 EL2」。**这个物理模型是错的**：`gic-version=3` 下那段 PA **有**真实
GIC，identity 直通会命中硬件、不会 external-abort。`stage2.c` 旁注与旧 DTS 注释里的
"UNBACKED" 字样，是 M3.0 跑**裸机 SVM guest**、尚未开物理 GIC 时的旧话，对
「Linux guest + gic-version=3」已过时。

---

## 4. 实证方法：instrumentation + 真实 boot

静态读码与文档互相矛盾，唯一定论办法是**看运行时实际发生什么**。

- 工具链：ARM GNU 14.2（`aarch64-none-linux-gnu-`）。
- guest：`linux-6.12.93/arch/arm64/boot/Image`。
- QEMU：`-machine virt,virtualization=on,gic-version=3 -cpu cortex-a72 -smp 1 -m 2G`。
- instrumentation：在 `vgicd_mmio_handler` / `vgicr_mmio_handler` 两个入口各加**一次性**
  `printk("[VERIFY] ... HIT ...")`，重新 build，抓 boot 日志看是否命中。

（GDB 断点法也试过，但批处理下 `continue` 会在断点不命中时永久阻塞，instrumentation
 法更可靠、证据更直观。验证完已 `git checkout` 还原 instrumentation。）

---

## 5. 实证结果（决定性）

```
[hv] vGICv3: GICD 0x8000000/0x10000 GICR 0x80a0000/0x20000 registered   ← 注册了软件总线
[    0.000000] GICv3: 256 SPIs implemented
[    0.000000] GICv3: GICD_CTRL.DS=1, SCR_EL3.FIQ=0
[    0.000000] GICv3: CPU0: found redistributor 0 region 0:0x00000000080a0000   ← 客户机直接读到真实 GICR
[    0.438826] 9000000.pl011: ttyAMA0 ... (irq = 13) ... PL011 rev1               ← 中断真的工作
（全程 grep [VERIFY] 命中数 = 0 → vgicd/vgicr_mmio_handler 从未被调用）
```

交叉验证 instrumentation 确实是活的（排除「printk 没生效」的假阴性）：

- `objdump -s -j .rodata` 在 PA `0x40083450` 看到完整字符串 `[VERIFY] vgicd_m...`；
- 同一份日志里 7 条 `[hv]` printk 正常输出 → printk 链路通、字符串已编入二进制；
- **但 `[VERIFY]` 命中 = 0** → 两个 handler 一次都没进。

`found redistributor 0 region 0:0x080a0000` 这一行是铁证：客户机的 GICv3 驱动**直接
读到了真实物理 GICR**，完成了 `256 SPIs` / `16 PPIs` / `GICD_CTRL.DS=1` 的完整探测，
PL011 SPI、arch timer 都正常——全部由**物理 GIC** 应答，而非影子模型。

---

## 6. 结论与影响

1. **ADR-0012 第 16-17 行与实现不符。** 客户机 GICD/GICR 访问**没有** trap、**没有**走
   `vgic_v3_mmio.c`，而是经 `l1_table[0]` 的 identity Device block **直通物理 GIC**。
2. **`vgicv3_mmio_init()` 只注册了软件 MMIO 总线 handler，没有在 Stage-2 给 GICD/GICR
   打洞（设 invalid）**，所以总线上的 handler 形同虚设——没有 fault 就永远查不到它。
   这与 M3.2「vGICv3 emulation」的设计意图矛盾。
3. **隔离性影响**：按 ADR-0012 自己的论证，客户机此时能直接写物理 `GICD_IROUTER`、
   关物理定时器使能——这个攻击面在当前 UP 单 VM 下虽未爆雷，但**真实存在**。

### 两条可能的修法（择一，待决策）

- **订正 ADR-0012**：如果有意让 UP Linux 直通物理 GIC（M3 阶段的简化），就把 ADR
  改成如实描述「identity 直通 + 软件总线备而未用」，并说明何时切到真模拟。
- **补 Stage-2 punch-hole**：在 `stage2_init`（或 vgic init）把 `l1_table[0]` 拆成更细
  的 L2/L3，将 GICD(`0x08000000`,64KB)/GICR(`0x080A0000`,128KB)对应页设为 invalid，
  让访问真的 fault → `mmio_bus_lookup` → vgic 影子模拟生效。这才符合 ADR-0012 当前文字。

> 完整逐步过程见本文；Stage-2 页表机制细节见 [[stage2]]；trap-and-emulate 分发见
> [[handle-exit-dispatch]]；物理 GIC 独占的设计意图见 [[adr-0012]]。
