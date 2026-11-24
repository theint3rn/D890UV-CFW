/* cfw/dsp demodulators, see dsp_demod.h. */
#include <math.h>
#include <string.h>
#include "dsp_demod.h"
#ifdef DSP_TUNE
#include <stdlib.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* sync words: dibits (2 bits per symbol) or bits, MSB first = first on air */
static const dsp_mode_info_t k_modes[DSP_MODE_COUNT] = {
    /* BS voice/data, MS voice/data, direct mode TS1 voice/data, TS2 voice/data (ETSI TS 102 361-1 table 9.2) */
    [DSP_MODE_DMR]    = { "DMR",    4800, 4, DSP_F_RRC,   1944.0f, 8, true,
                          { { 0x755FD7DF75F7ull, 24 }, { 0xDFF57D75DF5Dull, 24 },
                            { 0x7F7D5DD57DFDull, 24 }, { 0xD5D7F77FD757ull, 24 },
                            { 0x5D577F7757FFull, 24 }, { 0xF7FDD5DDFD55ull, 24 },
                            { 0x7DFFD5F55D5Full, 24 }, { 0xD7557F5FF7F5ull, 24 } } },
    [DSP_MODE_P25P1]  = { "P25",    4800, 4, DSP_F_ID,    1800.0f, 1, false, { { 0x5575F5FF77FFull, 24 } } },
    [DSP_MODE_YSF]    = { "YSF",    4800, 4, DSP_F_ID,    2700.0f, 1, false, { { 0xD471C9634Dull, 20 } } },
    [DSP_MODE_NXDN48] = { "NXDN48", 2400, 4, DSP_F_RRC,   1050.0f, 1, false, { { 0xCDF59ull, 10 } } },
    [DSP_MODE_NXDN96] = { "NXDN96", 4800, 4, DSP_F_RRC,   2400.0f, 1, false, { { 0xCDF59ull, 10 } } },
    [DSP_MODE_DSTAR]  = { "DSTAR",  4800, 2, DSP_F_GAUSS, 1200.0f, 2, false, { { 0xAAB468ull, 24 }, { 0x7650ull, 15 } } },
    [DSP_MODE_P25P2]  = { "P25P2",  6000, 4, DSP_F_ID,    2250.0f, 1, false, { { 0x575D57F7FFull, 20 } } },
};

const dsp_mode_info_t *dsp_mode_info(dsp_mode_t m) { return (unsigned)m < DSP_MODE_COUNT ? &k_modes[m] : NULL; }

float dsp_level_of(uint8_t dibit)
{
    static const float lv[4] = { 1.0f, 3.0f, -1.0f, -3.0f };
    return lv[dibit & 3];
}

/* level of symbol i of a sync word */
static float sync_level(const dsp_mode_info_t *mi, int s, int i)
{
    uint8_t len = mi->sync[s].len;
    if(mi->levels == 2) { return ((mi->sync[s].bits >> (len - 1 - i)) & 1) ? 1.0f : -1.0f; }
    return dsp_level_of((uint8_t)((mi->sync[s].bits >> (2 * (len - 1 - i))) & 3));
}

/* ---------------- filters ---------------- */
static int rrc_taps(float *h, float sps, float alpha, int span)
{
    int n = (int)(span * sps) | 1;
    if(n > DSP_MAX_TAPS) { n = DSP_MAX_TAPS | 1; if(n > DSP_MAX_TAPS) { n -= 2; } }
    int c = n / 2;
    float sum = 0;
    for(int i = 0; i < n; i++) {
        float t = (float)(i - c) / sps;           /* in symbols */
        float v;
        if(fabsf(t) < 1e-6f) {
            v = 1.0f - alpha + 4.0f * alpha / (float)M_PI;
        } else if(fabsf(fabsf(t) - 1.0f / (4.0f * alpha)) < 1e-4f) {
            v = alpha / sqrtf(2.0f) * ((1.0f + 2.0f / (float)M_PI) * sinf((float)M_PI / (4.0f * alpha)) +
                                       (1.0f - 2.0f / (float)M_PI) * cosf((float)M_PI / (4.0f * alpha)));
        } else {
            float pt = (float)M_PI * t;
            v = (sinf(pt * (1.0f - alpha)) + 4.0f * alpha * t * cosf(pt * (1.0f + alpha))) /
                (pt * (1.0f - 16.0f * alpha * alpha * t * t));
        }
        h[i] = v;
        sum += v;
    }
    for(int i = 0; i < n; i++) { h[i] /= sum; }
    return n;
}

