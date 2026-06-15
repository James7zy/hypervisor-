# M0 启动内部机制详解

涵盖 `head.S`、`vectors.S`、链接脚本三个核心文件的逐行解析，以及链接脚本在裸机工程中的通用作用。

---

## 一、head.S 解析

### 概览：启动流程

```
QEMU 跳转到 0x40080000 (_start)
    │
    ├─ 副核？→ secondary_park (wfi 死循环)
    │
    ├─ 不是 EL2？→ panic_early (输出 "!EL\n" 后 wfi 死循环)
    │
    ├─ 保存 DTB 地址 (x0 → x19)
    ├─ 设置栈 SP_EL2 = __stack_top
    ├─ 清零 BSS
    ├─ 安装向量表 VBAR_EL2
    ├─ 屏蔽所有中断/异常 (DAIF)
    └─ bl hypervisor_main  →  进入 C 世界
```

### 第一段：副核停泊

```asm
mrs     x9, mpidr_el1       ; 读 Multiprocessor Affinity Register
and     x9, x9, #0xFF       ; 取 Aff0 字段（= CPU 编号，0 = 主核）
cbnz    x9, secondary_park  ; 非 0 → 不是主核，跳走
```

QEMU 的所有 CPU（`-smp N`）都从同一个地址 `0x40080000` 开始执行。不加限制多个 CPU 会同时运行同一份代码，互相踩栈、踩 BSS。`Aff0 == 0` 是主核，其余全部进 `secondary_park` 的 `wfi` 死循环挂起。

### 第二段：EL 断言

```asm
mrs     x9, CurrentEL
cmp     x9, #(2 << 2)   ; EL2 = 0b10，左移 2 位 = 0x8
b.ne    panic_early
```

ARM 规范中 `CurrentEL` 寄存器的布局：

```
[63:4]  保留
[3:2]   EL 编码：00=EL0, 01=EL1, 10=EL2, 11=EL3
[1:0]   保留（读出为 0）
```

EL2 对应 `0b10`，整个寄存器值 = `0b1000` = `0x8`。`-machine virt,virtualization=on` 告诉 QEMU 在跳入代码前把 CPU 切到 EL2，不加这个参数默认是 EL1。

### 第三段：保存 DTB 指针

```asm
mov     x19, x0
```

QEMU 遵循 ARM Linux 启动协议，在 `x0` 里传入 DTB 物理地址。调用 `memset` 时 AAPCS64 允许被调用者修改 `x0–x18`，所以把 DTB 地址保存到 callee-saved 寄存器 `x19`（调用约定保证不被修改）。

### 第四段：设置启动栈

```asm
ldr     x9, =__stack_top
mov     sp, x9
```

必须在 `bl memset` 之前设置，因为函数调用需要栈来保存返回地址和 callee-saved 寄存器。`__stack_top` 由链接脚本在 `.bss` 之后预留 16KiB 空间后导出。AArch64 栈向低地址增长，SP 初始化为顶部（高地址端）。

### 第五段：清零 BSS

```asm
ldr     x0, =__bss_start
ldr     x2, =__bss_end
sub     x2, x2, x0          ; 长度 = end - start
mov     x1, #0
bl      memset
```

C 规范要求未初始化全局/静态变量启动时为 0。`.bss` 段在 ELF 文件里不占实际字节，QEMU 加载后内容是随机的，必须手动清零。`__bss_start` 和 `__bss_end` 由链接脚本导出。

### 第六段：安装向量表

```asm
ldr     x9, =hv_vectors
msr     vbar_el2, x9
isb
```

进入 C 代码前装好向量表，确保发生任何同步异常时 CPU 能跳到已知地址（panic stub）而不是随机地址。`isb` 保证后续指令看到新的 VBAR_EL2 值。

### 第七段：屏蔽中断和调试异常

```asm
msr     daifset, #0xF
dsb     sy
isb
```

| 位 | 含义 | 屏蔽什么 |
|----|------|---------|
| D  | Debug  | 调试异常（watchpoint、breakpoint）|
| A  | SError | 系统错误（异步外部中止）|
| I  | IRQ    | 普通中断 |
| F  | FIQ    | 快速中断 |

M0 没有初始化 GIC，屏蔽所有中断让初始化流程不受外设干扰，M2 再按需开放。

### 第八段：进入 C

```asm
mov     x0, x19         ; 恢复 DTB 地址 → hypervisor_main 第一参数
bl      hypervisor_main
1:  wfi
    b       1b           ; 防御性 halt（hypervisor_main 永不返回）
```

### panic_early：早期 panic 路径

