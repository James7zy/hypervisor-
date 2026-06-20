# M3 启动到 Shell — 调试实录(可重现教程)

目标:在 QEMU `virt` (AArch64, GICv3, `-smp 1`) 上,把一个**未修改的** arm64
Linux 启动到**可交互**的 busybox shell —— 这是 M3 的收尾目标。

本文是一次真实调试的**完整复盘**:M3.0–M3.4 此前只做过静态验证(编译干净 +
`readelf`),从未真正在 QEMU 里端到端跑过(`make run` 被当作 "operator handoff"
推迟了)。第一次实跑,连续暴露了 **3 个 bug**。下面逐步记录:每一步**用什么命令、
看到什么、推出什么、改了什么**,方便你重现和学习定位手法。

结论速览(三个 bug,依次串联):

| # | 症状 | 根因 | 修复 |
|---|------|------|------|
| 1 | guest 打印 `!EL`,执行的是 hv 代码 | Stage-2 L1 1 GB block 输出 PA 必须 1 GB 对齐,`ram_pa=0x48000000` 被掩码抹成 `0x40000000`(hv 镜像) | RAM 挪到 1 GB 对齐的 `0x80000000` |
| 2 | guest 取指外部中止,`EC=0x20 ELR=0x200` | PA `0x80000000` 在 `-m 1G` 下正好是 DRAM 末尾**之外**,无内存 | QEMU 改 `-m 2G` |
| 3 | shell 出来后一敲键盘 hv 崩溃 `!VEC` | `eret` 前 `msr daifclr,#2` 开了 IRQ,IRQ 在 EL2 被取到 → current-EL 向量 → `panic_vector` | 删掉 `daifclr,#2`;再补全 PL011 输入注入 |

---

## 0. 调试纪律:先建反馈环

整套调试的核心是**一个快、确定、可重复的信号**。这里天然就有:

```sh
# 一次完整启动尝试,串口输出落盘,超时兜底(shell 是交互的,不会自己退出)
timeout 80 qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 -cpu cortex-a72 -smp 1 -m 2G \
  -nographic -serial mon:stdio -kernel build/hypervisor.elf \
  -device loader,file=$IMAGE,addr=0x80080000 \
  -device loader,file=build/guest/guest.dtb,addr=0x82000000 \
  -device loader,file=$INITRD,addr=0x84000000 \
  </dev/null >/tmp/boot.log 2>&1
```

两个**加 sharpness 的开关**(全程靠它们定位):

- `-d int -D /tmp/int.log` —— 记录每一次异常进入/返回(EL 转换、ESR、ELR、FAR)。
  **这是本次定位 bug 2、3 的决定性工具。**
- `-d in_asm,cpu -D /tmp/asm.log` —— 记录执行过的指令块 + 寄存器,用于确认
  "guest 到底在执行谁的代码"。

> 坑:`-serial stdio` 在 virt 上会和默认串口冲突(`cannot use stdio by multiple
> character devices`),要用 `-serial mon:stdio`。喂输入用管道 + `sleep` 定时:
> `( sleep 8; printf 'ls\n'; sleep 2 ) | qemu ... </dev/null`。

---

## 1. 环境准备(内核 + initramfs 都要自备)

CLAUDE.md 写明:内核 `Image` 和 initramfs **不在本仓库构建**,由你提供。

```sh
# 工具链(不在默认 PATH)
source /home/corsair/Downloads/toolchain.sh   # aarch64-none-linux-gnu- 14.2

# (a) 交叉编译 arm64 Linux —— 注意要"瘦身",见下面的坑
cd ~/Music/virtual/linux-6.12.93
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- -j"$(nproc)" Image

# (b) 静态 aarch64 busybox
git clone --depth=1 https://git.busybox.net/busybox && cd busybox
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- defconfig
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- -j"$(nproc)"
file busybox   # 必须是 "ELF ... ARM aarch64 ... statically linked"

# (c) 组 initramfs(见 docs/reference/guest-initramfs.md),/init 末尾 exec /bin/sh
```

**坑 A —— 内核镜像大小预算。** hv 的地址布局给 Image 留的空间有限:

```sh
python3 -c "print('Image 预算 =', (0x4a000000-0x48080000)//1024//1024, 'MB')"  # 31 MB
```

`defconfig` 编出来的 Image 有 37 MB,**超过 31 MB 预算**,QEMU 直接报
`ROM regions are overlapping`(Image 盖到 DTB 上)。解决:裁掉用不到的大块
(`NET/PCI/USB/DRM/DEBUG_INFO/...`,保留 PL011、virtio、devtmpfs、initramfs),
Image 降到 28 MB。