static int gauss_taps(float *h, float sps, float bt, int span)
{
    int n = (int)(span * sps) | 1;
    if(n > DSP_MAX_TAPS) { n = DSP_MAX_TAPS - ((DSP_MAX_TAPS & 1) ? 0 : 1); }
    int c = n / 2;
    float sigma = sqrtf(logf(2.0f)) / (2.0f * (float)M_PI * bt);   /* in symbols */
    float sum = 0;
    for(int i = 0; i < n; i++) {
        float t = (float)(i - c) / sps;
        h[i] = expf(-t * t / (2.0f * sigma * sigma));
        sum += h[i];
    }
    for(int i = 0; i < n; i++) { h[i] /= sum; }
    return n;
}

static int boxcar_taps(float *h, float sps)
{
    int n = (int)(sps + 0.5f);
    if(n < 1) { n = 1; }
    for(int i = 0; i < n; i++) { h[i] = 1.0f / (float)n; }
    return n;
}

void dsp_init(dsp_demod_t *d, dsp_mode_t mode, uint32_t fs_in, const dsp_opts_t *opts)
{
    memset(d, 0, sizeof *d);
    d->mode = mode;
    d->mi = &k_modes[mode];
    d->fs_in = fs_in;
    d->dec = fs_in >= 40000 ? 2 : 1;              /* 48 kHz ADC -> 24 kHz DSP */
    d->fs = fs_in / d->dec;
    d->sps = (float)d->fs / (float)d->mi->sym_rate;
    switch(d->mi->filter) {
    case DSP_F_RRC:   d->ntaps = (uint16_t)rrc_taps(d->taps, d->sps, 0.2f, 8); break;
    case DSP_F_GAUSS: d->ntaps = (uint16_t)gauss_taps(d->taps, d->sps, 0.5f, 3); break;
    default:          d->ntaps = (uint16_t)boxcar_taps(d->taps, d->sps); break;
    }
    if(opts && opts->tau_us > 0) {
        d->de_on = true;
        d->de_a = opts->tau_us * 1e-6f * (float)d->fs;
    }
    d->t_next = d->sps;
    d->kp = 0.03f;
    d->ki = 0.0005f;
    d->pol = (opts && opts->invert) ? -1.0f : 1.0f;
    d->gain = d->pol;
#ifdef DSP_TUNE
    if(getenv("KP")) { d->kp = (float)atof(getenv("KP")); }
    if(getenv("KI")) { d->ki = (float)atof(getenv("KI")); }
#endif
    d->mabs = 0;
    d->evm = 1.0f;
}

int dsp_quality(const dsp_demod_t *d)
{
    float q = 100.0f - d->evm * 300.0f;
    return q < 0 ? 0 : q > 100 ? 100 : (int)q;
}

static float interp(const dsp_demod_t *d, float t)
{
    /* t in MF sample time, must be within the history */
    float fl = floorf(t);
    uint32_t i0 = (uint32_t)fl;
    float fr = t - fl;
    float y0 = d->yr[i0 % DSP_HIST], y1 = d->yr[(i0 + 1) % DSP_HIST];
    return y0 + fr * (y1 - y0);
}