```asm
panic_early:
    ldr     x9, =EARLY_UART_DR  ; 0x09000000（PL011 DR，唯一硬编码地址）
    mov     w10, #'!'
    str     w10, [x9]
    mov     w10, #'E'
    str     w10, [x9]
    mov     w10, #'L'
    str     w10, [x9]
    mov     w10, #'\n'
    str     w10, [x9]
2:  wfi
    b       2b
```

pre-C 路径无法使用 `board.h`，直接写 PL011 DR。这是整个项目**唯一被允许的硬编码 UART 地址**。

---

## 二、linker.lds 解析

### 全局声明

```ld
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)
```

`ENTRY(_start)` 让 `readelf -h` 看到的 Entry point 等于 `_start` 的地址（`0x40080000`）。QEMU 用这个字段决定 PC 从哪里开始执行。

### 加载基地址

```ld
. = 0x40080000;
```

`.`（位置计数器）设为 QEMU `virt` 用 `-kernel` 加载裸机 ELF 时的默认物理地址。代码中所有绝对地址引用必须和实际加载地址一致，否则运行时所有地址都是错的。

### `.text` 段顺序控制

```ld
.text : ALIGN(4) {
    KEEP(*(.text._start))   /* _start 必须排第一 */
    *(.text.vectors)        /* 向量表紧随其后 */
    *(.text*)               /* 其余所有代码 */
}
```

`KEEP` 防止 `-gc-sections` 把 `_start` 优化删除。顺序严格控制：`_start` 必须在 `0x40080000`，向量表紧跟其后（有 2KiB 对齐约束）。

### `.bss` 段与边界符号

```ld
.bss : ALIGN(16) {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
}
```

导出 `__bss_start` / `__bss_end` 供 `head.S` 计算清零范围。`.bss` 在 ELF 文件里不占实际字节，仅记录大小。

### 启动栈

```ld
. = ALIGN(16);
. += 0x4000;        /* 16 KiB */
__stack_top = .;
```

在 `.bss` 之后预留 16KiB，`__stack_top` 导出给 `head.S` 初始化 SP。

### 完整内存地址图

```
0x40080000  ┌────────────────────┐
            │  .text             │  _start → vectors → 其余代码
            ├────────────────────┤
            │  .rodata           │  字符串常量
            ├────────────────────┤
            │  .data             │  有初始值的全局变量
            ├────────────────────┤  ← __bss_start
            │  .bss              │  未初始化全局变量（清零后使用）
            ├────────────────────┤  ← __bss_end
            │  [对齐填充]         │
            ├────────────────────┤
            │  16 KiB 启动栈     │  SP 从顶部向下增长
            └────────────────────┘  ← __stack_top
```

### 丢弃无用段

```ld
/DISCARD/ : {
    *(.note.*)
    *(.comment*)
    *(.eh_frame*)
}
```

ELF note、编译器版本信息、C++ 异常展开表对裸机运行无用，丢弃减小体积。

---

## 三、链接脚本在工程中的作用

### 一句话概括

链接脚本控制**"代码和数据放在内存的哪里、按什么顺序、叫什么名字"**——这些是编译器不知道、只有工程师才清楚的事情。

### 三个核心职责

**1. 决定加载地址**

没有操作系统时，没有人帮你做地址重定向。不同平台起始地址完全不同：

| 平台 | 起始地址 | 原因 |
|------|---------|------|
| QEMU virt `-kernel` | `0x40080000` | QEMU 约定 |
| RK3588 SRAM | `0xFFFF0000` | 片上 SRAM 地址 |
| x86 实模式 | `0x7C00` | BIOS 约定 |
| 普通 Linux 进程 | 由内核 + PIE 决定 | 运行时确定 |

**2. 控制段的排列顺序**

如果让链接器自由排列，`uart_putc` 可能跑到 `_start` 前面，CPU 从 `0x40080000` 执行时跑到 UART 驱动代码，立刻崩溃。

**3. 导出边界符号给汇编/C 使用**

```ld
__bss_start = .;
__bss_end   = .;
__stack_top = .;
```

这些是**地址标签**（不是变量），由链接器在最终布局确定后填写。汇编通过 `ldr x0, =__bss_start` 使用，C 通过 `extern char __bss_start[]` 引用。

### 有 OS 和无 OS 的区别

| | 有 OS（Linux 进程）| 无 OS（裸机/hypervisor）|
|--|--|--|
| 地址布局 | 内核决定，程序不关心 | 工程师通过链接脚本完全控制 |
| 栈 | 内核分配，`sp` 启动时已就绪 | 链接脚本预留空间，汇编手动设置 SP |
| BSS 清零 | `libc` 的 `crt0.o` 负责 | `head.S` + `memset` 手动完成 |
| 链接脚本 | 发行版提供默认脚本，通常无需关心 | 必须自己写 |

