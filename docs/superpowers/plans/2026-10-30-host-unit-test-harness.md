# Host 单测 harness 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 `make host-test`：host gcc 秒级编译并运行同一份 hypervisor 纯逻辑源文件的单测（首批 lib/string.c + common/io/mmio.c），普通 shell 可跑、全绿、FAIL 路径实证可用。

**Architecture:** 无注册表 runner（main 显式调用 test_*）+ 共享头 host_test.h（TEST_ASSERT 宏与计数器契约）；Makefile 增量集成（goal filter 放行、独立 HOST_TEST_CFLAGS、产物进 `$(BUILD_DIR)/host/`），不触碰固件链接输入。

**Tech Stack:** C11、host gcc（`cc`）、GNU make。零第三方依赖。

**Spec:** `docs/superpowers/specs/2026-10-30-host-unit-test-harness-design.md`（关卡①已批准，含架构师裁决记录）

## Global Constraints

- host 编译 flag 集（实证钉死，整体照抄，不逐项重议）：`-Wall -Wextra -Werror -O2 -g -fno-builtin -U_FORTIFY_SOURCE -std=c11`（另加 `-Ihypervisor/include`）。
- 复用 Makefile:114 `HOST_CC := cc`；**不引入** `HOSTCC ?=`。
- `host-test` **必须**加进 Makefile:27 goal filter，否则普通 shell 解析期即 $(error)。
- 产物在 `$(BUILD_DIR)/host/`；`clean` 已覆盖 `$(BUILD_DIR)`，不改 clean。
- **不并入** `make test` 聚合链（gate 语义不变，ADR-0009 v1 裁决）。
- 无注册表：main 显式调用每个 `test_*`。
- 被测模块必须 arch-neutral（无 ARM64 内联汇编、`arch/` 头、percpu/spinlock/printk）——本切片两模块已满足，不改产品代码。
- **git 历史全程绿**：本切片没有"新实现"可被失败测试驱动，RED 证据 = 工作树内故意破坏断言 → 观察 `FAIL file:line` + 非零退出 → 还原（不落 commit）。"RED commit → GREEN commit"纪律从 M11 新模块起适用。
- 新文档中文（标识符/技术术语英文）；注释解释代码本身不能表达的原因。
- `hv-objs` 显式清单不 glob、固件链接输入不变（DoD 要求 bit 级不变，Task 3 验证）。

## Review Focus

| # | 失败模式（spec 隐含、无任务测试覆盖时最可能咬人） | 钉住它的测试/步骤 |
|---|---|---|
| 1 | host gcc -O2 把 memset 循环优化成自调用（缺 `-fno-builtin`）或 fortify 重定向到 glibc（缺 `-U_FORTIFY_SOURCE`）→ 假绿 | string_test 哨兵断言（Task 1）+ flag 集逐字照抄 |
| 2 | goal filter 漏加 → 无 .config/无交叉链的 shell 里 `make host-test` 解析期报错 | Task 1 步骤 7：`env -i` 裸环境运行 |
| 3 | lookup 边界 off-by-one：`addr == base+len` 误命中（区间应为 `[base, base+len)`） | mmio_test 边界断言组（Task 2） |
| 4 | handler 返回非 0 被 dispatch 吞成 0 | mmio_test handler 失败传播断言（Task 2） |
| 5 | 总线满后第 9 次 register 静默覆盖已有区域 | mmio_test 上限断言组（Task 2） |

---

### Task 1: Harness 骨架 + string 哨兵 + Makefile 集成

**Files:**
- Create: `tests/host/host_test.h`、`tests/host/runner.c`、`tests/host/string_test.c`
- Modify: `Makefile`（四处：goal filter 27、.PHONY 116-119、host-test 规则块插在 check-offsets-target 规则之后、help 307）
- 本任务不动 mmio（Task 2）。

**Interfaces:**
- Produces: `TEST_ASSERT(cond)` 宏（打印 `FAIL file:line: expr`，计数）；全局计数器 `tests_run`/`tests_failed`（runner.c 定义、头文件 extern）；`void test_string(void)`；make 目标 `host-test`。
- 后续任务依赖：host_test.h 是唯一断言契约入口；HOST_TEST_SRCS 是被测源清单（Task 2 向其追加）。