**坑 B —— Makefile 不跟踪头文件依赖。** 没有 `.d` 文件,改 `board.h` 之类的头
**不会触发重编**,`make` 会拿旧 `.o` 重链接(banner 还打印旧值)。**改头后必须
`make clean`。** 见 [[toolchain-no-header-deps]]。

---

## 2. 第一次跑:`!EL` —— guest 在执行 hypervisor 的代码(Bug 1)

```sh
# (build hv + dtb 后)第一次实跑
... -d int,cpu ...
```

串口输出:

```
[hv] Hello from EL2 ...
[hv] Linux guest: VMID=1 entry=0x40080000 dtb=0x42000000 ram_pa=0x48000000
...
!EL
```

**关键观察:`!EL` 是谁打印的?** 全仓库搜:

```sh
grep -rn '!EL' hypervisor/        # → head.S:61 panic_early
```

`!EL` 只可能从 `head.S` 的 `panic_early` 出来,而它只在 `_start` 里
`CurrentEL != EL2` 时跳到。但 hv 的 banner 已经打印过了(boot CPU 早过了这关)。
**那为什么又回到 `_start`?**

**用指令 trace 验证 guest 在执行什么:**

```sh
... -d in_asm,cpu -D /tmp/asm.log ...
grep -E '^ PC=0000000040080000' /tmp/asm.log   # 找 guest 入口执行点
```

看到入口处执行的指令字节是 `a90038d5...`(= `mrs x9, mpidr_el1`),再往下到
`0x40080060` 执行的正是 `panic_early`:把 `'!' 'E' 'L' '\n'`(`X10=0x21=='!'`)
写到 UART base `X09=0x09000000`。

**推论:guest 在 IPA `0x40080000` 执行的是 hv 自己的 `head.S`,不是 Linux。**
说明 Stage-2 把 guest 的 RAM 映射到了**错误的 PA**。

**算 Stage-2 描述符:**

```sh
python3 -c "print(hex(0x48000000 & 0xFFFFC0000000))"   # → 0x40000000
```

看 `stage2.c`:用了**单个 L1 1 GB block**映射 guest RAM:
`l1_table[1] = (ram_pa & 0xFFFFC0000000) | ...`。L1 block 是 1 GB,输出 PA 必须
1 GB 对齐;而 `ram_pa = 0x48000000` 只有 128 MB 对齐,掩码 `& 0xFFFFC0000000`
把它**抹成了 `0x40000000`** —— 正好是 hv 镜像的位置。ADR-0004 里甚至写了错误的
前提"`0x4800_0000` 是 1 GB 对齐"。

### 地址位解析图:为什么 `0x48000000` 会被抹成 `0x40000000`

本项目 `VTCR_EL2`:`T0SZ=25`(39-bit IPA)、`SL0=1`(从 L1 开始)、`TG0=0`
(4 KB granule)。39-bit IPA 在 4 KB granule 下的地址位拆分:

```
  IPA (39 bit)
  ┌──────────────┬──────────────┬──────────────┬───────────────────────┐
  │  L1 index    │  L2 index    │  L3 index    │   page offset          │
  │  bits[38:30] │  bits[29:21] │  bits[20:12] │   bits[11:0]           │
  └──────────────┴──────────────┴──────────────┴───────────────────────┘
        │              │              │
        │              │              └─ 每个 L3 entry 覆盖 4 KB (1<<12)
        │              └──────────────── 每个 L2 entry 覆盖 2 MB (1<<21)
        └─────────────────────────────── 每个 L1 entry 覆盖 1 GB (1<<30)
```

本项目不建 L2/L3,直接在 L1 放 **block descriptor**。一个 L1 block descriptor
(bit[1:0]=`01`)的输出地址**只占 bits[47:30]**,低 30 位是 RES0:

```
  bit 63                         47        30 29        12 11    2 1 0
  ┌──────────────┬─────────────────────────┬───────────┬───────┬───┐
  │ ... attrs ...│  Output Address [47:30]  │   RES0    │ attrs │0 1│
  └──────────────┴─────────────────────────┴───────────┴───────┴───┘
                  ↑                          ↑
                  只有 bits[47:30] 是输出 PA  bits[29:0] 在 L1 block
                  → PA 低 30 位被硬件当作 0     里必须为 0 (= 1 GB 对齐)
```

**硬件规则:L1 block 只能指向 1 GB 对齐的 PA(低 30 位恒 0)。** 代码用
`& 0xFFFFC0000000` 模拟了这个行为(`0xFFFFC0000000` 即清掉低 30 位)。把
`0x48000000` 喂进去:

