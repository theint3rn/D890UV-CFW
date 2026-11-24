/* tiny test harness for the P25 host tests */
#ifndef TST_H
#define TST_H
#include <stdio.h>
#include <stdint.h>

extern int tst_fail, tst_pass;
#define CHECK(c) do { if (c) tst_pass++; else { tst_fail++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) tst_pass++; else { tst_fail++; \
    printf("  FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); } } while (0)

/* deterministic PRNG (xorshift32) */
extern uint32_t tst_rng;
static inline uint32_t rnd(void) { uint32_t x = tst_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return tst_rng = x; }
static inline uint32_t rndn(uint32_t n) { return rnd() % n; }

void test_fec(void);
void test_p1(void);
void test_p2(void);
void test_trunk(void);
void test_dsp(void);
void test_crypt(void);
#endif