- [ ] **Step 1: 在 develop 上（未分支前）固化固件基线镜像**

```bash
cd /home/corsair/Music/virtual/hyp-/develop
PATH=$HOME/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH \
  make BUILD_DIR=build/pre-harness all
sha256sum build/pre-harness/hypervisor.elf   # 记录哈希，Task 3 Step 1 要对比
```

Expected: 构建零警告成功；记下 sha256。

- [ ] **Step 2: 建 feature 分支**

```bash
git checkout -b feature/host-unit-test-harness
```

（在 develop worktree 内切分支；合并时 `--no-ff` 回 develop，协议固定。）

- [ ] **Step 3: 写 `tests/host/host_test.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_HOST_TEST_H
#define HV_HOST_TEST_H

#include <stdio.h>

/* 计数器定义在 runner.c。纯文本输出，不做机器可解析格式（ADR-0009）。 */
extern int tests_run;
extern int tests_failed;

#define TEST_ASSERT(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

void test_string(void);

#endif /* HV_HOST_TEST_H */
```

（spec 文件树外的第 4 个文件：宏必须同时被 runner 与各测试文件可见，三个 .c 无法不重复地表达；关卡①裁决点。）

- [ ] **Step 4: 写 `tests/host/runner.c` 与 `tests/host/string_test.c`**

runner.c：定义两个计数器；`main(void)` 依次调用 `test_string()`；结尾无失败打印 `PASS (N assertions)` 返回 0，有失败打印 `FAIL (M/N assertions failed)` 返回 1。

string_test.c 断言组（哨兵值用 0x55 预填、0xAA 填充；聚合循环比较，避免 32 条同型断言刷屏）：

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include "host_test.h"

void *memset(void *dst, int c, size_t n);

void test_string(void)
{
    unsigned char buf[32];
    int ok = 1;

    for (int i = 0; i < 32; i++)
        buf[i] = 0x55;

    /* n=0：返回值==dst，且一个字节都不写 */
    TEST_ASSERT(memset(buf, 0xAA, 0) == buf);
    for (int i = 0; i < 32; i++)
        ok = ok && (buf[i] == 0x55);
    TEST_ASSERT(ok);

    /* n=16：前 16 字节填充，后 16 字节不碰 */
    TEST_ASSERT(memset(buf, 0xAA, 16) == buf);
    ok = 1;
    for (int i = 0; i < 16; i++)
        ok = ok && (buf[i] == 0xAA);
    TEST_ASSERT(ok);
    ok = 1;
    for (int i = 16; i < 32; i++)
        ok = ok && (buf[i] == 0x55);
    TEST_ASSERT(ok);

    /* 非零起点：只写 [buf+8, buf+16)，两侧不碰（buf[16] 属于后半哨兵区） */
    memset(buf + 8, 0x77, 8);
    TEST_ASSERT(buf[7] == 0xAA && buf[8] == 0x77 && buf[15] == 0x77 && buf[16] == 0x55);
}
```

注意：不 include `<string.h>`（避免 glibc 原型/宏干扰，被测 memset 由上面手写原型声明）。

- [ ] **Step 5: Makefile 四处修改**

1. 27 行 goal filter 名单追加 `host-test`：
   `ifeq ($(filter %_defconfig defconfig clean help menuconfig host-test,$(MAKECMDGOALS)),)`
2. .PHONY 名单（116-119）`check-offsets` 后加 `host-test`。
3. check-offsets-target 规则之后插入（注释只写 flag 的非显然原因，防止未来被"清理"掉）：

```make
# Host unit tests (ADR-0009, 2026-10-30 revision): second-level feedback for
# arch-neutral logic; NOT part of `make test` (that stays the QEMU gate).
# -fno-builtin: host gcc -O2 turns string.c's memset loop into a call to
# itself without it. -U_FORTIFY_SOURCE: Ubuntu default fortify would redirect
# the tests' memset calls to glibc __memset_chk (fake green).
HOST_TEST_CFLAGS := -Wall -Wextra -Werror -O2 -g -fno-builtin -U_FORTIFY_SOURCE \
    -std=c11 -Ihypervisor/include
HOST_TEST_SRCS := tests/host/runner.c tests/host/string_test.c \
    hypervisor/lib/string.c

