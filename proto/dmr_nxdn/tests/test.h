#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; \
    if (_a != _b) { g_fail++; printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* deterministic PRNG for tests */
extern uint32_t g_seed;
static inline uint32_t trand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}

void test_fec(void);
void test_dmr(void);
void test_dmr_t3(void);
void test_nxdn(void);
void test_crypt(void);
void test_simplex(void);
#endif