/* correlate the last strobes against the sync words; on a match: polarity, DC and level from a LS fit */
static void sync_check(dsp_demod_t *d)
{
    const dsp_mode_info_t *mi = d->mi;
    for(int s = 0; s < mi->nsync; s++) {
        int len = mi->sync[s].len;
        if(d->since_sync < (uint32_t)len && d->syncs) { continue; }
        float sy = 0, sp = 0, syy = 0, spp = 0, syp = 0;
        for(int i = 0; i < len; i++) {
            float y = d->sr[(d->sri + DSP_MAX_SYNC - len + i) % DSP_MAX_SYNC];
            float p = sync_level(mi, s, i);
            sy += y; sp += p; syy += y * y; spp += p * p; syp += y * p;
        }
        float n = (float)len;
        float cyy = syy - sy * sy / n, cpp = spp - sp * sp / n, cyp = syp - sy * sp / n;
        if(cyy <= 0 || cpp <= 0) { continue; }
        float corr = cyp / sqrtf(cyy * cpp);
        float thr = len >= 20 ? 0.90f : 0.96f;
        if(mi->pol_fixed) { corr *= d->pol; }          /* only the expected polarity counts */
        if(corr < thr && (mi->pol_fixed || -corr < thr)) { continue; }
        if(d->locked && (corr > 0) != (d->gain > 0)) { continue; }   /* no polarity flips while locked */
        if(len < 20) {          /* short words (NXDN FSW 10, D-STAR frame sync 15): also an exact hard match */
            float g = cyp / cpp, dc = (sy - g * sp) / n;
            bool exact = true;
            for(int i = 0; i < len && exact; i++) {
                float y = d->sr[(d->sri + DSP_MAX_SYNC - len + i) % DSP_MAX_SYNC];
                float yn = (y - dc) / g, p = sync_level(mi, s, i);
                float q = mi->levels == 2 ? (yn > 0 ? 1.0f : -1.0f)
                        : (yn >= 2.0f ? 3.0f : yn >= 0 ? 1.0f : yn >= -2.0f ? -1.0f : -3.0f);
                exact = q == p;
            }
            if(!exact) { continue; }
        }
        float g = cyp / cpp;
        d->gain = g;
        d->dc = (sy - g * sp) / n;
        d->locked = true;
        d->since_sync = 0;
        d->syncs++;
        d->last_corr = corr;
        return;
    }
}

static uint8_t slice(const dsp_demod_t *d, float yn, int8_t *soft, float *err)
{
    if(d->mi->levels == 2) {
        float e = fabsf(yn) - 1.0f;
        *err = e * e;
        float s = yn * 64.0f;
        *soft = (int8_t)(s > 127 ? 127 : s < -127 ? -127 : (s == 0 ? 1 : s));
        return yn > 0 ? 1 : 0;
    }
    uint8_t sym;
    float lvl;
    if(yn >= 2.0f) { sym = 1; lvl = 3; } else if(yn >= 0) { sym = 0; lvl = 1; }
    else if(yn >= -2.0f) { sym = 2; lvl = -1; } else { sym = 3; lvl = -3; }
    float dist = fminf(fabsf(yn - 2.0f), fminf(fabsf(yn), fabsf(yn + 2.0f)));
    float s = dist * 127.0f;
    *soft = (int8_t)(s > 127 ? 127 : s < 1 ? 1 : s);
    *err = (yn - lvl) * (yn - lvl);
    return sym;
}

static float nearest_level(const dsp_demod_t *d, float yn)
{
    if(d->mi->levels == 2) { return yn > 0 ? 1.0f : -1.0f; }
    return yn >= 2.0f ? 3.0f : yn >= 0 ? 1.0f : yn >= -2.0f ? -1.0f : -3.0f;
}

