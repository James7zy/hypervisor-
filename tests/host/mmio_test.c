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
