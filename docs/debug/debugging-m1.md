# M1 调试手册

目标：在 QEMU `virt` (AArch64) 上验证 Stage-2 MMU、vCPU 上下文切换和 HVC 分发。

---

## 1. 环境准备

```sh
# 工具链（GCC 14.2，aarch64-none-linux-gnu-）
export PATH="$HOME/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"

# 验证工具链
aarch64-none-linux-gnu-gcc --version
qemu-system-aarch64 --version   # 需要 6.0+（实际 9.2.4）
```

---

## 2. 编译

```sh
# Hypervisor
make defconfig
make

# SVM guest binary（M1 新增）
make svm
# 输出：build/svm/svm.elf  build/svm/svm.bin

# 一次性跑所有测试（偏移检查 + QEMU 集成）
make test
```

---

## 3. 静态检查

```sh
# (a) entry point 仍在 0x40080000
aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep "Entry point"

# (b) Stage-2 L1 页表符号存在且 4KB 对齐
aarch64-none-linux-gnu-nm build/hypervisor.elf | grep l1_table
# 期望：地址末 12 位全为 0，例如 0x400xxxxx000

# (c) struct 偏移量与汇编宏一致（编译期检查，无需 QEMU）
make check-offsets
# 期望：PASS: all struct offsets match assembly macros

# (d) SVM binary 起始指令是 movz（加载 HC_GUEST_DONE 低 16 位）
aarch64-none-linux-gnu-objdump -d build/svm/svm.elf | head -20
# 期望第一条：movz x0, #...  第二条：movk x0, #..., lsl #16
```

---

## 4. 运行

### 4.1 交互模式

```sh
SVM_BIN=build/svm/svm.bin make run
```

期望输出（5 秒内）：

```
  H   H Y   Y PPPP  ...
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
[hv] SVM: launching VMID=1 entry=0x40200000
[hv] SVM HVC: done (x1=0x0)
```

退出 QEMU：`Ctrl-A x`

### 4.2 非交互（自动化测试）

```sh
make test-qemu
# 输出：
# PASS: 'Hello from EL2'
# PASS: 'SVM: launching VMID='
# PASS: 'SVM HVC: done'
# ALL PASS
```

### 4.3 输出重定向到文件

```sh
SVM_BIN=build/svm/svm.bin timeout 10 qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial file:/tmp/hv_m1.txt \
  -kernel build/hypervisor.elf \
  -device loader,file=build/svm/svm.bin,addr=0x40200000
cat /tmp/hv_m1.txt
```

---

## 5. GDB 调试

### 5.1 启动

```sh
# 终端 A：QEMU 冻结在 _start，等待 GDB
SVM_BIN=build/svm/svm.bin QEMU_EXTRA_ARGS="-s -S" make run

# 终端 B：连接 GDB
aarch64-none-linux-gnu-gdb build/hypervisor.elf \
  -ex 'target remote :1234'
```

### 5.2 M1 关键断点

```
# Stage-2 页表初始化（填 l1_table，写 vttbr_el2 字段）
(gdb) b stage2_init

# 激活 Stage-2（msr vtcr_el2 / msr vttbr_el2 / isb）
(gdb) b stage2_activate

# eret 进入 guest 之前
(gdb) b vcpu_run

# 任意 VM exit 入口（el1_sync_handler 调用 handle_exit）
(gdb) b handle_exit

# HVC 分发（识别 EC=0x16）
(gdb) b handle_hvc

# SVM _start：guest 第一条指令
(gdb) b *0x40200000

# hv_restore：从 guest 返回 hypervisor
(gdb) b hv_restore
```

### 5.3 验证 Stage-2 页表

在 `stage2_init` 断点后执行完函数，检查：

```
(gdb) finish
(gdb) x/4xg &l1_table
# l1_table[0]（Device）：末 byte 含 0x01（block），bit 54（XN）应为 1
# l1_table[1]（Normal）：基地址 0x40000000，末 byte 含 0x01，无 XN

(gdb) p/x g_vm.vcpu.vttbr_el2
# bits[63:48] = 0x0001（VMID=1）
# bits[47:12] = l1_table 物理地址（4KB 对齐，末 12 位为 0）
```

### 5.4 验证 eret 前的寄存器

在 `vcpu_run` 里的 `eret` 指令处打断点（先反汇编找地址）：

```
(gdb) disas vcpu_run
# 找到 eret 那行，记下地址，例如 0x40082xxx
(gdb) b *0x40082xxx
(gdb) c
(gdb) info registers
# elr_el2  应为 0x40200000（SVM 入口）
# spsr_el2 应为 0x3c5     （EL1h，DAIF 全屏蔽）
```

### 5.5 验证 VM exit 的 ESR

在 `handle_exit` 断点停下后：