### 在本项目中的 board 隔离

```
hypervisor/arch/arm64/board/qemu_virt/linker.lds   ← 当前
hypervisor/arch/arm64/board/rk3588/linker.lds      ← M4 时新增
```

M4 移植到 RK3588 时只需新写一份链接脚本，C 代码不用改。链接脚本是 board 隔离边界的一部分，和 `board.h` 共同承担这个职责。

---

## 四、vectors.S 解析

### AArch64 向量表结构

ARM 规范把 EL2 向量表分成 **4 组 × 4 类 = 16 个条目**，每条目 128 字节，合计 **2KiB**。

**4 组（来源）：**

| 组 | 含义 |
|----|------|
| Current EL with SP_EL0 | EL2 代码意外用了 EL0 的栈 |
| Current EL with SP_ELx | 正常情况：EL2 自己发生了异常 |
| Lower EL, AArch64 | Guest（EL1/EL0）触发 VM exit |
| Lower EL, AArch32 | 32 位 Guest 触发 VM exit |

**4 类（异常类型）：** Synchronous / IRQ / FIQ / SError

### 对齐约束

```asm
.align 11    /* 2^11 = 2048 字节对齐 */
```

ARM 硬性规定 `VBAR_EL2` 低 11 位必须全为 0。不满足时写入无效，CPU 跳到随机地址。

### `VECTOR_ENTRY` 宏

```asm
.macro VECTOR_ENTRY label
.align 7               /* 每条目 2^7 = 128 字节 */
b   \label
.endm
```

CPU 按 `VBAR + 固定偏移` 计算跳转地址：

```
Sync   = VBAR + 0x000 / 0x200 / 0x400 / 0x600（各组）
IRQ    = Sync + 0x080
FIQ    = Sync + 0x100
SError = Sync + 0x180
```

### M0 策略：全部指向 `panic_vector`

M0 没有 GIC、没有 Guest，任何异常都是 bug，直接 panic。

### `panic_vector` 实现

```asm
panic_vector:
    mrs  x0, esr_el2          /* 异常原因 */
    mrs  x1, elr_el2          /* 出错时的 PC */
    adrp x2, .Lpanic_fmt
    add  x2, x2, :lo12:.Lpanic_fmt
    mov  x3, x0
    mov  x4, x1
    mov  x0, x2               /* x0 = fmt  */
    mov  x1, x3               /* x1 = ESR  */
    mov  x2, x4               /* x2 = ELR  */
    bl   printk               /* "!VEC ESR=0x... ELR=0x...\n" */
1:  wfi
    b    1b
```

**ESR_EL2 关键字段：**

| 字段 | 位 | 含义 |
|------|----|------|
| EC | [31:26] | 异常类型（0x25=Data Abort, 0x16=HVC 等）|
| IL | [24] | 指令长度 |
| ISS | [23:0] | 具体信息 |

`adrp` + `:lo12:` 是 AArch64 加载 PC 相对地址的标准两步写法（单条指令无法编码任意 64 位立即数）。

### 向量表内存布局

```
hv_vectors  (2KiB 对齐)
├── +0x000  Sync   Current/SP_EL0  → panic_vector
├── +0x080  IRQ    Current/SP_EL0  → panic_vector
├── +0x100  FIQ    Current/SP_EL0  → panic_vector
├── +0x180  SError Current/SP_EL0  → panic_vector
├── +0x200  Sync   Current/SP_ELx  → panic_vector  ← 正常运行时用此组
├── +0x280  IRQ    Current/SP_ELx  → panic_vector
├── +0x300  FIQ    Current/SP_ELx  → panic_vector
├── +0x380  SError Current/SP_ELx  → panic_vector
├── +0x400  Sync   Lower/AArch64   → panic_vector  ← M1 Guest VM exit 入口
├── +0x480  IRQ    Lower/AArch64   → panic_vector
├── +0x500  FIQ    Lower/AArch64   → panic_vector
├── +0x580  SError Lower/AArch64   → panic_vector
├── +0x600  Sync   Lower/AArch32   → panic_vector  ← 32位 Guest
├── +0x680  IRQ    Lower/AArch32   → panic_vector
├── +0x700  FIQ    Lower/AArch32   → panic_vector
└── +0x780  SError Lower/AArch32   → panic_vector
```

### M1 会怎么改

`+0x400`（Lower EL, AArch64, Sync）替换为真正的 VM exit 处理程序：保存 Guest 寄存器、调度到 hypervisor 处理逻辑。IRQ/FIQ 条目逐步实现路由到 vGIC。`panic_vector` 保留为兜底。
