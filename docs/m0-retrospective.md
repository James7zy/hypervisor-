# M0 — Hello EL2 回顾总结

**完成日期：** 2026-05-24  
**Tag：** `m0`  
**目标：** 在 QEMU `virt` (AArch64) 上进入 EL2，打印 banner，然后 halt。

---

## 1. 目标与结果

| 项目 | 结果 |
|------|------|
| 进入 EL2 | ✅ `CurrentEL=0x8` 验证通过 |
| PL011 UART 输出 | ✅ banner 3 秒内打印 |
| 零 warning 编译 | ✅ `-Werror` 下无 warning |
| `.text` 加载地址 | ✅ `0x40080000` |
| early-panic 路径 | ✅ EL3 断言触发 `!EL` 输出 |

最终 banner：
```
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
```

---

## 2. 新增文件一览

### 目录骨架
仿照 ACRN `hypervisor/` 结构搭建，空目录用 `.gitkeep` 保留：

```
hypervisor/
├── arch/arm64/
│   ├── board/qemu_virt/   board.h, board.c, linker.lds
│   ├── board/rk3588/      .gitkeep（M4 预留）
│   ├── boot/              head.S, vectors.S
│   ├── cpu/               cpu.c
│   └── include/           asm/sysreg.h, board.h（shadow 占位）
├── boot/                  main.c
├── debug/                 uart_pl011.c
├── include/               types.h, uart.h, printk.h
└── lib/                   string.c, print.c
```

### 构建系统
```
Makefile                   顶层构建入口
hypervisor/Makefile        收集所有 obj
hypervisor/arch/arm64/Makefile
Kconfig / hypervisor/Kconfig / arch/arm64/Kconfig
configs/qemu_virt_defconfig
scripts/run-qemu.sh
```

---

## 3. 关键设计决策

### 3.1 board.h shadow 模型
`#include <board.h>` 在 arch-independent 代码中通过 `-I` 路径顺序解析到：
```
hypervisor/arch/arm64/board/qemu_virt/board.h
```
源码中不出现 board 名称。`hypervisor/arch/arm64/include/board.h` 是空占位文件，永远不会被实际 include。

### 3.2 board vs driver 分离
- `uart_pl011.c` 不含任何硬编码地址，通过 `uart_init(base)` 接收 base
- `BOARD_UART_BASE = 0x09000000` 只出现在 `board.h`
- 唯一例外：`head.S` 的 pre-C early-panic 路径直接写 UART DR 地址（C 尚未可用）

### 3.3 board_name
```c
// board.c
const char board_name[] = "qemu_virt";
// board.h
extern const char board_name[];
```
banner 通过 `%s` 打印，字符串不在驱动层硬编码。

### 3.4 memset-only string.c
M0 只实现 `memset`（BSS 清零用），`memcpy` 推迟到 M1。

### 3.5 向量表
16 个向量条目全部指向 `panic_vector` 存根（打印 ESR_EL2/ELR_EL2 后 wfi 循环），M0 不需要真正的异常处理。

---

## 4. 编译器约束

```
-ffreestanding -nostdlib -nostartfiles
-fno-pic -fno-stack-protector
-mgeneral-regs-only -mstrict-align
-Wall -Wextra -Werror -O2 -g
```

`-mgeneral-regs-only` 是强制约束：M0 不保存 FP/SIMD 状态，任何触发 FP 指令的代码都会导致未定义行为。

---

## 5. 遇到的问题与修复

| 问题 | 原因 | 修复 |
|------|------|------|
| `aarch64-linux-gnu-gcc: not found` | 工具链前缀错误 | 全局替换为 `aarch64-none-linux-gnu-`，Makefile 加 PATH fallback 指向 `~/Downloads/toolchain/` |
| `make defconfig` 被 `$(error)` 阻断 | `$(error)` 在 parse 阶段执行，`defconfig` 目标未豁免 | 用 `ifeq ($(filter defconfig clean ...,$(MAKECMDGOALS)),)` 包裹检查 |
| 链接器 RWX segment 警告 | GNU ld 14.x 新增此警告 | `$(LD)` 行加 `--no-warn-rwx-segments` |

---

## 6. 验收结果

```
[x] make 编译零 warning
[x] .text 地址 0x40080000
[x] entry point 0x40080000
[x] make run 3 秒内打印 banner
[x] CurrentEL=0x8（EL2）
[x] 强制 EL3 断言触发 !EL panic 路径（已还原）
[x] Ctrl-A x 正常退出 QEMU
```

---

## 7. M1 注意事项

- `memcpy` 需要在 M1 加入 `lib/string.c`
- Stage-2 MMU 启用后需要保存/恢复 FP/SIMD 寄存器（届时需重新评估 `-mgeneral-regs-only`）
- `hypervisor_main` 的 `dtb_phys` 参数目前被忽略，M1 解析 DTB 时需要启用
- 向量表存根需替换为真正的 sync/IRQ/FIQ 分发逻辑