```
  0x48000000 = 0100 1000 0000 0000 ... 0000   ← bit31=1, bit27=1
                    │
                    └─ bit27 (0x08000000, 128MB) 落在"低 30 位"里!

  & 0xFFFFC0000000  (保留 bit30 及以上,清 bit29..0)
  ─────────────────────────────────────────────
  0x40000000 = 0100 0000 0000 0000 ... 0000   ← bit27 被吃掉了
                                                  → 正好是 hv 镜像位置
```

于是 guest 访问 IPA `0x40080000` 实际落到 PA `0x40000000 + 0x80000 = 0x40080000`
—— hypervisor 的 `head.S`,所以打印 `!EL`:

```
  [BUG] ram_pa=0x48000000          [FIX] ram_pa=0x80000000
        │ & 0xFFFFC0000000               │ & 0xFFFFC0000000
        ▼ (bit27 被吃)                   ▼ (低30位本就全0,不丢位)
  base = 0x40000000                 base = 0x80000000
        │ + 0x80000                      │ + 0x80000
        ▼                                ▼
  PA = 0x40080000                   PA = 0x80080000
        ▼                                ▼
  ┌─────────────────┐              ┌─────────────────┐
  │ hypervisor      │              │ Linux Image     │
  │ head.S → "!EL"  │ ✗            │ (QEMU loader)   │ ✓ 正常启动
  └─────────────────┘              └─────────────────┘
```

候选 PA 对齐性对照(选 `0x80000000` 的原因 —— 1 GB 对齐 + 不撞 hv):

```
  地址          低30位全0? 1GB对齐?  说明
  ──────────────────────────────────────────────────
  0x40000000      是        是      hv 镜像在这,冲突 ✗
  0x48000000      否        否      bit27=1,就是本 bug ✗
  0x80000000      是        是      2GB,本次选用 ✓
  0xC0000000      是        是      3GB,也可行 ✓
```

> 代价:`0x80000000` 在 QEMU `virt`(DRAM 从 `0x40000000` 起)只有给到 >1 GB
> 内存时才被背书 —— 这正是 **Bug 2**(§3)的由来。另一条不挪地址的路是把
> Stage-2 降到 **L2 2 MB block**(输出取 bits[47:21],只要 2 MB 对齐,
> `0x48000000` 就能直接映射),见 ADR-0004 的 Considered Options。

**修复(选择:保留单 1 GB block,把 RAM 挪到 1 GB 对齐 PA):**
`board.h` 把 `BOARD_LINUX_RAM_PA` 从 `0x48000000` 改成 `0x80000000`,
IMAGE_PA/DTB_PA/initrd 一起平移到 `0x80000000` 基址,`run-qemu.sh` 的 loader
地址同步改成 `0x80080000 / 0x82000000 / 0x84000000`。**guest IPA 布局不变**
(RAM `0x40000000`、DTB `0x42000000`),所以 DTS 不用改。

---

## 3. 第二次跑:取指外部中止 `EC=0x20 ELR=0x200`(Bug 2)

`make clean && make`(切记坑 B)后重跑。`!EL` 没了,banner 变成
`ram_pa=0x80000000`(修复生效),但出现真正的 guest 异常:

```
[hv] unexpected exit EC=0x20 ESR=0x8200000d ELR=0x200
```

**用 `-d int` 拿精确异常链** —— 这一步是关键,别靠猜:

```sh
... -d int -D /tmp/int.log ...
grep -E 'Taking exception|with ESR|with FAR|with ELR|return from' /tmp/int.log
```

```
Exception return from EL2 to EL1 PC 0x40080000   ← eret 进 guest,入口正确
Taking exception 3 [Prefetch Abort]
...from EL1 to EL1
...with ESR 0x86000010  (EC=0x21 取指中止 / IFSC 0x10 = 同步外部中止)
...with FAR 0x40080000  ← 取 guest 第一条指令就失败
...to EL1 PC 0x200       ← guest VBAR 还是 0,异常向量到 0x200
Taking exception 3 [Prefetch Abort]
...from EL1 to EL2
...with ESR 0x20/0x8200000d  ← 0x200 也取不到 → 翻译错误 → 进 EL2(就是看到的那个 exit)
```

**IFSC `0x10` = 同步外部中止,不是翻译错误。** 意思是:Stage-2 把 IPA
`0x40080000` 翻译出一个 PA,但**那个 PA 上没有内存响应**。

**验证 PA 是否在 DRAM 范围内:**

```sh
# QEMU virt 的 DRAM 从 0x40000000 起。-m 1G => 0x40000000..0x80000000(不含末尾)
python3 -c "print('0x80080000 在 1GB RAM 内?', 0x80080000 < 0x40000000+0x40000000)"  # False
```