/* one symbol strobe: y = on-time sample, mid = half a symbol earlier */
static int strobe(dsp_demod_t *d, float y, float mid, uint8_t *sym, int8_t *soft)
{
    /* coarse estimates (used until a sync word is seen) */
    if(d->quiet < 3) {                           /* not during unmodulated gaps */
        d->mavg += 0.005f * (y - d->mavg);
        d->mabs += 0.02f * (fabsf(y - d->mavg) - d->mabs);
    }
    if(!d->syncs) {
        float pol = d->gain < 0 ? -1.0f : 1.0f;
        d->dc = d->mavg;
        d->gain = pol * (d->mi->levels == 4 ? d->mabs / 2.0f : d->mabs);
        if(fabsf(d->gain) < 1e-9f) { d->gain = pol * 1e-9f; }
    }
    /* timing error (Gardner), normalised by the outer level */
    float outer = fabsf(d->gain) * (d->mi->levels == 4 ? 3.0f : 1.0f);
    float e = (mid - d->dc) * (y - d->prev_sym) / (outer * outer + 1e-12f);
    if(e > 1.0f) { e = 1.0f; } else if(e < -1.0f) { e = -1.0f; }
    d->prev_sym = y;
    /* no timing information on an unmodulated carrier (before a call, DMR TDMA gaps): hold the loop.
     * Before the first sync word: faster acquisition gains. */
    /* ... and for the first symbols after such a gap: the 0 -> symbol edge is not a symbol transition */
    if(d->quiet >= 2) { d->post_quiet = 0; } else if(d->post_quiet < 255) { d->post_quiet++; }
    bool flat = d->quiet >= 2 || d->post_quiet < 3;
    float kp = d->kp, ki = d->ki;
    if(flat) { e = 0; }
    d->t_int -= ki * e * d->sps;
    float lim = 0.001f * d->sps;                   /* +/-1000 ppm clock: crystals are ~10 ppm; a wider clamp lets a
                                                     * pre-sync transient run the phase off during DMR TDMA gaps */
    if(d->t_int > lim) { d->t_int = lim; } else if(d->t_int < -lim) { d->t_int = -lim; }
    d->t_next += d->sps + d->t_int - kp * e * d->sps;

    /* sync / lock */
    d->sr[d->sri] = y;
    d->sri = (uint8_t)((d->sri + 1) % DSP_MAX_SYNC);
    d->since_sync++;
    sync_check(d);
    if(d->since_sync > d->mi->sym_rate * 2u) { d->locked = false; }

    float yn = (y - d->dc) / d->gain;
    float err;
    *sym = slice(d, yn, soft, &err);
    /* unmodulated carrier (DMR direct-mode / inbound TDMA gaps): no valid symbol sits near 0 level units, so a run
     * of them freezes the level tracking instead of shrinking the gain towards the idle carrier */
    if(fabsf(yn) < 0.5f) { if(d->quiet < 255) { d->quiet++; } } else { d->quiet = 0; }
    bool hold = d->quiet >= 3;
    if(!hold) { d->evm += 0.01f * (err - d->evm); }
    /* decision directed DC / level tracking once a sync word fixed polarity and scale */
    if(d->syncs && !hold) {
        float lvl = nearest_level(d, yn);
        float ex = y - (d->dc + d->gain * lvl);
        d->dc += 0.004f * ex;
        d->gain += 0.004f * ex * lvl / (lvl * lvl);
    }
    d->syms++;
    return 1;
}

size_t dsp_process(dsp_demod_t *d, const float *x, size_t n, uint8_t *sym, int8_t *soft, size_t max)
{
    size_t out = 0;
    for(size_t k = 0; k < n; k++) {
        /* decimate */
        d->dacc += x[k];
        if(++d->dcnt < d->dec) { continue; }
        float v = d->dacc / (float)d->dec;
        d->dacc = 0; d->dcnt = 0;
        /* inverse de-emphasis: x + tau * dx/dt */
        if(d->de_on) { float u = v + d->de_a * (v - d->de_prev); d->de_prev = v; v = u; }
        /* matched filter */
        d->xr[d->xi] = v;
        float acc = 0;
        uint16_t j = d->xi;
        for(uint16_t t = 0; t < d->ntaps; t++) {
            acc += d->taps[t] * d->xr[j];
            j = j ? (uint16_t)(j - 1) : (uint16_t)(d->ntaps - 1);
        }
        d->xi = (uint16_t)((d->xi + 1) % d->ntaps);
        d->yr[d->n % DSP_HIST] = acc;
        d->n++;
        /* strobes due up to the newest sample (index n-1); keep half a symbol + 1 of history */
        while(d->t_next <= (float)(d->n - 1)) {
            if(d->t_next < (float)d->n - (float)DSP_HIST + 1 || d->t_next - d->sps * 0.5f < 0) {
                d->t_next += d->sps;                 /* fell out of the history (start-up): skip */
                continue;
            }
            float y = interp(d, d->t_next);
            float mid = interp(d, d->t_next - d->sps * 0.5f);
            if(out < max) {
                strobe(d, y, mid, &sym[out], &soft[out]);
                out++;
            } else {
                uint8_t s; int8_t q;
                strobe(d, y, mid, &s, &q);
            }
        }
    }
    return out;
}
