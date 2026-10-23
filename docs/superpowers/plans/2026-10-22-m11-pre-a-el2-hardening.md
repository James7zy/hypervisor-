# M11 前置 A — EL2 加固 Implementation Plan

> **Status:** In progress. **时间盒:** 2 周(ADR-0017 2026-10-22 修订),到期即止,
> 未完成部分记已知缺口,强制进入 M11 前置 B,不顺延。

**Goal:** EL2 自己开 Stage-1 MMU 和 cache(hypervisor 内存从 Device 变 Normal),并加上
`HCR_EL2`/`MDCR_EL2` 的陷入集合(`TSC`/`TWI`/`TWE`/ID 寄存器过滤/PMU-debug)。
Full design: `docs/superpowers/specs/2026-10-22-m11-pre-a-el2-hardening-design.md`。

**Observable DoD:** 三个切片各自的 DoD 见下;整体 DoD 是现有两个 Linux VM 的所有已知
行为(`make test` 全绿 + 双 VM 到 `~ #` + Ctrl-T 切换 + PSCI poweroff 隔离)保持不变,
外加切片 3 新增的 `smc` 陷入行为可观察。

**分支:** 一个切片一个分支,gate 过了就合并:
`m11-pre-a-slice1-identity-mmu` → `m11-pre-a-slice2-cache` →
`m11-pre-a-slice3-hcr-traps`。

**Header 依赖提醒:** Makefile 不跟踪头文件依赖,改了头文件后 `make clean`。

---

## Slice 1 — EL2 Stage-1 MMU 身份映射,不开 cache (`m11-pre-a-slice1-identity-mmu`)

> 纯新增,不改变任何可观察行为。失败模式是启动即挂(页表错误),不是潜伏数据损坏。

### Task 1.1 — `el2_mmu.h`:常量与接口

**Files:** Create `hypervisor/arch/arm64/mmu/el2_mmu.h`

- [ ] 声明两个函数,风格对齐 `stage2.h`:

```c
#ifndef HV_ARCH_ARM64_EL2_MMU_H
#define HV_ARCH_ARM64_EL2_MMU_H

/* Build the shared identity-map L1 table. Boot CPU only, once. */
void el2_mmu_init(void);

/* Program this PE's MAIR/TCR/TTBR0/SCTLR and enable the MMU (M=1, C=0, I=0
 * in slice 1). Every pCPU calls this once, after el2_mmu_init() has run. */
void el2_mmu_enable(void);

#endif
```

### Task 1.2 — `el2_mmu.c`:身份映射表 + 寄存器编程

**Files:** Create `hypervisor/arch/arm64/mmu/el2_mmu.c`

- [ ] 寄存器取值常量(数值来源见 spec "Implementation Decisions / 切片 1"——已对照
  bao-hypervisor 与 xvisor 两份独立实现交叉核对,**不要**从 `stage2.c` 的
  `VTCR_EL2_VALUE` 直接照抄 RES1,`TCR_EL2` 比 `VTCR_EL2` 多一个 bit23 的 RES1):

```c
/* MAIR_EL2: index 0 = Device-nGnRE, index 1 = Normal WB-RA-WA (inner+outer). */
#define MAIR_ATTR_DEV_nGnRE   0x04ULL
#define MAIR_ATTR_NORMAL_WB   0xFFULL
#define MAIR_EL2_VALUE        (MAIR_ATTR_DEV_nGnRE | (MAIR_ATTR_NORMAL_WB << 8))

/* TCR_EL2 (non-VHE / E2H=0 format). RES1 at bit23 AND bit31 — NOT the same
 * RES1 as VTCR_EL2 (bit31 only). Cross-checked against bao sysregs.h
 * TCR_RES1 = (1<<23)|(1<<31) and xvisor TCR_INITVAL = 0x80800000. */
#define TCR_RES1        ((1ULL << 23) | (1ULL << 31))
#define TCR_T0SZ        (25ULL << 0)    /* 39-bit input address, matches stage2.c */
#define TCR_IRGN0_WBWA  (1ULL  << 8)
#define TCR_ORGN0_WBWA  (1ULL  << 10)
#define TCR_SH0_ISH     (3ULL  << 12)
#define TCR_TG0_4K      (0ULL  << 14)
#define TCR_PS_40BIT    (2ULL  << 16)
#define TCR_EL2_VALUE \
    (TCR_RES1 | TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_ISH | \
     TCR_TG0_4K | TCR_PS_40BIT)

/* SCTLR_EL2 bits this slice touches. */
#define SCTLR_EL2_M     (1ULL << 0)

/* Stage-1 block descriptor fields (EL2 regime: single AP bit, meaningful
 * only when set to RO — leave AP unset for RW, per bao PTE_AP_RW_PRIV == 0). */
#define S1_BLOCK        0x1ULL
#define S1_ATTRIDX(i)   ((u64)(i) << 2)
#define S1_AF           (1ULL << 10)
#define S1_SH_ISH       (3ULL << 8)
#define S1_SH_OSH       (2ULL << 8)
#define S1_XN           (1ULL << 54)
```

