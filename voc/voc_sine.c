/* cfw "sine" test vocoder, see voc_sine.h. Own code, no third-party sources. */
#include <math.h>
#include <string.h>
#include "voc_sine.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFFT 512
#define NWIN 256
#define FS   8000.0f
#define FMAX 3800.0f

static float s_win[NWIN];
static float s_wsum;
static float s_cos[NFFT / 2], s_sin[NFFT / 2];
static int   s_ready;

static void tables(void)
{
    if(s_ready) { return; }
    s_wsum = 0;
    for(int i = 0; i < NWIN; i++) {
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)NWIN);
        s_wsum += s_win[i];
    }
    for(int i = 0; i < NFFT / 2; i++) {
        s_cos[i] = cosf(2.0f * (float)M_PI * (float)i / (float)NFFT);
        s_sin[i] = -sinf(2.0f * (float)M_PI * (float)i / (float)NFFT);
    }
    s_ready = 1;
}

static void fft(float *re, float *im)
{
    for(int i = 1, j = 0; i < NFFT; i++) {                 /* bit reversal */
        int b = NFFT >> 1;
        for(; j & b; b >>= 1) { j ^= b; }
        j |= b;
        if(i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for(int len = 2; len <= NFFT; len <<= 1) {
        int step = NFFT / len;
        for(int i = 0; i < NFFT; i += len) {
            for(int k = 0; k < len / 2; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                float xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                float xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
        }
    }
}

static float f0_of(int b0) { return 16000.0f / ((float)b0 + 39.5f); }
static int   nharm(float f0) { int l = (int)(FMAX / f0); return l > VOC_MAXH ? VOC_MAXH : l; }

/* bits for harmonic k (0-based) given the amplitude budget */
static int hbits(int k, int L, int budget)
{
    if(budget >= L) { return budget / L + (k < budget % L ? 1 : 0); }
    return k < budget ? 1 : 0;
}

/* quantiser levels in dB below the frame peak; the last level of each table means "off" */
static float level_db(int b, int q)
{
    static const float l1[2] = { 0, -200 };
    static const float l2[4] = { 0, -10, -25, -200 };
    static const float l3[8] = { 0, -3, -6, -10, -15, -22, -32, -200 };
    switch(b) {
    case 0: return -200;
    case 1: return l1[q];
    case 2: return l2[q];
    case 3: return l3[q];
    default: { int n = 1 << b; return q == n - 1 ? -200 : -3.0f * (float)q; }
    }
}

static int quant_db(int b, float db)
{
    int n = 1 << b, best = n - 1;
    if(db < -30.0f) { return n - 1; }      /* Hann sidelobes (-31.5 dB) / noise floor: off */
    float bd = 1e9f;
    for(int q = 0; q < n; q++) {
        float l = level_db(b, q), d = fabsf((l < -150 ? -60.0f : l) - db);
        if(d < bd) { bd = d; best = q; }
    }
    return best;
}

static void put_bits(uint8_t *buf, int *pos, uint32_t v, int n)
{
    for(int i = n - 1; i >= 0; i--, (*pos)++) {
        if((v >> i) & 1) { buf[*pos >> 3] |= (uint8_t)(0x80 >> (*pos & 7)); }
    }
}

static uint32_t get_bits(const uint8_t *buf, int *pos, int n)
{
    uint32_t v = 0;
    for(int i = 0; i < n; i++, (*pos)++) { v = (v << 1) | ((buf[*pos >> 3] >> (7 - (*pos & 7))) & 1u); }
    return v;
}

void voc_sine_enc_init(voc_sine_enc_t *e) { memset(e, 0, sizeof *e); tables(); }

void voc_sine_dec_init(voc_sine_dec_t *d) { memset(d, 0, sizeof *d); tables(); }

/* magnitude spectrum, linearly interpolated at frequency f */
static float mag_at(const float *m, float f)
{
    float x = f / FS * NFFT;
    int i = (int)x;
    if(i < 0 || i + 1 >= NFFT / 2) { return 0; }
    float fr = x - (float)i;
    return m[i] + fr * (m[i + 1] - m[i]);
}

/* amplitude of a sinusoid near f: peak of the magnitude within +/- 1.5 bins */
static float amp_at(const float *m, float f)
{
    float x = f / FS * NFFT;
    int lo = (int)(x - 1.5f), hi = (int)(x + 1.5f) + 1;
    float pk = 0;
    for(int i = lo < 1 ? 1 : lo; i <= hi && i + 1 < NFFT / 2; i++) {
        if(m[i] > pk && m[i] >= m[i - 1] && m[i] >= m[i + 1]) { pk = m[i]; }   /* local maxima only */
    }
    if(pk == 0) { return 0.1f * mag_at(m, f) * 2.0f / s_wsum; }   /* only a skirt of a neighbour: weak */
    return 2.0f * pk / s_wsum;
}

void voc_sine_encode(voc_sine_enc_t *e, const int16_t pcm[VOC_FRAME], uint8_t *bits, int nbits)
{
    static float re[NFFT], im[NFFT], m[NFFT / 2];
    int gbits = nbits >= 88 ? 6 : 5;
    int budget = nbits - 8 - gbits;
    memset(bits, 0, (size_t)(nbits + 7) / 8);

    /* window = 96 previous + 160 new samples */
    for(int i = 0; i < NFFT; i++) { re[i] = 0; im[i] = 0; }
    for(int i = 0; i < 96; i++) { re[i] = e->hist[i] * s_win[i]; }
    for(int i = 0; i < VOC_FRAME; i++) { re[96 + i] = (float)pcm[i] / 32768.0f * s_win[96 + i]; }
    for(int i = 0; i < 96; i++) { e->hist[i] = (float)pcm[VOC_FRAME - 96 + i] / 32768.0f; }
    fft(re, im);
    float mx = 0;
    int pk = 0;
    for(int i = 0; i < NFFT / 2; i++) {
        m[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
        if(i > 2 && i < (int)(FMAX / FS * NFFT) && m[i] > mx) { mx = m[i]; pk = i; }
    }
    float peak_amp = 2.0f * mx / s_wsum;
    int pos = 0;
    if(peak_amp < 0.001f) {                                 /* < -60 dBFS: silence frame (gain 0) */
        put_bits(bits, &pos, 0, 8);
        put_bits(bits, &pos, 0, gbits);
        return;
    }
    /* dominant peak frequency, parabolic interpolation on log magnitude */
    float a = logf(m[pk - 1] + 1e-12f), b = logf(m[pk] + 1e-12f), c = logf(m[pk + 1] + 1e-12f);
    float dlt = 0.5f * (a - c) / (a - 2 * b + c + 1e-12f);
    float fpk = ((float)pk + dlt) * FS / NFFT;

    /* harmonic-sum pitch search on the IMBE grid */
    float score[208], best = 0;
    for(int b0 = 0; b0 < 208; b0++) {
        float f0 = f0_of(b0), s = 0;
        int L = nharm(f0);
        for(int k = 1; k <= L; k++) { float v = mag_at(m, f0 * (float)k); s += v * v; }
        score[b0] = s;
        if(s > best) { best = s; }
    }
    int bb = 0;
    float berr = 1e9f;
    for(int b0 = 0; b0 < 208; b0++) {                       /* among the near-best: exact hit of the peak */
        if(score[b0] < 0.9f * best) { continue; }
        float f0 = f0_of(b0);
        float k = floorf(fpk / f0 + 0.5f);
        float err = fabsf(k * f0 - fpk) - 1e-4f * f0;       /* tie: prefer the higher pitch */
        if(err < berr) { berr = err; bb = b0; }
    }
    float f0 = f0_of(bb);
    int L = nharm(f0);
    float gdb = 20.0f * log10f(peak_amp);
    int gq;
    if(gbits == 5) { gq = (int)floorf((gdb + 60.0f) / 2.0f + 1.5f); if(gq > 31) { gq = 31; } }
    else { gq = (int)floorf(gdb + 62.0f + 1.5f); if(gq > 63) { gq = 63; } }
    if(gq < 1) { gq = 1; }
    float gref = gbits == 5 ? -60.0f + (float)(gq - 1) * 2.0f : -62.0f + (float)(gq - 1);
    put_bits(bits, &pos, (uint32_t)bb, 8);
    put_bits(bits, &pos, (uint32_t)gq, gbits);
    for(int k = 0; k < L; k++) {
        int hb = hbits(k, L, budget);
        if(!hb) { continue; }
        float amp = amp_at(m, f0 * (float)(k + 1));
        float db = 20.0f * log10f(amp + 1e-9f) - gref;
        put_bits(bits, &pos, (uint32_t)quant_db(hb, db), hb);
    }
}

void voc_sine_decode(voc_sine_dec_t *d, const uint8_t *bits, int nbits, int16_t pcm[VOC_FRAME])
{
    int gbits = nbits >= 88 ? 6 : 5;
    int budget = nbits - 8 - gbits;
    int pos = 0;
    int b0 = (int)get_bits(bits, &pos, 8);
    int gq = (int)get_bits(bits, &pos, gbits);
    float amp[VOC_MAXH];
    memset(amp, 0, sizeof amp);
    float f0 = b0 < 208 ? f0_of(b0) : d->f0;
    if(gq && b0 < 208) {
        float gref = gbits == 5 ? -60.0f + (float)(gq - 1) * 2.0f : -62.0f + (float)(gq - 1);
        int L = nharm(f0);
        for(int k = 0; k < L; k++) {
            int hb = hbits(k, L, budget);
            if(!hb) { continue; }
            float db = level_db(hb, (int)get_bits(bits, &pos, hb));
            amp[k] = db < -150 ? 0 : powf(10.0f, (db + gref) / 20.0f);
        }
    }
    if(d->f0 <= 0) { d->f0 = f0; }
    float acc[VOC_FRAME];
    memset(acc, 0, sizeof acc);
    for(int k = 0; k < VOC_MAXH; k++) {
        float h = (float)(k + 1), ph = d->phase[k];
        if(d->amp[k] <= 0 && amp[k] <= 0) {                /* silent harmonic: just keep its phase going */
            ph += 2.0f * (float)M_PI * h * 0.5f * (d->f0 + f0) * (float)VOC_FRAME / FS;
        } else {
            for(int n = 0; n < VOC_FRAME; n++) {
                float t = (float)n / (float)VOC_FRAME;
                ph += 2.0f * (float)M_PI * h * (d->f0 + t * (f0 - d->f0)) / FS;   /* pitch glides across the frame */
                acc[n] += (d->amp[k] + t * (amp[k] - d->amp[k])) * cosf(ph);
            }
        }
        d->phase[k] = ph - 2.0f * (float)M_PI * floorf(ph / (2.0f * (float)M_PI));
    }
    for(int n = 0; n < VOC_FRAME; n++) {
        float v = acc[n] * 32767.0f;
        pcm[n] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    memcpy(d->amp, amp, sizeof amp);
    d->f0 = f0;
}
