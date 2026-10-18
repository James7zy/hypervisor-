# vGIC 调试日志（CONFIG_DEBUG_VGIC）

开关定义在 `hypervisor/arch/arm64/vgic/vgic_debug.h`。当前只有 **3 个打印点**：
`vgic_v3_mmio.c` 的 GICD/GICR handler，以及 `irq/irq_handler.c` 的稳态 timer HW 注入。
**没有 vuart SPI、SGI 或远程 LR1 reload 的日志点**，不能用缺少这些日志判断链路失败。

配套阅读：[vGIC 注入](vgic-injection.md)、[MMIO 分发](handle-exit-dispatch.md)。

## 1. 开启与关闭

默认配置没有 `CONFIG_DEBUG_VGIC`，`vgic_dbg(...)` 展开为 `((void)0)`，不生成打印和
对应格式串。开启后 GIC MMIO 与持续 timer tick 会产生大量输出，只用于临时排查。

```sh
# 若尚无 .config，先 make defconfig；已有配置不要覆盖。
echo 'CONFIG_DEBUG_VGIC=y' >> .config
make clean && make
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
```

关闭时从 `.config` 删除该行，再 `make clean && make`。Makefile 不跟踪头文件 / 配置
变化，必须重建，不能只重链旧 `.o`。`make clean` 清理默认 build 时也会删除其测试子目录。

可检查当前 ELF 的格式串（不是 boot 实测）：

```sh
# 关闭时预期 0，开启时预期 3；grep -c 无匹配返回 1 是正常现象。
aarch64-none-linux-gnu-strings build/hypervisor.elf | grep -c '\[vgic\]'
```

这是当前调用点数量，不是稳定接口；新增打印点后数量会改变。

## 2. GICD/GICR 寄存器访问

handler 入口打印，以下为格式示意：

```text
[vgic] GICD rd off=0xffe8 size=4 data=0x0
[vgic] GICR rd off=0x8 size=8 data=0x0
[vgic] GICR rd off=0x20008 size=8 data=0x0
```

| 字段 | 含义 |
| --- | --- |
| GICD / GICR | 访问 distributor 或 redistributor 区域 |
| rd / wr | 读或写 |
| off | 相对整个注册区域基址的 offset；GICR 还包含 frame index |
| size | 访问宽度，字节 |
| data | 写时是 guest 写入值；读时是 handler **入口**的初始值，不是返回值 |

当前每 VM 两个 GICR frame，stride 为 `0x20000`。因此 `off=0x20008` 表示本 VM
vCPU1 的 GICR_TYPER，不是物理 pCPU1；真实物理核需加该 VM 的 `pcpu_base`。
日志不含 VM id，多 VM 的相同 offset 不能仅凭这一行区分归属。

## 3. timer 注入

```text
[vgic] inject HW PPI=27 (timer, ICH_LR0)
```

该行表示物理 timer PPI 已进入 EL2，且 guest VENG1 已开启，正在调用
`vgic_inject_hw()`。它使用 LR0 / HW=1，EL2 只 priority-drop；guest deactivate
通过 LR 链接释放物理中断。

secondary 启动早期的 VENG1=0 软件回退路径**不打印这行**。旧说明中的
`inject SPI=33 (PL011 RX, ICH_LR1)` 已没有对应代码，敲键不会产生它。

## 4. 排查时能得出什么结论

- 在确认 debug 构建正确、guest 已运行到 GIC 初始化后，若仍无 GICD/GICR 日志，
  再检查 Stage-2 hole 与总线注册；单凭“没有日志”不能断定 guest 直通了物理 GIC。
- GIC 访问正常但无 HW timer 行：区分 timer 尚未触发、物理 IRQ 路由未就绪，以及
  VENG1=0 回退路径。日志只覆盖其中一个分支。
- 键盘无响应：检查 `console_rx_drain → vuart_rx → vcpu_arch_inject_irq` 与目标核
  kick / pending gate，而不是寻找不存在的 SPI 日志。`make test-qemu-vspi` 有
  guest 可见的 RX/IPI/replay 断言，`make test-qemu-shell` 检查焦点和命令交互。
- 读日志的 `data=0` 不等于 guest 读回 0；需要读结果时在 handler 出口临时观察。

调试输出会扰动中断时序，不应代替关闭 debug 后的正常回归。最初用 instrumentation
查证 GIC 直通问题的过程保留在
[历史调查](2026-06-21-gicd-gicr-trap-investigation.md)，不是当前运行结果。
