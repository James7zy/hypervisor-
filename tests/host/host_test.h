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
void test_mmio(void);

#endif /* HV_HOST_TEST_H */
