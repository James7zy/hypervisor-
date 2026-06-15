# M0 调试手册

目标：在 QEMU `virt` (AArch64) 上验证 hypervisor 进入 EL2 并打印 banner。

---

## 1. 环境准备

```sh
# 工具链（GCC 14.2，aarch64-none-linux-gnu-）
export PATH="$HOME/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"

# 验证工具链
aarch64-none-linux-gnu-gcc --version
# 期望：aarch64-none-linux-gnu-gcc (... 14.2...) ...

# 验证 QEMU
qemu-system-aarch64 --version
# 期望：QEMU emulator version 6.0+ （实际 9.2.4）
```

---

## 2. 编译

```sh
cd /home/corsair/Music/virtual/hypervisor/hypervisor-/.claude/worktrees/feat+m0-hello-el2

# 初次编译或 clean 之后
make defconfig
make

# 期望输出（无 warning，无 error）：
# aarch64-none-linux-gnu-gcc ... -c -o build/obj/boot/main.o ...
# aarch64-none-linux-gnu-ld ...  -o build/hypervisor.elf ...
# aarch64-none-linux-gnu-objcopy ... build/hypervisor.bin
```

清理重新编译：

```sh
make clean && make defconfig && make
```

---

## 3. 静态检查

编译成功后先做静态验证，不需要启动 QEMU。

```sh
# (a) .text 段地址必须是 0x40080000
aarch64-none-linux-gnu-objdump -h build/hypervisor.elf | grep -A1 "\.text"

# (b) entry point 必须是 0x40080000
aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep "Entry point"

# (c) _start 是第一个符号，地址 0x40080000
aarch64-none-linux-gnu-nm -n build/hypervisor.elf | head -5

# (d) 反汇编 _start 确认第一条指令
aarch64-none-linux-gnu-objdump -d build/hypervisor.elf | grep -A20 "<_start>"
```

期望结果：

```
.text   ... VMA 0x0000000040080000
Entry point address: 0x40080000
0000000040080000 T _start
```

---

## 4. 运行

### 4.1 交互模式（默认）

```sh
make run
# 等价于：
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf

# 期望输出（3 秒内）：
# [hv] Hello from EL2 on qemu_virt, CurrentEL=0x8

# 退出 QEMU：Ctrl-A x
```

### 4.2 捕获 serial 到文件（非交互）

```sh
timeout 8 qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial file:/tmp/hv_out.txt \
  -kernel build/hypervisor.elf; cat /tmp/hv_out.txt
```

### 4.3 telnet serial（后台运行）

```sh
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic \
  -serial telnet::3333,server,nowait \
  -kernel build/hypervisor.elf &

sleep 1
telnet localhost 3333
# 期望：[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
# 退出 telnet：Ctrl-] → quit
# 停止 QEMU：kill %1
```

---

## 5. GDB 调试

```sh
# 终端 1：启动 QEMU，暂停在 entry，等待 GDB 连接
QEMU_EXTRA_ARGS="-s -S" make run

# 终端 2：连接 GDB
aarch64-none-linux-gnu-gdb build/hypervisor.elf \
  -ex 'target remote :1234' \
  -ex 'b hypervisor_main' \
  -ex 'b uart_init' \
  -ex 'c'
```

常用 GDB 命令：

```
(gdb) x/5i $pc          # 查看当前 PC 附近指令
(gdb) info reg          # 所有通用寄存器
(gdb) x/s 0x09000000    # 直接读 PL011 DR 寄存器地址
(gdb) stepi             # 单步（指令级）
(gdb) si 10             # 步进 10 条指令
(gdb) c                 # 继续运行
(gdb) q                 # 退出
```

观察 EL 寄存器（在 `_start` 入口断点后）：

```
(gdb) b *0x40080000
(gdb) c
(gdb) si 3              # 步进到 mrs x9, CurrentEL 之后
(gdb) p/x $x9           # 期望 0x8（EL2）
```

---

## 6. 验证 early-panic 路径

验证当 EL 断言失败时会输出 `!EL`：

```sh
# (1) 临时修改：把 EL2 断言改为要求 EL3（head.S 第 25 行）
#     cmp x9, #(2 << 2)  →  cmp x9, #(3 << 2)

# (2) 重新编译
make

# (3) 运行，期望看到 "!EL" 而不是 banner
timeout 6 qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial file:/tmp/hv_panic.txt \
  -kernel build/hypervisor.elf; cat /tmp/hv_panic.txt

# (4) 恢复原始断言，重新编译
#     cmp x9, #(3 << 2)  →  cmp x9, #(2 << 2)
make
```

---

## 7. 常见问题排查

| 现象 | 可能原因 | 排查方法 |
|------|----------|----------|
| `aarch64-none-linux-gnu-gcc: not found` | PATH 未包含工具链 | `export PATH=...` 见第 1 节 |
| `make` 报 `.config not found` | 未执行 `make defconfig` | `make defconfig` |
| QEMU 无输出 | serial 重定向问题 | 改用 `-serial file:/tmp/out.txt` |
| `CurrentEL=0x4`（EL1）| `-machine` 缺少 `virtualization=on` | 检查 `scripts/run-qemu.sh` |
| `CurrentEL=0xc`（EL3）| QEMU 启动了 secure firmware | 去掉 `-bios` 参数 |
| `!EL` 出现在正常运行中 | EL 断言被修改未还原 | 检查 `head.S` 第 25 行 |
| 链接警告 `RWX segment` | 旧版 Makefile 缺少 flag | `$(LD)` 行加 `--no-warn-rwx-segments` |

---

## 8. 关键地址速查

| 符号 | 地址 | 说明 |
|------|------|------|
| `_start` / `.text` | `0x40080000` | ELF 入口，QEMU virt 默认 kernel 加载地址 |
| `__stack_top` | `0x40084000` + 16KiB | SP_EL2 初始值 |
| `BOARD_UART_BASE` | `0x09000000` | PL011 UART base（QEMU virt） |
| `hv_vectors` | 链接后确定 | VBAR_EL2，2KiB 对齐 |
