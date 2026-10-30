/* SPDX-License-Identifier: TBD */
#include "host_test.h"

int tests_run;
int tests_failed;

int main(void)
{
    test_string();
    test_mmio();

    if (tests_failed == 0) {
        printf("PASS (%d assertions)\n", tests_run);
        return 0;
    }
    printf("FAIL (%d/%d assertions failed)\n", tests_failed, tests_run);
    return 1;
}
