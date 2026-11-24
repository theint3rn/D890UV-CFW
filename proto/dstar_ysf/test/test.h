/* Minimal host test harness. */
#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdio.h>

extern int g_fail, g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_fail++;                                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        long long a_ = (long long)(a), b_ = (long long)(b);                            \
        g_checks++;                                                                    \
        if (a_ != b_) {                                                                \
            g_fail++;                                                                  \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                   a_, b_);                                                            \
        }                                                                              \
    } while (0)

/* deterministic xorshift */
static inline uint32_t rnd(void)
{
    static uint32_t s = 0x12345678u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

void test_common(void);
void test_dstar(void);
void test_ysf(void);

#endif