$(BUILD_DIR)/host/host_test: $(HOST_TEST_SRCS) tests/host/host_test.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -o $@ $(HOST_TEST_SRCS)

host-test: $(BUILD_DIR)/host/host_test
	$(BUILD_DIR)/host/host_test
```

4. help 307 行 Targets 加 `host-test`：`Targets: all run clean test host-test <board>_defconfig defconfig`

- [ ] **Step 6: 跑绿**

Run: `make host-test`
Expected: 编译零警告；输出 `PASS (6 assertions)`（6 = Task 1 的 TEST_ASSERT 总数）+ 退出码 0。

- [ ] **Step 7: 裸环境验证（DoD：普通 shell、无交叉链、无 .config 依赖）**

Run: `env -i PATH=/usr/bin:/bin make host-test`
Expected: 同样 PASS。（证明 goal filter 放行后不碰 .config/交叉链路径。）

- [ ] **Step 8: RED 证据（工作树内验收操作，不留 commit）**

把 string_test.c 某断言故意改错（如 `buf[7] == 0xAB`）→ `make host-test` → 预期：`FAIL tests/host/string_test.c:<行>: ...` 行 + `make` 非零退出。观察后 **git checkout 还原**，重跑确认回绿。

- [ ] **Step 9: Commit**

```bash
git add tests/host/ Makefile
git commit -m "test: host unit-test harness skeleton with memset sentinel"
```

### Task 2: mmio 总线单测

**Files:**
- Create: `tests/host/mmio_test.c`
- Modify: `tests/host/host_test.h`（+`void test_mmio(void);` 原型）、`tests/host/runner.c`（main 追加调用）、`Makefile`（HOST_TEST_SRCS 追加 `tests/host/mmio_test.c hypervisor/common/io/mmio.c`）

**Interfaces:**
- Consumes: Task 1 的 `TEST_ASSERT`/计数器/host-test 目标；`<mmio.h>` 的 `mmio_bus_register`/`mmio_bus_dispatch`/`struct mmio_access`/`mmio_handler_t`。
- Produces: `void test_mmio(void)`。

约束：mmio.c 的 static 总线**无 reset 接口**（spec 已记录的已知风险）——mmio_test 假定自己是进程内第一个也是唯一动总线的测试：开头"空总线 dispatch"断言依赖 count==0；结尾把 8 槽用满。测试函数内一次跑完，不假设中间可重入。

- [ ] **Step 1: 写 `tests/host/mmio_test.c`**

捕获型 handler + 断言组：

```c
/* SPDX-License-Identifier: TBD */
#include <mmio.h>
#include "host_test.h"

struct capture {
    struct mmio_access acc;
    int calls;
    int ret;
};

static int capture_handler(struct mmio_access *acc, void *ctx)
{
    struct capture *c = ctx;
    c->acc = *acc;
    c->calls++;
    return c->ret;
}

