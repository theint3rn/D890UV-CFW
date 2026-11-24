#include "test.h"

int g_fail, g_checks;

int main(void)
{
    printf("common\n");
    test_common();
    printf("dstar\n");
    test_dstar();
    printf("ysf\n");
    test_ysf();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