```
(gdb) p/x regs->elr_el2   # guest PC（出 exit 时指令地址）
(gdb) p/x esr             # Exception Syndrome Register
# HVC 正常：esr = 0x56000000（EC=0x16，ISS=0）
# Stage-2 Instruction Abort：esr = 0x820000xx（EC=0x20）
# Stage-2 Data Abort：esr = 0x940000xx（EC=0x24）
# Illegal Execution State：esr = 0x3a000000（EC=0x0E）
```

### 5.6 常用 GDB 命令速查

```
(gdb) c                        # continue
(gdb) si                       # 单步一条汇编指令
(gdb) ni                       # 单步（跳过 bl）
(gdb) finish                   # 运行到函数返回
(gdb) info registers           # 所有通用寄存器
(gdb) p/x $x0                  # 单个寄存器（十六进制）
(gdb) p g_vm.vcpu              # 打印 vcpu 结构体
(gdb) x/4xg 0x40200000        # 查看 SVM 代码（4 个 64-bit 值）
(gdb) x/512xg &l1_table       # 查看完整 Stage-2 L1 页表
(gdb) disas vcpu_run           # 反汇编函数
(gdb) layout asm               # TUI 汇编窗口
(gdb) layout src               # TUI 源码窗口
```

---

## 6. QEMU Monitor

QEMU 运行中按 `Ctrl-A c` 进入 monitor，再按一次切回串口：

```
(qemu) info registers          # 所有 CPU 寄存器（含系统寄存器）
(qemu) xp /4xg 0x40200000     # 查物理内存（不经 MMU）
(qemu) xp /4xg 0x09000000     # 查 PL011 UART 寄存器区
(qemu) info mem                # 当前有效内存映射
(qemu) q                       # 退出 QEMU
```

---

## 7. 常见失败模式

| 现象 | ESR / 输出 | 根因 | 修复 |
|------|-----------|------|------|
| `!VEC ESR=0x3a000000` | EC=0x0E Illegal Execution State | `HCR_EL2.RW`（bit 31）未置 1，EL1 被当成 AArch32 | `vm.c`：`hcr_el2 \|= (1ULL<<31)` |
| `EC=0x20 IFSC=0x5` | Stage-2 Translation Fault（取指） | `l1_table` 未 4KB 对齐，`VTTBR_EL2[11:0] != 0` | `stage2.c`：加 `__attribute__((aligned(4096)))` |
| `EC=0x20 IFSC=0x5` | Stage-2 Translation Fault（取指） | `VTCR_EL2` bit 31（RES1）未置 1 | `stage2.c`：`VTCR_EL2_VALUE` 加 `VTCR_RES1` |
| guest 跳到错误地址 | `elr_el2` 值不对 | `svm_config.entry` 配置错误 | 检查 `vm_config.h` 中 `entry = 0x40200000` |
| `EC=0x24`（Data Abort） | 访问了未映射 IPA | 正常隔离行为（如 IPA 0xC0000000） | 若非预期，检查 `stage2.c` 的 L1 覆盖范围 |
| `SVM_BIN: must be set` | make run 未传 SVM_BIN | 未先 `make svm` | `make svm` 或直接 `make test-qemu` |
| SVM 编译报 `immediate cannot be moved` | `mov x0, #0x80000001` 不可编码 | 立即数超出单指令范围 | 用 `register ... __asm__("x0")` 约束 |

---

## 8. ESR 解码速查

`ESR_EL2[31:26]` 是 EC（Exception Class）：

| EC（hex） | 含义 |
|-----------|------|
| `0x16` | HVC（AArch64），M1 正常退出路径 |
| `0x17` | SMC（AArch64） |
| `0x20` | Instruction Abort（lower EL），Stage-2 取指失败 |
| `0x24` | Data Abort（lower EL），Stage-2 数据访问失败 |
| `0x0E` | Illegal Execution State，通常是 HCR_EL2.RW 未设 |

解码 ESR：

```
EC       = (esr >> 26) & 0x3F
ISS      = esr & 0x1FFFFFF
IFSC/DFSC（Fault Status Code）= ISS & 0x3F
  0x05 = Translation Fault Level 1（L1 entry 无效）
  0x07 = Translation Fault Level 3
  0x0D = Permission Fault Level 1
```

---

## 9. 关键地址速查

| 符号 / 含义 | 地址 | 说明 |
|------------|------|------|
| `_start` | `0x40080000` | Hypervisor ELF 入口 |
| `BOARD_UART_BASE` | `0x09000000` | PL011 UART（QEMU virt） |
| `l1_table[0]` 覆盖 | `IPA 0x00000000–0x3FFFFFFF` | Device-nGnRE，含 UART，XN |
| `l1_table[1]` 覆盖 | `IPA 0x40000000–0x7FFFFFFF` | Normal WB，含 DRAM 和 SVM |
| SVM 加载地址 | `0x40200000` | `-device loader,addr=0x40200000` |
| `HC_GUEST_DONE` | `0x80000001` | SVM 用 HVC 传递的 func_id |
| `VMID` | `1` | `svm_config.vmid`，写入 `VTTBR_EL2[63:48]` |