void test_mmio(void)
{
    struct capture cap = { .ret = 0 };
    struct mmio_access a;

    /* 空总线：任何地址都无人接 */
    a.addr = 0x1000; a.size = 4; a.is_write = false; a.data = 0;
    TEST_ASSERT(mmio_bus_dispatch(&a) == -1);

    /* 注册 [0x1000, 0x1100) */
    TEST_ASSERT(mmio_bus_register(0x1000, 0x100, capture_handler, &cap) == 0);

    /* 命中：offset/data 原样到达 handler */
    a.addr = 0x1080; a.size = 4; a.is_write = true; a.data = 0xDEADBEEF;
    TEST_ASSERT(mmio_bus_dispatch(&a) == 0);
    TEST_ASSERT(cap.calls == 1);
    TEST_ASSERT(cap.acc.offset == 0x80);
    TEST_ASSERT(cap.acc.size == 4 && cap.acc.is_write && cap.acc.data == 0xDEADBEEF);

    /* 边界：[base, base+len) 半开区间 */
    a.addr = 0x1000;
    TEST_ASSERT(mmio_bus_dispatch(&a) == 0 && cap.acc.offset == 0);
    a.addr = 0x10FF;
    TEST_ASSERT(mmio_bus_dispatch(&a) == 0 && cap.acc.offset == 0xFF);
    a.addr = 0x1100;                    /* base+len：不属区间 */
    TEST_ASSERT(mmio_bus_dispatch(&a) == -1);
    a.addr = 0x0FFF;                    /* base-1 */
    TEST_ASSERT(mmio_bus_dispatch(&a) == -1);

    /* handler 失败必须传播为 -1 */
    cap.ret = 1;
    a.addr = 0x1000;
    TEST_ASSERT(mmio_bus_dispatch(&a) == -1);

    /* 上限：8 槽已用 1，再注册 7 个成功、第 9 个 -1（不静默覆盖） */
    for (int i = 0; i < 7; i++)
        TEST_ASSERT(mmio_bus_register(0x2000 + 0x100 * i, 0x100, capture_handler, &cap) == 0);
    TEST_ASSERT(mmio_bus_register(0x8000, 0x100, capture_handler, &cap) == -1);
}
```

注意 `cap.ret = 1` 之后不再改回：本测试到此结束（总线已满），无后续用例需要成功路径。

- [ ] **Step 2: 接线（host_test.h 原型 + runner 调用 + Makefile HOST_TEST_SRCS 追加）**

- [ ] **Step 3: 跑绿**

Run: `make host-test`
Expected: `PASS (25 assertions)`（6 + Task 2 新增 19：固定断言 11 + 注册循环 7 + 上限 1）。**若 FAIL：这是发现真 bug，停下报告架构师，不许为绿改产品代码**（本任务是特性测试不是驱动新实现）。

- [ ] **Step 4: Commit**

```bash
git add tests/host/ Makefile
git commit -m "test: mmio bus unit tests (register limit, lookup bounds, dispatch offset)"
```

### Task 3: 验收收尾（固件 bit 级不变 + 文档）

**Files:**
- Modify: `CLAUDE.md`（验证一节）、`docs/adr/0009-no-ci-manual-verification.md`（修订记录，仿 0015 头部日期注记惯例）、本 plan 状态行。

**Interfaces:** 无代码接口；DoD 的三项收尾。

- [ ] **Step 1: 固件 bit 级不变**

```bash
PATH=$HOME/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH \
  make BUILD_DIR=build/post-harness all
sha256sum build/pre-harness/hypervisor.elf build/post-harness/hypervisor.elf
```

Expected: 两个哈希**逐字节相同**（链接输入未变：hv-objs 显式清单、交叉 CFLAGS 未动）。不同 → 停下查因，不许跳过。

- [ ] **Step 2: CLAUDE.md 验证一节**

CLAUDE.md 现无独立验证小节；新增一节（置于 "## Project" 之后、"## Where knowledge lives" 之前）：

```markdown
## 验证

- **硬件验收 gate**：`make test`（3 项静态检查 + 7 个 QEMU 场景，ADR-0009；分钟级）。
- **纯逻辑改动**：`make host-test`（host gcc 直接编译同一份源文件，秒级；tests/host/，
  ADR-0009 2026-10-30 修订）。不并入 `make test`，gate 语义不变。
```

- [ ] **Step 3: ADR-0009 修订记录**

头部 Status 引用块内追加日期注记（仿 0015 惯例）：

```markdown
> **2026-10-30:** host 单测层启用：`make host-test` 用 host gcc 编译同一份
> arch-neutral 源文件跑纯逻辑单测（tests/host/，零依赖）。本 ADR 的三步 gate
> 语义不变，仍是强制验收；host 层是它的下一级快速反馈，不是替代。
> "Considered Options" 里"值得在有可移植逻辑后重访"的条件自此兑现。
```

- [ ] **Step 4: Commit（docs）**

```bash
git add CLAUDE.md docs/adr/0009-no-ci-manual-verification.md
git commit -m "docs: record host-test layer in CLAUDE.md verification + ADR-0009 revision"
```

- [ ] **Step 5: 更新本 plan 状态行**

头部追加 `> **Status:** Executed（见合并提交）。`（pre-a/pre-b 惯例；随切片最后提交落库。）

---

## 执行方式（协议已定）

loop 协议固定：主会话 `superpowers:executing-plans` 自实现（小切片、subagent 预算留给架构师关卡②与测试专家独立 gate）→ 测试专家独立复跑 → 架构师关卡② review → `--no-ff` 合回 develop。
