/* ysf_c4fm.c - see ysf_c4fm.h */
#include "ysf_c4fm.h"

#include <math.h>
#include <string.h>

#include "ysf.h"

static int8_t SYNC_LVL[20];
static float SYNC_MEAN, SYNC_VAR;
static bool sync_ready;

static void sync_init(void)
{
    if (sync_ready)
        return;
    float m = 0.0f, v = 0.0f;
    for (int i = 0; i < 20; i++) {
        SYNC_LVL[i] = (int8_t)ysf_dibit_to_level((uint8_t)((YSF_SYNC >> (38 - 2 * i)) & 3u));
        m += SYNC_LVL[i];
    }
    m /= 20.0f;
    for (int i = 0; i < 20; i++)
        v += ((float)SYNC_LVL[i] - m) * ((float)SYNC_LVL[i] - m);
    SYNC_MEAN = m;
    SYNC_VAR = v;
    sync_ready = true;
}

void ysf_c4fm_slicer_init(ysf_c4fm_slicer_t *s)
{
    sync_init();
    memset(s, 0, sizeof(*s));
    s->gain = 1.0f;
    s->min_corr = 0.8f;
}

float ysf_c4fm_norm(const ysf_c4fm_slicer_t *s, float x)
{
    if (s->locked)
        return (x - s->offset) * s->gain;
    float span = s->peak_pos - s->peak_neg;
    if (span <= 0.0f)
        return x;
    return (x - (s->peak_pos + s->peak_neg) * 0.5f) * (6.0f / span);
}

uint8_t ysf_c4fm_slice(ysf_c4fm_slicer_t *s, float x, bool *sync)
{
    if (sync)
        *sync = false;
    s->hist[s->pos] = x;
    s->pos = (s->pos + 1u) % 20u;

    /* fallback peak tracker with slow decay */
    if (x > s->peak_pos)
        s->peak_pos = x;
    else
        s->peak_pos -= 0.002f * (s->peak_pos - x);
    if (x < s->peak_neg)
        s->peak_neg = x;
    else
        s->peak_neg += 0.002f * (x - s->peak_neg);

    /* correlation with the sync, oldest sample first */
    float mx = 0.0f;
    for (int i = 0; i < 20; i++)
        mx += s->hist[i];
    mx /= 20.0f;
    float sxl = 0.0f, sxx = 0.0f;
    for (unsigned i = 0; i < 20; i++) {
        float xi = s->hist[(s->pos + i) % 20u] - mx;
        sxl += xi * ((float)SYNC_LVL[i] - SYNC_MEAN);
        sxx += xi * xi;
    }
    if (sxx > 0.0f && sxl > 0.0f && sxl / sqrtf(sxx * SYNC_VAR) >= s->min_corr) {
        float a = sxl / SYNC_VAR; /* x = a * level + b */
        float b = mx - a * SYNC_MEAN;
        if (a > 0.0f) {
            s->gain = 1.0f / a;
            s->offset = b;
            s->locked = true;
            if (sync)
                *sync = true;
        }
    }
    return ysf_level_to_dibit(ysf_c4fm_norm(s, x));
}

float ysf_c4fm_deviation_hz(uint8_t dibit, bool narrow)
{
    float hz = 900.0f * (float)ysf_dibit_to_level(dibit);
    return narrow ? hz * 0.5f : hz;
}