**推论:Bug 1 的修复把 RAM 挪到 `0x80000000`(为了 1 GB 对齐),但 `-m 1G` 时
DRAM 正好到 `0x80000000` 为止 —— 新基址是物理内存的第一个字节之外,没内存。**
Image 被 loader 写进了"空洞",取指自然外部中止。

**修复:`run-qemu.sh` 改 `-m 2G`**(DRAM 覆盖到 `0xBFFFFFFF`,`0x80080000` 落在
内存里)。这也是 Bug 1 那个修复方案的代价:1 GB 对齐的 RAM 基址**强制**要 >1 GB
guest 内存(另一条路是 Stage-2 降到 L2 2 MB block,就能把 RAM 留在 `0x48000000`)。

跑通 → 一路 `Booting Linux` → `Run /init` → `=== M3.4: busybox rootfs up ===`
→ 出现 `~ #`。**到此 M3 的启动目标达成。**

---

## 4. 第三次:一敲键盘就崩 `!VEC`(Bug 3)

往 shell 喂一个字符:

```sh
( sleep 7; printf 'x'; sleep 2 ) | qemu ... -d int -D /tmp/int.log
```

```
~ # !VEC ESR=0x5a000000 ELR=0x40082fb0
```

**再用 `-d int` 看最后的异常**(`!VEC` 的 ESR 是 panic_vector 读到的陈旧值,别信它):

```
Taking exception 5 [IRQ]
...from EL2 to EL2          ← IRQ 在"正处于 EL2"时被取到!
...with ESR 0x15/0x56000000 (真正的 EC=0x15? 实为 IRQ class)
...with ELR 0x40082fb0      ← 当时 EL2 的 PC
...to EL2 PC 0x40080a80     ← 跳到了 hv_vectors+0x280
```

**定位 ELR 和向量:**

```sh
nm build/hypervisor.elf | sort | grep -iE '40082(e|f)|hv_vectors'
python3 -c "print('hv_vectors+0x280 =', hex(0x40080800+0x280))"  # 0x40080a80
```

`0x40082fb0` 落在 `el1_irq_handler_asm` 尾部;`0x40080a80` = `hv_vectors+0x280`
= "Current EL with SP_ELx / IRQ" 向量,而 `vectors.S` 里这条接的是 `panic_vector`。

**搞清是哪个中断在风暴 —— 加一行带标记的探针(用完即删):**

```c
// el2_irq_handler() 开头,记得 #include <printk.h>
printk("[DEBUG-irq3] el2_irq intid=%u\n", (unsigned)intid);
```

```
476 el2_irq intid=27   ← vtimer,一直在打
  1 el2_irq intid=33   ← PL011 RX(键盘),来一下就崩
```

**推论(根因):** `el1_irq_handler_asm` 和 `vcpu_run` 都在 `eret` **前一条指令**
做 `msr daifclr, #2`(开 EL2 IRQ)。看反汇编:

```sh
objdump -d build/hypervisor.elf --start-address=0x40082fa0 --stop-address=0x40082fc0
#   ...fac:  msr  daifclr, #0x2
#   ...fb0:  eret
```

在 `daifclr`→`eret` 这**一条指令的窗口**里,只要有挂起的物理 IRQ,就会**在 EL2
被取到**,而 EL2 没有 current-EL IRQ 处理(向量接 `panic_vector`)。vtimer 一直在
打,所以一旦键盘制造出第二个 IRQ(33)抬高竞争,这个窗口立刻被命中 → 崩。

**为什么 `daifclr` 是多余的?** `HCR_EL2.IMO=1` 时,guest 在 EL1 跑时来的物理
IRQ **本来就路由到 EL2**,且不受 guest 的 `PSTATE.I` 屏蔽(只受 EL2 自己的
`PSTATE.I` 门控,而那在 EL1 时不适用)。所以 `eret` 之后 CPU 在 EL1,挂起的 timer
IRQ 会立刻经 **Lower-EL IRQ 向量(+0x480)** 正常取到。`daifclr` 只是制造了一个
"在 EL2 取到 IRQ" 的有害窗口。

**修复:删掉 `vcpu_run`(`vmexit_asm.S`)和 `el1_irq_handler_asm`
(`irq_handler_asm.S`)里 `eret` 前的 `msr daifclr, #2`。**

验证:40 次连击 + 多命令,`grep -cE '!VEC|!EL'` = 0,shell 不崩了。

---

## 5. 收尾:让 shell 真的能输入