- [ ] 静态表 + 两个函数:

```c
static u64 l1_table[512] __attribute__((aligned(4096)));

void el2_mmu_init(void)
{
    /* L1[0]: 0x00000000-0x3FFFFFFF, Device-nGnRE, XN, identity. Covers
     * BOARD_GIC_DIST_BASE/BOARD_GIC_RDIST_BASE/BOARD_UART_BASE. */
    l1_table[0] = 0x00000000ULL | S1_BLOCK | S1_ATTRIDX(0) | S1_AF |
                  S1_SH_OSH | S1_XN;

    /* L1[1]: 0x40000000-0x7FFFFFFF, Normal WB, executable, identity. Covers
     * the hv image (loaded at 0x40080000); nothing else lives in this 1 GB
     * block (board.h: guest RAM starts at BOARD_LINUX_RAM_PA=0x80000000). */
    l1_table[1] = 0x40000000ULL | S1_BLOCK | S1_ATTRIDX(1) | S1_AF |
                  S1_SH_ISH;
}

void el2_mmu_enable(void)
{
    __asm__ volatile(
        "msr mair_el2, %0\n"
        "msr tcr_el2,  %1\n"
        "msr ttbr0_el2, %2\n"
        "isb\n"
        "tlbi alle2\n"
        "dsb nsh\n"
        "isb\n"
        :
        : "r"((u64)MAIR_EL2_VALUE), "r"((u64)TCR_EL2_VALUE),
          "r"((u64)(uintptr_t)l1_table)
        : "memory"
    );

    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    sctlr |= SCTLR_EL2_M;
    __asm__ volatile(
        "msr sctlr_el2, %0\n"
        "isb\n"
        :: "r"(sctlr) : "memory"
    );
}
```

  （`uintptr_t` 需要 `#include <types.h>` 已提供的定义，确认本仓库 `types.h` 有
  `uintptr_t`——`stage2.c` 已经在用 `(u64)(uintptr_t)l1` 这个写法，照抄即可。）

### Task 1.3 — 调用点

