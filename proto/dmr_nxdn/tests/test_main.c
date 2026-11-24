#include "test.h"

#include <stdlib.h>

int g_checks, g_fail;
uint32_t g_seed = 0x12345678u;

int main(int argc, char **argv)
{
    if (argc > 1) g_seed = (uint32_t)strtoul(argv[1], NULL, 0) | 1u;   /* optional seed */
    struct { const char *name; void (*fn)(void); } t[] = {
        { "fec", test_fec }, { "dmr", test_dmr }, { "dmr_t3", test_dmr_t3 }, { "nxdn", test_nxdn }, { "crypt", test_crypt }, { "simplex", test_simplex },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int f0 = g_fail, c0 = g_checks;
        t[i].fn();
        printf("%-8s %5d checks, %d failed\n", t[i].name, g_checks - c0, g_fail - f0);
    }
    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
