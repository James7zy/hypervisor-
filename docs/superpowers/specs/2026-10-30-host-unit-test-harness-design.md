# Host 单测 harness 设计（nasa-glm-loop 首个切片）

> **Status:** Approved（关卡① 2026-10-30 通过，APPROVE_WITH_CHANGES；架构师实证发现的
> flag 集缺陷与 Makefile 集成问题已按裁决修入本文）。**日期:** 2026-10-30。
> **来源:** [nasa-glm-loop 设计](2026-10-30-nasa-glm-loop-design.md) "分级 TDD" 一节。

## 背景与问题

仓库现有验证只有 QEMU 场景层（`make test`，7 个场景 + 4 项静态检查，分钟级反馈）。
loop 协议要求纯逻辑严格 RED-GREEN-REFACTOR，分钟级 QEMU 反馈撑不起 TDD 节奏，且
M11 调度器（位图、runqueue、优先级）是纯逻辑最密集的里程碑——需要 host 单测层。

## 目标与非目标

**目标**：`make host-test` 在开发机上用 host gcc 直接编译**同一份** hypervisor 源文件
并运行单测，秒级反馈；零第三方依赖（ponytail 第 4 条）。

**非目标**：不改固件构建（hypervisor.elf 的链接不碰 tests/host 的任何东西）；不替代
QEMU gate（硬件路径仍以 gate 为强制验收）；不追求覆盖现有全部模块。

## 方案

### 目录与 runner

```
tests/host/
  runner.c        # assert 宏 + main：顺序调用所有 test_* 函数，失败打印 文件:行，非零退出
  host_test.h     # TEST_ASSERT 宏 + 计数器 extern + test_* 原型（关卡① 2026-10-30 裁决加入）
  mmio_test.c     # 首批被测：common/io/mmio.c
  string_test.c   # 首批被测：lib/string.c
```

- runner 形态：**无注册表**，`main` 里显式调用每个 `test_*`（模块少时最简；>15 个再谈注册）。
- 断言宏：`TEST_ASSERT(cond)` 打印 `FAIL file:line: expr` 并计数，跑完全部后汇总，
  有失败则 `return 1`（供 `make host-test` 判定）。纯文本输出，不做机器可解析格式
  （无 CI 即无机器消费者——ADR-0009）。

### Makefile 集成

- 变量沿用现有 `HOST_CC := cc`（Makefile:114，check-offsets 先例）；**不引入**
  `HOSTCC ?=`（仓库唯一先例是直接赋值，`?=` 的环境覆盖是无人要求的灵活性）。
- `host-test` 目标**必须加进 Makefile:27 的 goal filter**（`%_defconfig defconfig …`
  名单）：否则走 need-config 路径，要求 `.config` 与交叉工具链在 PATH，普通 shell 里
  解析期即 $(error)，与"秒级反馈"目标冲突。
- 编译用独立 `HOST_TEST_CFLAGS` 变量，**不得复用**含 arch-cflags 的全局 CFLAGS。
- 产物放 `$(BUILD_DIR)/host/`（与 build/check_offsets 同风格；build/ 已被
  .gitignore 与 clean 覆盖，不污染 git status）。
- v1 **不并入** `make test` 聚合链：make test 是 QEMU gate 语义（ADR-0009 强制验收，
  分钟级），host-test 是秒级开发反馈，混入会模糊 gate 定义并让 gate 依赖 host 机环境。
  作为补偿：host-test 进 loop 的合并关卡②（纯逻辑改动必过），并在本切片落地时给
  CLAUDE.md 验证一节加一行"纯逻辑改动：`make host-test`"。M11 中期单测量大且仍秒级
  时再议并入（届时修订 ADR-0009 记录）。

### host 编译 flag 集（完整清单，实证钉死）

```
-Wall -Wextra -Werror -O2 -g -fno-builtin -U_FORTIFY_SOURCE -std=c11
```

两条非常规 flag 是架构师实证的硬要求，缺一即坏：

- **`-fno-builtin`**：host gcc 13.3 在 `-O2` 下会把 string.c 的 memset 循环优化为对
  memset 自身的调用（实测重定位 `R_X86_64_PLT32 memset`），任何 n>0 调用即无限递归。
  固件不受影响（aarch64 侧 `-ffreestanding` 隐含 `-fno-builtin`），但 host 侧必须显式加。
- **`-U_FORTIFY_SOURCE`**：Ubuntu gcc 默认开 fortify，`<string.h>` 宏会把测试里的
  memset 重定向到 glibc `__memset_chk`（objdump 实证），造成"测的是 libc 不是被测
  代码"的假绿。`-U` 后 call 直达被测 memset。

后续模块加入 HOST_TEST_SRCS 时照抄此集，不逐项重议。

### 首批被测模块与 arch-neutral 约束

| 模块 | 依赖 | 入选理由 |
|---|---|---|
| `lib/string.c` | 仅 `types.h` | **哨兵**：memset 恰是 host 编译陷阱最集中的模块（builtin/fortify/自递归），这组断言把 harness 的 flag 正确性钉在原地。范围限于现有行为：n=0 不写、n>0 填充正确、返回值==dst；不测 libc 等价性 |
| `common/io/mmio.c` | `types.h` + `mmio.h` | 有真实分支逻辑（register 上限、lookup 边界、dispatch 偏移），M3 起就在用；架构师已实证 host 编译+断言全过（GREEN 不需要改产品代码） |

**约束（架构师执法点）**：进 `HOST_TEST_SRCS` 的模块不得引入 ARM64 内联汇编、`arch/`
头或 `percpu/spinlock/printk`（这些需 host shim，v2 再议）。shim 触发条件：M11 出现
第一个"逻辑值得测但依赖 percpu"的模块时，由架构师批准 shim 方案，v1 不预建。

### TDD 纪律对 harness 自身的适用

harness 切片没有"新实现"可被失败测试驱动——**RED 证据作为工作树内验收操作**：故意
错一个断言 → 观察 `FAIL file:line` 与非零退出码 → 还原，全程不留红 commit。落库历史
保持全绿。"RED commit → GREEN commit 顺序可见"的纪律从 harness 之后的 M11 纯逻辑
模块开始适用（那时失败测试先于新实现存在）。

## 验收（DoD）

- `make host-test` 在普通 shell（无交叉工具链 export、无 .config）可跑，全绿，秒级。
- 上述边界用例存在且通过；故意破坏一个断言能看到 FAIL 输出与非零退出码（验收操作，
  还原后落库）。
- `make test` 不受影响；固件产物 bit 级不变（链接输入未变，hv-objs 显式清单不 glob）。
- 收尾：CLAUDE.md 验证一节加一行"纯逻辑改动：`make host-test`"；ADR-0009 补修订
  记录（host 单测层启用，gate 语义不变，仿 0015 修订惯例）。

## 架构师裁决记录（2026-10-30，已并入上文）

1. host-test 不并入 `make test`（v1），理由与补偿见 Makefile 集成一节。
2. 输出纯文本，不做机器可解析格式。
3. string_test 留，范围限于 memset 现有行为（哨兵价值，非覆盖率价值）。

已知不阻塞风险（记录在案）：mmio.c 静态全局状态无 reset 接口，测试文件 >2 且共用
该状态时再定约定；发行版 fortify 默认值差异已被 `-U_FORTIFY_SOURCE` 钉死。
