/*
 * p25_c4fm.c - C4FM symbol modulator (RC pulse) and discriminator-domain
 * demodulator (Gardner timing recovery, adaptive DC/level slicer).
 */
#include "p25_c4fm.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

int p25_c4fm_mod_init(p25_c4fm_mod_t *m, unsigned sps)
{
    if (sps < 2 || sps > P25_C4FM_MAX_SPS) return -1;
    memset(m, 0, sizeof *m);
    m->sps = sps;
    const float alpha = 0.2f;
    unsigned n = P25_C4FM_SPAN * sps;
    for (unsigned i = 0; i < n; i++) {
        float t = ((float)i - (float)(n / 2)) / (float)sps;
        float sinc = fabsf(t) < 1e-6f ? 1.0f : sinf(PI_F * t) / (PI_F * t);
        float den = 1.0f - (2.0f * alpha * t) * (2.0f * alpha * t);
        float rc = fabsf(den) < 1e-6f ? (PI_F / 4.0f) : cosf(PI_F * alpha * t) / den;
        m->taps[i] = sinc * rc;
    }
    return 0;
}

size_t p25_c4fm_mod(p25_c4fm_mod_t *m, const uint8_t *dib, size_t n, int16_t *out, float scale)
{
    size_t o = 0;
    unsigned sps = m->sps;
    for (size_t k = 0; k < n; k++) {
        memmove(m->hist + 1, m->hist, (P25_C4FM_SPAN - 1) * sizeof(float));
        m->hist[0] = (float)p25_dibit_level(dib[k]);
        for (unsigned p = 0; p < sps; p++) {
            float y = 0;
            for (unsigned j = 0; j < P25_C4FM_SPAN; j++) y += m->hist[j] * m->taps[j * sps + p];
            float v = y * scale;
            if (v > 32767.f) v = 32767.f;
            if (v < -32768.f) v = -32768.f;
            out[o++] = (int16_t)lrintf(v);
        }
    }
    return o;
}

int p25_c4fm_demod_init(p25_c4fm_demod_t *d, unsigned sps)
{
    if (sps < 2 || sps > P25_C4FM_MAX_SPS + 10) return -1;
    memset(d, 0, sizeof *d);
    d->sps = sps;
    d->timing_gain = 1.0f / 64.0f;
    d->level_gain = 0.02f;
    return 0;
}

static void emit_sym(p25_c4fm_demod_t *d, float y, uint8_t *out, float *soft, size_t *no)
{
    if (d->amp <= 0) d->amp = fabsf(y) + 1.0f;
    d->amp += d->level_gain * (fabsf(y) - d->amp);
    d->dc += d->level_gain * 0.5f * y;
    float unit = d->amp * 0.5f;              /* E|level| = 2 units */
    float lv = y / (unit + 1e-3f);
    if (soft) soft[*no] = lv;
    out[(*no)++] = lv >= 2.0f ? 1 : lv >= 0.0f ? 0 : lv >= -2.0f ? 2 : 3;
}

static float kurt(const p25_c4fm_demod_t *d, unsigned ph)
{
    float e2 = d->energy[ph];
    return e2 > 0 ? d->energy4[ph] / (e2 * e2) : 3.0f;
}

size_t p25_c4fm_demod(p25_c4fm_demod_t *d, const int16_t *in, size_t n, uint8_t *out, float *soft)
{
    size_t no = 0;
    const unsigned last = d->sps - 1;
    for (size_t i = 0; i < n; i++) {
        float y = (float)in[i] - d->dc;
        unsigned ph = d->ph;
        float y2 = y * y;
        d->energy[ph] += d->timing_gain * (y2 - d->energy[ph]);
        d->energy4[ph] += d->timing_gain * (y2 * y2 - d->energy4[ph]);
        if (ph == d->best) {
            if (d->skip) d->skip = 0;
            else emit_sym(d, y, out, soft, &no);
        }
        if (++d->ph >= d->sps) {
            d->ph = 0;
            /* Eye centre = lowest kurtosis E[y^4]/E[y^2]^2 (4-level data: 1.64,
             * ISI pushes it towards 3). Move one phase at a time, only if
             * clearly better (hysteresis). */
            unsigned b = d->best, up = b == last ? 0 : b + 1, dn = b == 0 ? last : b - 1;
            float kb = kurt(d, b) * 0.98f, ku = kurt(d, up), kd = kurt(d, dn);
            if (ku < kb && ku <= kd) d->best = up;
            else if (kd < kb) d->best = dn;
            if (b == last && d->best == 0) d->skip = 1;               /* would sample the same symbol twice */
            else if (b == 0 && d->best == last) emit_sym(d, y, out, soft, &no); /* would miss one */
        }
    }
    return no;
}

void p25_c4fm_demod_train(p25_c4fm_demod_t *d, const float *raw)
{
    /* sync = 0x5575F5FF77FF: levels from dibits; fit raw = dc + u*level */
    static const uint64_t sync = 0x5575F5FF77FFULL;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < 24; i++) {
        float x = (float)p25_dibit_level((uint8_t)((sync >> (46 - 2 * i)) & 3));
        sx += x; sy += raw[i]; sxx += x * x; sxy += x * raw[i];
    }
    float den = 24 * sxx - sx * sx;
    if (den == 0) return;
    /* raw[] are soft outputs: soft = (s - dc) / unit; fit soft = a + b*level */
    float b = (24 * sxy - sx * sy) / den;
    float a = (sy - b * sx) / 24;
    if (b <= 0) return;
    float unit = d->amp * 0.5f;
    d->dc += a * unit;
    d->amp = 2.0f * unit * b;
}
