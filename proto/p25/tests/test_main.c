#include "tst.h"

int tst_fail, tst_pass;
uint32_t tst_rng = 0x12345678u;

int main(void)
{
    struct { const char *name; void (*fn)(void); } t[] = {
        { "fec", test_fec }, { "dsp", test_dsp }, { "phase1", test_p1 },
        { "phase2", test_p2 }, { "trunk", test_trunk }, { "crypt", test_crypt },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int f0 = tst_fail, p0 = tst_pass;
        t[i].fn();
        printf("%-8s %4d passed, %d failed\n", t[i].name, tst_pass - p0, tst_fail - f0);
    }
    printf("TOTAL    %4d passed, %d failed\n", tst_pass, tst_fail);
    return tst_fail ? 1 : 0;
}