**Files:** Modify `hypervisor/arch/arm64/cpu/cpu.c`,
`hypervisor/arch/arm64/cpu/secondary.c`, `Makefile`(加 `el2_mmu.o` 到对象列表）

- [ ] `cpu.c` `cpu_arch_init()` 顶部,在任何其它初始化之前:

```c
void cpu_arch_init(void)
{
    el2_mmu_init();
    el2_mmu_enable();

    printk("[hv] Hello from EL2 on %s, CurrentEL=0x%lx\n", board_name,
           SYSREG_READ(CurrentEL));
    ...
```

  `#include "../mmu/el2_mmu.h"`。
- [ ] `secondary.c` `secondary_main()` 顶部,在触碰 GICR MMIO 之前:

```c
void secondary_main(u32 id)
{
    el2_mmu_enable();   /* table already built by the boot CPU */

    const unsigned long rd = ...
```

  `#include "../mmu/el2_mmu.h"`。
- [ ] `Makefile`:把 `hypervisor/arch/arm64/mmu/el2_mmu.o` 加进 arm64 的对象列表
  （紧挨 `stage2.o` 那一行）。

### Task 1.4 — Slice-1 gate

- [ ] `make clean && make` 零警告。
- [ ] `make test` → 全绿(basic/vtimer/dual-VM/shell/vSPI + 两个 offset 检查)。
- [ ] 手动:`LINUX_IMAGE=… LINUX_INITRD=… make run` → 两个 VM 都到 `~ #`；
  `/sys/devices/system/cpu/online` 两边 `0-1`；Ctrl-T 切换正常；`poweroff` 隔离测试
  （M10 已知场景）不回归。
- [ ] 一次性故障注入(spec Testing Decisions seam 三):临时把 `l1_table[1]` 的输出
  地址减一页，`make run`，确认挂死在预期位置而不是产生更诡异的行为；验证完撤销，
  **不提交**这个改动。
- [ ] 评估 `-mstrict-align` 是否还需要保留(本切片 `SCTLR_EL2.A` 仍为 0，不做硬件对齐
  检查；如果决定暂时保留，在 commit message 或 spec 里记一句原因，不留空白结论）。
- [ ] Commit: `feat(m11-pre-a-slice1): EL2 Stage-1 MMU identity map, caches off`

---

## Slice 2 — 开 cache (`m11-pre-a-slice2-cache`)

> 前置条件:Slice 1 已合并且 gate 通过。

### Task 2.1 — `el2_mmu_enable()` 加 cache 使能

**Files:** Modify `hypervisor/arch/arm64/mmu/el2_mmu.c`

- [ ] `SCTLR_EL2` 加 `C`(bit2)、`I`(bit12):

```c
#define SCTLR_EL2_C     (1ULL << 2)
#define SCTLR_EL2_I     (1ULL << 12)
```

- [ ] 使能序列在原有 `tlbi alle2` 前加 `ic iallu`(指令缓存失效，参照 xvisor
  `proc_setup` 的 `ic iallu` / `tlbi alle2is` / `dsb sy` 顺序——两份参考实现都有这一步，
  不是单一来源的习惯）：

```c
    __asm__ volatile(
        "ic iallu\n"
        "msr mair_el2, %0\n"
        ...
        "tlbi alle2\n"
        "dsb nsh\n"
        "isb\n"
        ...
    );

    sctlr |= SCTLR_EL2_M | SCTLR_EL2_C | SCTLR_EL2_I;
```

  （`ic iallu` 要在读写 `sctlr_el2` 之前执行一次，确保开 I-cache 之前已失效的指令
  缓存里没有残留的、MMU 关闭状态下取到的指令行。）

### Task 2.2 — Slice-2 gate

- [ ] `make clean && make` 零警告；`make test` 全绿。
- [ ] 手动 Linux 双 VM 跑通，同 Slice 1 的全部检查项。
- [ ] Commit: `feat(m11-pre-a-slice2): enable EL2 Stage-1 data+instruction cache`

---

## Slice 3 — `HCR_EL2`/`MDCR_EL2` 陷入集合 (`m11-pre-a-slice3-hcr-traps`)

> 前置条件:Slice 2 已合并且 gate 通过。这一片新增可观察行为（`smc` 的返回值），
> 需要新测试用例，不是纯重构。

### Task 3.1 — `vm.c`:`hcr_el2` 初值追加陷入位

**Files:** Modify `hypervisor/arch/arm64/guest/vm.c`

- [ ] `vcpu_arch_reset()` 的 `hcr_el2` 赋值追加 `TSC`(bit19)、`TWI`(bit13)、
  `TWE`(bit14)、`TID3`(bit18):

```c
    v->arch.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                      (1ULL << 13) | (1ULL << 14) | (1ULL << 18) |
                      (1ULL << 19) | (1ULL << 31);
    /* TWI(13)/TWE(14): trap guest wfi/wfe — handler passes through (no
     * scheduler yet). TID3(18): trap ID_AA64*_EL1 reads — handler forwards
     * the real value. TSC(19): trap guest smc — handler returns
     * SMCCC_NOT_SUPPORTED instead of letting it reach EL3 (ADR-0017). */
```

### Task 3.2 — `vmexit.c`:新增 `smc`/`wfx`/ID 寄存器陷入处理

**Files:** Modify `hypervisor/arch/arm64/vmexit/vmexit.c`

- [ ] 新增 `handle_smc()`(EC=0x17),行为同 `handle_hvc` 的 `default` 分支：

```c
static void handle_smc(struct arch_regs *regs)
{
    printk("[hv] SMC trapped (func_id=0x%lx), returning NOT_SUPPORTED\n",
           regs->x[0]);
    regs->x[0] = SMCCC_NOT_SUPPORTED;
    regs->elr_el2 += 4ULL;   /* smc has no "advance past" semantics in ESR;
                                unlike hvc, ELR already points past it only
                                for sync exceptions where FAR/ELR convention
                                matches hvc — confirm against ESR_EL2.IL
                                before assuming +4 during implementation. */
}
```

  **实现时核对**:`smc` 陷入后 `ELR_EL2` 是否已经指向下一条指令（和 `hvc` 一致）还是
  指向 `smc` 本身——如果和 `hvc` 不同，修正这里的 `elr_el2` 调整逻辑，不要照抄假设。
- [ ] 新增 `handle_wfx()`(EC=0x01)，原样放行：

```c
static void handle_wfx(struct arch_regs *regs)
{
    regs->elr_el2 += 4ULL;   /* no scheduler yet: treat wfi/wfe as a no-op */
}
```

- [ ] `handle_sysreg_trap()` 加 ID 寄存器分支：`CRn==0`（`ID_AA64*_EL1` 组的编码
  特征，实现时查 ARM ARM 确认 `CRn` 具体值，不要猜）→ 读物理寄存器原样转发给
  `regs->x[rt]`（只读访问，`wr` 应为 false，若 guest 尝试写要怎么处理——这些寄存器
  架构上是只读的，guest 写入是违规访问，打印一行警告 + 不修改任何状态即可，不需要
  模拟异常注入）。
- [ ] 顶层 vmexit 分发（查找 `handle_hvc`/`handle_sysreg_trap` 的调用处，通常是
  `switch (ec)` 结构）加 `case 0x17:` → `handle_smc`，`case 0x01:` → `handle_wfx`。

### Task 3.3 — `cpu.c`:`MDCR_EL2` 设置

**Files:** Modify `hypervisor/arch/arm64/cpu/cpu.c`

- [ ] `cpu_arch_init()`（EL2 MMU 使能之后，GIC/vtimer 初始化之前或之后均可，和内存
  类型无关）加：

```c
    SYSREG_WRITE(MDCR_EL2, (1ULL << 8) | (1ULL << 6) | (1ULL << 5));
    /* TDE(8): trap debug exceptions to EL2. TPM(6)/TPMCR(5): trap PMU
     * register access to EL2. No virtual debug/PMU support yet — traps are
     * caught by the default "unknown trap" path and logged. */
```

  需要确认这个陷入落到哪个 EC（PMU/debug 寄存器访问通常也是 EC=0x18 的 trapped
  MSR/MRS，和已有的 `handle_sysreg_trap` 同一入口——实现时核对，如果走的是专门的
  debug EC，要在 vmexit 分发里单独加一个 `case`，不要假设它自动落进现有分支）。

### Task 3.4 — 新测试用例:`smc` 陷入

**Files:** Modify `tests/svm/svm_main.c`（或新建 `tests/svm_smc/`，视当时
`svm_main.c` 结构决定——如果它已经是多场景分支结构，加一个分支比新建一整套
`.lds`/Makefile 规则更小）；`tests/run_svm_test.sh`（或对应新用例的脚本）

- [ ] SVM guest 发一条 `smc #0`（`func_id` 随便取一个不会被误认成真实固件服务的值，
  比如 `0x8300FFFF`），把返回的 `x0` 打印出来（复用现有的 `HC_GUEST_DONE` 式
  banner 打印模式）。
- [ ] 期望脚本断言打印的 `x0` 等于 `SMCCC_NOT_SUPPORTED`（`0xFFFFFFFF`，
  `hypercall.h` 已定义这个常量，`handle_hvc` 已经在用）。
- [ ] 把这个新场景接进 `make test`（照抄 `test-qemu-vspi` 之类目标的模式，加一个
  `test-qemu-smc` 目标，追加到 `test:` 的依赖列表）。

### Task 3.5 — Slice-3 gate(= 本阶段整体 gate)

- [ ] `make clean && make` 零警告；`make test` 全绿，含新的 `smc` 场景。
- [ ] 手动 Linux 双 VM 跑通，同前两片的全部检查项（这一片不该影响 Linux guest 的
  `smc` 使用——确认 Linux boot 过程中不依赖 `smc` 到达真实固件的路径，如果依赖，
  这是一个需要在 commit message 里记录的行为变化，不能悄悄放过）。
- [ ] Commit: `feat(m11-pre-a-slice3): HCR_EL2 trap set (TSC/TWI/TWE/TID3) + MDCR_EL2`

---

## 时间盒到期处理

- 若 3 周（ADR-0017 给前置 A+B 的总时间盒是 3 周，前置 A 单独是 2 周）内只做完部分
  切片：未完成的切片记入 CLAUDE.md 和 ADR-0017 的"已知缺口"列表，**不延长**，直接
  进入 M11 前置 B（PSCI/SMCCC 补全）。
- 到期状态更新清单：
  - [ ] `CLAUDE.md` 里程碑表:M11 前置 A 行从 "next" 改成 "done (日期)" 或
    "part-done (已知缺口: …)"。
  - [ ] 本 plan 文件头部 Status 行更新为 Executed/Partially executed，附实际完成的
    切片和提交号。
  - [ ] Spec 文件状态行同步。
