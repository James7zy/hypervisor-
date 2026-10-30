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