崩溃没了,但敲命令**没回显**。分析输入路径:

- guest 控制台是 `console=ttyAMA0`(PL011 直通),guest 自己读 PL011 DR。
- 但 hv 的 `vm_run` 循环每轮调 `virtio_console_rx_poll()` → `uart_getc()`,
  **把同一个 PL011 RX FIFO 的字节抢走丢弃**(guest 没走 hvc0)。
- 而且 PL011 的 SPI 33 **既没在物理 GICD 使能,也没注入给 guest**,guest 的
  ttyAMA0 中断处理永远不触发。

**逐项修(每改一处都用 §0 的反馈环验证):**

1. `vm.c`:从 `vm_run` 循环里删掉 `virtio_console_rx_poll()`(它只服务
   `console=hvc0` 的 guest,否则纯属抢输入)。
2. `gic_v3.c` + `board.h` + `gic_v3.h`:在物理 GICD 里使能 PL011 `SPI 33`
   (Group 1、prio 0xA0、路由 CPU0、enable)。
3. `irq_handler.c`:`intid==33` 时注入 guest vGIC,只 priority-drop(留 Active)。
4. `vgic.c`:`vgic_inject_spi` 改成 **HW-forward 注入到 LR1**。

**为什么是 LR1 + HW-forward?**(这步踩了两次坑,值得记)

- **先用 SW 注入到 LR0** → 没回显。因为 vtimer 每拍都把 LR0 重写一遍,SW 注入的
  PL011 vIRQ 还没被 guest 取走就被覆盖了。**改注入到 LR1**(`vgic_restore` 已经
  会存取 LR1)。
- **再用 SW 注入 LR1 + drop&deactivate** → 探针显示 SPI 33 风暴 **48 万次**:
  物理线 level-sensitive,guest 还没读 DR 我就 deactivate,立刻 re-pend。
- **改 priority-drop only(留 Active)** → 第一条命令能跑,但之后不行:没有
  HW-forward 的 LR 链接,物理 SPI 一直 Active,后续中断进不来。
- **最终:HW-forward(LR.HW=1,带物理 INTID)注入到 LR1** → guest deactivate
  虚拟 IRQ 时,经 LR 链接自动释放物理线。完美。

**最终实测(提交后的 `scripts/run-qemu.sh`,即 `make run` 路径):**

```
~ # ls /
bin   dev   etc   init  proc  root  sbin  sys
~ # echo HELLO_FROM_GUEST
HELLO_FROM_GUEST
~ # uname -m
aarch64
~ # cat /proc/uptime
14.58 12.21
```

---

## 6. 复现清单(照着跑就能从零到可交互 shell)

```sh
source /home/corsair/Downloads/toolchain.sh

# 1) 瘦身内核(<31 MB)+ 静态 busybox initramfs —— 见 §1
# 2) 构建 hv(改过头文件就先 make clean)
make clean && make && make guest

# 3) 跑(注意 -m 2G、loader 地址在 0x80000000 基址)
LINUX_IMAGE=~/Music/virtual/linux-6.12.93/arch/arm64/boot/Image \
LINUX_INITRD=~/Music/virtual/initramfs.cpio.gz \
make run
# 在 ~ # 提示符敲 ls / uname -m 等,应有输出且不崩
```

**定位手法小结(可迁移到别的 bug):**

1. **先问"这串输出/这个字节是谁打印的"** —— `grep` 源码定位 emitter,往往一步
   缩小到具体函数。
2. **`-d int` 看异常链** —— EL 转换 + ESR/FAR/ELR 是 ARM 上最硬的证据,胜过读代码
   猜。解 ESR:`EC=ESR>>26`,常见 `0x20/0x21` 取指中止、`0x24` 数据中止、
   `0x16` HVC、IRQ 单独的 exception 编号。
3. **`-d in_asm` 确认"在执行谁的代码"** —— 把 trace PC 跟 `nm`/`objdump` 的符号
   对齐;注意 guest IPA、Image 文件偏移、vmlinux VA 三者的换算别搞混。
4. **带标记探针 `[DEBUG-xxxx]`** —— 一行 `printk` 常比读十遍代码快;用完
   `grep` 一把删干净。
5. **一次只改一个变量**,每改一处用同一个反馈环验证,别堆改动。

## 7. 关联文档 / 待办

- 结论与改动清单:[[m3-boot-verification]](m3-boot-verification.md)
- ADR-0004 里"`0x48000000` 是 1 GB 对齐"是错的,且没记录"必须 `-m 2G`",应更新。
- CLAUDE.md 里 M3 各子里程碑标 "done" 属实,但都是静态验证;本次才是首次实测。
