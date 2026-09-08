# vGIC 调试日志（CONFIG_DEBUG_VGIC）

> 配套阅读：[[adr-0012]]（物理 GICv3 独占）、
> [2026-06-21-gicd-gicr-trap-investigation.md](2026-06-21-gicd-gicr-trap-investigation.md)
> （这两条路径最初是怎么用临时 instrumentation 实证的）、
> [vgic-injection.md](vgic-injection.md)（注入写 `ICH_LR` 的细节）。

本文说明仓库内置的 vGIC 调试日志开关怎么用、打印的每个字段是什么意思、典型输出怎么读。
代码见 `hypervisor/arch/arm64/vgic/vgic_debug.h` 及它的 4 个调用点。

---

## 1. 它是什么 / 为什么默认关

vGIC 的两条核心路径——**GICD/GICR 寄存器影子访问**与**中断注入**——各自插了调试打印，
由编译期开关 `CONFIG_DEBUG_VGIC` 控制：

- **默认关**（不在 `configs/qemu_virt_defconfig` 里）：`vgic_dbg(...)` 被预处理成
  `((void)0)`，**零开销**——不进 `.rodata`、不生成分支，ELF 里连 `[vgic]` 字符串都没有。
  默认 boot 输出与不带这套日志时**逐字节相同**。
- **手动开**：一次 boot 会打**几千行**（实测约 4294 行 / 一次启动到 shell）。量极大，足以
  淹没控制台，所以**只在排查 vGIC 问题时临时开**，不进默认配置。

---

## 2. 怎么开启

仓库用 `.config` → `-DCONFIG_FOO=1` 的机制（Makefile 第 39-40 行把 `CONFIG_FOO=y` 转成
编译宏），与现有的 `CONFIG_DEBUG_UART` 同理。开启 vGIC 日志：

```sh
make defconfig                          # 若还没有 .config
echo 'CONFIG_DEBUG_VGIC=y' >> .config   # 临时打开（不要改 defconfig）
make clean && make                      # 头文件依赖不被 Makefile 跟踪，必须 clean 重建
```

> **务必 `make clean`**：本仓库 Makefile 不跟踪头文件依赖，只改 `.config` / 头文件而不
> clean，旧 `.o` 会带着旧宏值重链（CLAUDE.md「Build constraints」一节有记）。

跑起来观察日志：

```sh
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
```

**关闭（恢复默认）**：把那行从 `.config` 删掉（或 `make defconfig` 覆盖），再 `make clean && make`。
注意 `.config` 不是 git 跟踪文件、`defconfig` 也不含此项，所以开关只影响你本地构建，不会误提交。

### 验证开/关是否生效

```sh
# 关：ELF 里应当 0 个 [vgic] 字符串（被编译期消除）
aarch64-none-linux-gnu-strings build/hypervisor.elf | grep -c '\[vgic\]'   # → 0
# 开：应当 4 个（对应 4 个调用点的格式串）
aarch64-none-linux-gnu-strings build/hypervisor.elf | grep -c '\[vgic\]'   # → 4
```

---

## 3. 日志字段含义

所有行都带 `[vgic] ` 前缀。共两类、4 个打印点。

### 3.1 寄存器影子访问（`vgic_v3_mmio.c`）

guest 对 GICD/GICR 的每次内存映射访问被 Stage-2 punch-hole 截获后，进入影子 handler 时打印：

```
[vgic] GICD rd off=0xffe8 size=4 data=0x0
[vgic] GICR wr off=0x0    size=4 data=0x1
```

| 字段 | 含义 |
| --- | --- |
| `GICD` / `GICR` | 落在哪个区域：GICD = distributor（`0x08000000`，SPI≥32）；GICR = redistributor（`0x080A0000`，本 PE 的 SGI/PPI 0–31）。 |
| `rd` / `wr` | 访问方向：`rd`=guest 读（`acc->is_write==0`）、`wr`=guest 写。 |
| `off=0x..` | **相对该区域基址的偏移**（不是绝对 IPA）。例：`off=0xffe8` = `GICD_PIDR2`/`GICR_PIDR2`（ID 寄存器，Linux GICv3 驱动探测时读的第一个）。 |
| `size=N` | 访问宽度（字节）：1/2/4/8。GICv3 的 64 位寄存器（如 IROUTER、GICR_TYPER）会出现 `size=8`。 |
| `data=0x..` | 写时=guest 写入的值；读时=**进入 handler 时**的 `acc->data`（读路径上此处通常还是 0，真正返回值由 handler 之后填入 `acc->data`，本行打印不反映返回值）。 |

