/* C4FM modulator -> (gain, DC, noise, timing offset) -> demodulator. */
#include "tst.h"
#include "p25_c4fm.h"
#include "p25_p1.h"
#include <math.h>
#include <string.h>

static float gauss(void)
{
    float u1 = ((float)rndn(1u << 24) + 1.0f) / (float)(1u << 24);
    float u2 = (float)rndn(1u << 24) / (float)(1u << 24);
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static void run(unsigned sps, int skip, float dc, float gain, float noise)
{
    enum { N = 3000 };
    static uint8_t tx[N], rxd[N + 10];
    static int16_t smp[N * 20 + 100];
    for (int i = 0; i < N; i++) tx[i] = (uint8_t)rndn(4);
    p25_c4fm_mod_t m;
    CHECK_EQ(p25_c4fm_mod_init(&m, sps), 0);
    size_t ns = p25_c4fm_mod(&m, tx, N, smp, 3000.0f);
    CHECK_EQ(ns, N * sps);
    for (size_t i = 0; i < ns; i++) {
        float v = (float)smp[i] * gain + dc + noise * gauss();
        smp[i] = (int16_t)lrintf(v);
    }
    p25_c4fm_demod_t d;
    CHECK_EQ(p25_c4fm_demod_init(&d, sps), 0);
    size_t nd = p25_c4fm_demod(&d, smp + skip, ns - (size_t)skip, rxd, NULL);
    CHECK(nd > N - 10);
    /* find best alignment over the second half */
    int best_err = 1 << 30;
    for (int lag = -8; lag <= 8; lag++) {
        int err = 0;
        for (int i = N / 2; i < N - 20; i++) {
            int j = i + lag;
            if (j < 0 || j >= (int)nd) { err += 1; continue; }
            if (rxd[j] != tx[i]) err++;
        }
        if (err < best_err) best_err = err;
    }
    if (best_err > 3) printf("  dsp sps=%u skip=%d dc=%.0f gain=%.2f noise=%.0f: %d symbol errors\n",
                             sps, skip, dc, gain, noise, best_err);
    CHECK(best_err <= 3);
}

static void t_levels(void)
{
    CHECK_EQ(p25_dibit_level(1), 3);
    CHECK_EQ(p25_dibit_level(0), 1);
    CHECK_EQ(p25_dibit_level(2), -1);
    CHECK_EQ(p25_dibit_level(3), -3);
    for (uint8_t dd = 0; dd < 4; dd++) CHECK_EQ(p25_level_dibit(p25_dibit_level(dd)), dd);
}

void test_dsp(void)
{
    t_levels();
    run(10, 0, 0, 1.0f, 0);
    run(10, 3, 800, 0.6f, 60);
    run(10, 7, -1500, 1.4f, 150);
    run(5, 2, 300, 1.0f, 50);
    run(8, 5, 0, 0.3f, 20);
}