> 注意 `data` 在 **读** 方向意义有限：日志打在 handler 入口，读返回值是在之后算出来的。
> 要看读返回了什么，看的是 guest 后续行为或在 handler 出口另加打印。写方向的 `data` 则是
> guest 真实写入值，可直接采信。

### 3.2 中断注入（`irq_handler.c` 的 `el2_irq_handler`）

物理中断进 EL2、被分流到注入函数前打印：

```
[vgic] inject HW PPI=27 (timer, ICH_LR0)
[vgic] inject SPI=33 (PL011 RX, ICH_LR1)
```

| 字段 | 含义 |
| --- | --- |
| `inject HW PPI=27` | EL1 虚拟定时器 PPI（INTID 27）走 **HW-forward** 注入（`vgic_inject_hw`），写列表寄存器 **`ICH_LR0`**，`HW=1` 让 guest 的 deactivate 经 LR 链路释放物理中断（ADR-0001）。 |
| `inject SPI=33` | PL011 UART 的 SPI（INTID 33，ttyAMA0 直通）走**软件注入**（`vgic_inject_spi`），写 **`ICH_LR1`**。每次键盘 RX 触发一次——开日志时敲一次键就多一行，可直观看到「键盘→中断→注入」闭环。 |

> 为什么 timer 用 LR0、PL011 用 LR1：vtimer 每 tick 重新注入会占用 LR0，PL011 用 LR1 避免被
> 覆盖（见 `vgic.c` 注释与 [vgic-injection.md](vgic-injection.md)）。

---

## 4. 典型 boot 片段怎么读

开 `CONFIG_DEBUG_VGIC` 后，启动早期 Linux GICv3 驱动探测 GIC 的一段长这样：

```
[vgic] GICD rd off=0xffe8 size=4 data=0x0     ← 读 GICD_PIDR2：确认是 GICv3
[vgic] GICD rd off=0x4    size=4 data=0x0     ← GICD_TYPER：拿 SPI 数 / CPU 数
[vgic] GICR rd off=0xffe8 size=4 data=0x0     ← 读 GICR_PIDR2
[vgic] GICR rd off=0x8    size=8 data=0x0     ← GICR_TYPER（64 位 → size=8）
[vgic] GICD wr off=0x84   size=4 data=0xffffffff  ← 写 IGROUPR：把 INTID 设为 Group1
...
[vgic] inject SPI=33 (PL011 RX, ICH_LR1)      ← 之后每次键盘输入各一行
```

**怎么用它判断问题**：
- 若 **完全没有** `[vgic] GIC*` 行 → guest 的 GIC 访问没被截获（punch-hole 失效，回退到直通
  物理 GIC）——正是 [investigation 文档](2026-06-21-gicd-gicr-trap-investigation.md) 修复前的症状。
- 若有 GICD/GICR 访问但 **没有 `inject` 行** → 中断没进 EL2 或没被分流（查 `HCR_EL2.IMO`、
  物理 GIC 使能、`el2_irq_handler` 的 INTID 分支）。
- 键盘输入 **不产生** `inject SPI=33` → PL011 SPI 注入链断（查 PL011 SPI 使能 / 直通）。

---

## 5. 注意事项

- **日志量巨大**（约 4 千行/启动）。只在排查时开；长会话可考虑把 `make run` 输出重定向到
  文件再 `grep`。
- **只开你需要的那条**：当前是一个总开关同时控制两类打印。若只想看注入、不想被寄存器访问
  刷屏，临时把 `vgic_v3_mmio.c` 里两行 `vgic_dbg` 注释掉重编即可（无需引入第二个 CONFIG）。
- **`data` 读方向的语义见 §3.1 的注记**——别把读行的 `data=0x0` 当成「guest 读到了 0」。
- 开关只影响本地构建（`.config` / `defconfig` 都不提交此项），不会污染默认配置或他人构建。

---

## 修改的文件

- **新增** `docs/reference/arm/vgic-debug-logging.md`（本文）。
- **更新** `docs/reference/README.md`（索引加一行）。

> 代码本身（`vgic_debug.h` + 两个调用文件）由 commit `feat(vgic): add CONFIG_DEBUG_VGIC
> trace logging` 落地，本文只是其使用说明。
