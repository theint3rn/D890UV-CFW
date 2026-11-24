/*
 * imbe_dec.c - IMBE 7200x4400 parameter dequantisation, spectral enhancement and speech synthesis.
 *
 * imbe_unpack() and spectral_enhance() are adapted from mbelib (imbe7200x4400.c / mbelib.c):
 *
 *   Copyright (C) 2010 mbelib Author
 *   GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)
 *
 *   Permission to use, copy, modify, and/or distribute this software for any
 *   purpose with or without fee is hereby granted, provided that the above
 *   copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 *   REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 *   AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 *   INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 *   LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 *   OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *   PERFORMANCE OF THIS SOFTWARE.
 *
 * Changes for cfw: packed-bit input, compact tables, no heap/strtol/printf, the predictor state is
 * not modified in place. The synthesis (imbe_synth) and the FFT are new code for cfw (MCU friendly:
 * phasor oscillators instead of per-sample cosf, FFT noise for unvoiced bands).
 */
#include "imbe.h"
#include "imbe_tables.h"
#include <math.h>
#include <string.h>

#define PI_F  3.14159265358979f
#define TWOPI 6.28318530717959f
#define N     IMBE_FRAME

static unsigned bit_at(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7 - (i & 7))) & 1u; }

void imbe_params_init(imbe_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->w0 = 0.09378f;
    p->L = 30;
    p->K = 10;
}

unsigned imbe_b0(const uint8_t bits[IMBE_BYTES])
{
    unsigned b0 = 0;
    for (unsigned i = 0; i < 6; i++) b0 = (b0 << 1) | bit_at(bits, i);
    b0 = (b0 << 1) | bit_at(bits, 85);
    b0 = (b0 << 1) | bit_at(bits, 86);
    return b0;
}

float imbe_w0_of_b0(unsigned b0) { return 4.0f * PI_F / ((float)b0 + 39.5f); }

int imbe_l_of_w0(float w0) { return (int)(0.9254f * (float)(int)(PI_F / w0 + 0.25f)); }

int imbe_unpack(const uint8_t bits[IMBE_BYTES], const imbe_params_t *prev, imbe_params_t *cur)
{
    unsigned b0 = imbe_b0(bits);
    if (b0 > 207) return 1;                       /* 216..219 = silence, others invalid */
    float w0 = imbe_w0_of_b0(b0);
    int L = imbe_l_of_w0(w0);
    if (L < 9 || L > IMBE_MAXL) return 1;
    int L9 = L - 9, K = L < 37 ? (L + 2) / 3 : 12;

    /* b_m values (m = 1 .. L+1) from the 79 prioritised info bits */
    uint16_t bm[IMBE_MAXL + 3];
    memset(bm, 0, sizeof bm);
    for (unsigned i = 0; i < 79; i++)
        if (bit_at(bits, 6 + i)) bm[imbe_bo_m[L9][i]] |= (uint16_t)(1u << imbe_bo_b[L9][i]);

    cur->w0 = w0;
    cur->L = L;
    cur->K = K;
    /* V/UV: band 1 = MSB of b1, three harmonics per band, the last band takes the rest */
    for (int l = 1, k = K - 1, j = 1; l <= L; l++) {
        cur->vl[l] = (uint8_t)((bm[1] >> k) & 1u);
        if (j == 3) { j = 1; if (k > 0) k--; }
        else j++;
    }
    /* gain vector G1..G6 */
    float G[7], R[7], C[7][11], T[IMBE_MAXL + 2];
    G[1] = imbe_b2[bm[2] & 63];
    for (int i = 2; i <= 6; i++) {
        unsigned B = imbe_ba_bits[L9][i - 2];
        G[i] = imbe_ba_step[L9][i - 2] * ((float)bm[i + 1] - (float)(1u << (B - 1)) + 0.5f);
    }
    const float *b6 = imbe_dct_basis(6);
    for (int i = 1; i <= 6; i++) {               /* inverse 6-point DCT -> block means R_i */
        float s = 0;
        for (int m = 1; m <= 6; m++) s += (m == 1 ? 1.0f : 2.0f) * G[m] * b6[(m - 1) * 6 + (i - 1)];
        R[i] = s;
    }
    int m = 8;
    for (int i = 1; i <= 6; i++) {
        C[i][1] = R[i];
        for (int k = 2; k <= imbe_ji[L9][i - 1]; k++, m++) {
            unsigned B = imbe_hoba[L9][m - 8];
            C[i][k] = B ? imbe_quantstep[B - 1] * imbe_standdev[k - 2] *
                              ((float)bm[m] - (float)(1u << (B - 1)) + 0.5f)
                        : 0.0f;
        }
    }
    for (int i = 1, l = 1; i <= 6; i++) {         /* inverse DCT per block -> residuals T_l */
        int J = imbe_ji[L9][i - 1];
        const float *bj = imbe_dct_basis(J);
        for (int j = 1; j <= J; j++, l++) {
            float s = 0;
            for (int k = 1; k <= J; k++) s += (k == 1 ? 1.0f : 2.0f) * C[i][k] * bj[(k - 1) * J + (j - 1)];
            T[l] = s;
        }
    }
    /* prediction from the previous frame's log2 amplitudes */
    float rho = L <= 15 ? 0.4f : L <= 24 ? 0.03f * (float)L - 0.05f : 0.7f;
    float pl[IMBE_MAXL + 3];
    int pL = prev->L;
    memcpy(pl, prev->log2ml, sizeof prev->log2ml);
    pl[0] = prev->log2ml[0];
    for (int l = pL + 1; l <= IMBE_MAXL + 2; l++) pl[l] = prev->log2ml[pL];
    float kl[IMBE_MAXL + 2], dl[IMBE_MAXL + 2], sum77 = 0;
    int il[IMBE_MAXL + 2];
    for (int l = 1; l <= L; l++) {
        kl[l] = (float)pL / (float)L * (float)l;
        il[l] = (int)kl[l];
        dl[l] = kl[l] - (float)il[l];
        sum77 += (1.0f - dl[l]) * pl[il[l]] + dl[l] * pl[il[l] + 1];
    }
    sum77 *= rho / (float)L;
    for (int l = 1; l <= L; l++) {
        cur->log2ml[l] = T[l] + rho * (1.0f - dl[l]) * pl[il[l]] + rho * dl[l] * pl[il[l] + 1] - sum77;
        cur->ml[l] = exp2f(cur->log2ml[l]);
    }
    cur->log2ml[0] = 0;
    cur->ml[0] = 0;
    for (int l = L + 1; l <= IMBE_MAXL + 1; l++) { cur->ml[l] = 0; cur->log2ml[l] = 0; cur->vl[l] = 0; }
    return 0;
}

/* spectral amplitude enhancement (mbelib mbe_spectralAmpEnhance) */
static void spectral_enhance(imbe_params_t *p)
{
    float rm0 = 0, rm1 = 0, cw[IMBE_MAXL + 2];
    float c1 = cosf(p->w0), cp = 1.0f, cc = c1;
    for (int l = 1; l <= p->L; l++) {           /* cos(w0*l) by recurrence */
        cw[l] = cc;
        float cn = 2.0f * c1 * cc - cp;
        cp = cc;
        cc = cn;
    }
    for (int l = 1; l <= p->L; l++) {
        float m2 = p->ml[l] * p->ml[l];
        rm0 += m2;
        rm1 += m2 * cw[l];
    }
    if (rm0 <= 0) return;
    float r2m0 = rm0 * rm0, r2m1 = rm1 * rm1, den = p->w0 * rm0 * (r2m0 - r2m1);
    if (den <= 0) return;
    for (int l = 1; l <= p->L; l++) {
        if (p->ml[l] == 0 || 8 * l <= p->L) continue;
        float q = 0.96f * PI_F * (r2m0 + r2m1 - 2.0f * rm0 * rm1 * cw[l]) / den;
        float w = sqrtf(p->ml[l]) * sqrtf(sqrtf(q > 0 ? q : 0));
        if (w > 1.2f) w = 1.2f;
        else if (w < 0.5f) w = 0.5f;
        p->ml[l] *= w;
    }
    float s = 0;
    for (int l = 1; l <= p->L; l++) s += p->ml[l] * p->ml[l];
    float g = s > 0 ? sqrtf(rm0 / s) : 1.0f;
    for (int l = 1; l <= p->L; l++) p->ml[l] *= g;
}

/* ------------------------------------------------------------------ FFT (own) */
static float tw_c[256], tw_s[256];
static bool tw_ready;

static void tw_init(void)
{
    if (tw_ready) return;
    for (int i = 0; i < 256; i++) {
        tw_c[i] = cosf(TWOPI * (float)i / 512.0f);
        tw_s[i] = sinf(TWOPI * (float)i / 512.0f);
    }
    tw_ready = true;
}

float imbe_cos512(unsigned i) { tw_init(); i &= 511; return i < 256 ? tw_c[i] : -tw_c[i - 256]; }
float imbe_sin512(unsigned i) { tw_init(); i &= 511; return i < 256 ? tw_s[i] : -tw_s[i - 256]; }

static float dct_tab[650];                      /* J = 1..12: sum J^2 = 650 */
static bool  dct_ready;

const float *imbe_dct_basis(int J)
{
    if (!dct_ready) {
        float *t = dct_tab;
        for (int n = 1; n <= 12; n++)
            for (int k = 0; k < n; k++)
                for (int j = 0; j < n; j++) *t++ = cosf(PI_F * (float)k * ((float)j + 0.5f) / (float)n);
        dct_ready = true;
    }
    int off = 0;
    for (int n = 1; n < J; n++) off += n * n;
    return dct_tab + off;
}

/* in-place radix-2 FFT of n = 256 or 512 points (twiddles from the 512-point table) */
static void fft_n(float *re, float *im, int n, bool inverse)
{
    tw_init();
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    float sgn = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = 512 / len;
        for (int k = 0; k < half; k++) {
            float wr = tw_c[k * step], wi = sgn * tw_s[k * step];
            for (int i = k; i < n; i += len) {
                int b = i + half;
                float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[i] - xr; im[b] = im[i] - xi;
                re[i] += xr; im[i] += xi;
            }
        }
    }
}

void imbe_fft512(float *re, float *im, bool inverse) { fft_n(re, im, 512, inverse); }

void imbe_rfft512(const float *x, float *re, float *im)
{
    float zr[256], zi[256];
    for (int k = 0; k < 256; k++) { zr[k] = x[2 * k]; zi[k] = x[2 * k + 1]; }
    fft_n(zr, zi, 256, false);
    for (int k = 0; k <= 256; k++) {
        int a = k & 255, b = (256 - k) & 255;
        float er = 0.5f * (zr[a] + zr[b]), ei = 0.5f * (zi[a] - zi[b]);      /* E = (Z[k] + conj Z[-k]) / 2 */
        float or_ = 0.5f * (zi[a] + zi[b]), oi = -0.5f * (zr[a] - zr[b]);    /* O = (Z[k] - conj Z[-k]) / 2j */
        float wr = imbe_cos512((unsigned)k), wi = -imbe_sin512((unsigned)k); /* W^k = e^-j2pik/512 */
        re[k] = er + (or_ * wr - oi * wi);
        im[k] = ei + (or_ * wi + oi * wr);
    }
}

void imbe_irfft512(const float *re, const float *im, float *x)
{
    float zr[256], zi[256];
    for (int k = 0; k < 256; k++) {
        int b = 256 - k;
        float er = re[k] + re[b], ei = im[k] - im[b];                        /* 2E = X[k] + conj X[256-k] */
        float dr = re[k] - re[b], di = im[k] + im[b];                        /* 2 W^k O */
        float wr = imbe_cos512((unsigned)k), wi = imbe_sin512((unsigned)k);  /* W^-k */
        float or_ = dr * wr - di * wi, oi = dr * wi + di * wr;               /* 2O */
        zr[k] = er - oi;                                                     /* Z = 2E + j 2O */
        zi[k] = ei + or_;
    }
    fft_n(zr, zi, 256, true);
    for (int k = 0; k < 256; k++) { x[2 * k] = zr[k]; x[2 * k + 1] = zi[k]; }
}

/* ------------------------------------------------------------------ synthesis (own) */
static float rnd01(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static float wrap(float p)
{
    p = fmodf(p, TWOPI);
    return p < 0 ? p + TWOPI : p;
}

/* crossfade weight of the current frame's voiced part: 0 .. sample 55, linear to 1 at 105 */
#define XF0 56
#define XF1 105
static float xfade[N];
static bool  xf_ready;

static void xf_init(void)
{
    if (xf_ready) return;
    for (int n = 0; n < N; n++) xfade[n] = n <= 55 ? 0.0f : n >= 105 ? 1.0f : (float)(n - 55) / 50.0f;
    xf_ready = true;
}

/* cos/sin of a phase in [0, 2pi) from the 512-point table, linear interpolation (error < 2e-5) */
static void sincos_tab(float ph, float *c, float *s)
{
    float x = ph * (512.0f / TWOPI);
    unsigned i = (unsigned)x;
    float f = x - (float)i;
    float c0 = imbe_cos512(i), c1 = imbe_cos512(i + 1), s0 = imbe_sin512(i), s1 = imbe_sin512(i + 1);
    *c = c0 + f * (c1 - c0);
    *s = s0 + f * (s1 - s0);
}

/* add A * w(n) * cos(ph0 + rot*n) for the samples where the crossfade weight w is not 0;
 * (rr, ri) = e^{j*omega} */
static void osc_add(float *out, float A, float rr, float ri, float ph0, bool rising)
{
    float zr, zi;
    sincos_tab(wrap(ph0), &zr, &zi);
    if (rising) {
        /* advance the phasor to sample XF0 */
        float pr = rr, pi = ri, ar = 1.0f, ai = 0.0f;
        for (int e = XF0; e; e >>= 1) {                /* (rr + j ri)^XF0 by squaring */
            if (e & 1) { float t = ar * pr - ai * pi; ai = ar * pi + ai * pr; ar = t; }
            float t = pr * pr - pi * pi; pi = 2.0f * pr * pi; pr = t;
        }
        float t = zr * ar - zi * ai; zi = zr * ai + zi * ar; zr = t;
        for (int n = XF0; n < N; n++) {
            out[n] += A * xfade[n] * zr;
            t = zr * rr - zi * ri; zi = zr * ri + zi * rr; zr = t;
        }
    } else {
        for (int n = 0; n < XF1; n++) {
            out[n] += A * (1.0f - xfade[n]) * zr;
            float t = zr * rr - zi * ri; zi = zr * ri + zi * rr; zr = t;
        }
    }
}

void imbe_synth(imbe_dec_t *d, imbe_params_t *cur, int16_t pcm[IMBE_FRAME])
{
    imbe_params_t *prev = &d->syn;
    float v[N], re[257], im[257], xn[512];
    memset(v, 0, sizeof v);
    int maxl = cur->L > prev->L ? cur->L : prev->L;
    int nuv = 0;
    for (int l = 1; l <= cur->L; l++) nuv += !cur->vl[l];
    float pw0 = prev->w0, cw0 = cur->w0;
    float phi_cur[IMBE_MAXL + 2];
    for (int l = 1; l <= IMBE_MAXL; l++) {
        d->psi[l] = wrap(d->psi[l] + (pw0 + cw0) * (float)(l * N) * 0.5f);
        phi_cur[l] = l <= cur->L / 4 ? d->psi[l]
                                     : wrap(d->psi[l] + (float)nuv * (rnd01(&d->rng) * TWOPI - PI_F) / (float)cur->L);
    }
    /* voiced harmonics: previous frame fading out, current fading in. Per-harmonic rotations
     * e^{j*l*w0} by recurrence from e^{j*w0}. */
    xf_init();
    float pr1 = cosf(pw0), pi1 = sinf(pw0), cr1 = cosf(cw0), ci1 = sinf(cw0);
    float prr = 1, pri = 0, crr = 1, cri = 0;
    for (int l = 1; l <= maxl; l++) {
        float t = prr * pr1 - pri * pi1; pri = prr * pi1 + pri * pr1; prr = t;
        t = crr * cr1 - cri * ci1; cri = crr * ci1 + cri * cr1; crr = t;
        if (l <= prev->L && prev->vl[l] && prev->ml[l] > 0)
            osc_add(v, prev->ml[l], prr, pri, d->phi[l], false);
        if (l <= cur->L && cur->vl[l] && cur->ml[l] > 0)
            osc_add(v, cur->ml[l], crr, cri, phi_cur[l] - cw0 * (float)(l * N), true);
    }
    /* unvoiced bands: one random-phase noise grain per frame (320 samples, centred on the end of
     * this frame), sine window, overlap-add with the previous grain (power complementary) */
    memset(re, 0, sizeof re);
    memset(im, 0, sizeof im);
    bool any_uv = false;
    float binw = TWOPI / 512.0f;
    for (int l = 1; l <= cur->L; l++) {
        if (cur->vl[l] || cur->ml[l] <= 0) continue;
        int m0 = (int)ceilf(((float)l - 0.5f) * cw0 / binw), m1 = (int)ceilf(((float)l + 0.5f) * cw0 / binw) - 1;
        if (m0 < 1) m0 = 1;
        if (m1 > 255) m1 = 255;
        if (m1 < m0) continue;
        float a = 512.0f * IMBE_UV_GAIN * cur->ml[l] / (2.0f * sqrtf((float)(m1 - m0 + 1)));
        for (int m = m0; m <= m1; m++) {
            d->rng = d->rng * 1664525u + 1013904223u;
            unsigned th = d->rng >> 23;                /* random phase, 512 steps */
            re[m] = a * imbe_cos512(th); im[m] = a * imbe_sin512(th);
        }
        any_uv = true;
    }
    float uv[N];
    if (any_uv) {
        imbe_irfft512(re, im, xn);
        static float sw[N];                          /* sine window: sw[n] = sin(pi (n+0.5)/320) */
        static bool sw_ready;
        if (!sw_ready) { for (int n = 0; n < N; n++) sw[n] = sinf(PI_F * ((float)n + 0.5f) / 320.0f); sw_ready = true; }
        for (int n = 0; n < N; n++) {
            float w1 = sw[n], w2 = sw[N - 1 - n];
            uv[n] = d->noise_tail[n] + xn[n] * (1.0f / 512.0f) * w1;
            d->noise_tail[n] = xn[n + N] * (1.0f / 512.0f) * w2;
        }
    } else {
        memcpy(uv, d->noise_tail, sizeof uv);
        memset(d->noise_tail, 0, sizeof d->noise_tail);
    }
    for (int n = 0; n < N; n++) {
        float s = 7.0f * (v[n] + uv[n]);
        if (s > 32760.0f) s = 32760.0f;
        if (s < -32760.0f) s = -32760.0f;
        pcm[n] = (int16_t)lrintf(s);
    }
    for (int l = 1; l <= IMBE_MAXL; l++) d->phi[l] = phi_cur[l];
    *prev = *cur;
}

void imbe_dec_init(imbe_dec_t *d)
{
    uint32_t rng = d->rng ? d->rng : 0x1234567u;
    memset(d, 0, sizeof *d);
    imbe_params_init(&d->pred);
    imbe_params_init(&d->syn);
    for (int l = 0; l <= IMBE_MAXL + 1; l++) d->psi[l] = PI_F / 2.0f;
    d->rng = rng;
}

int imbe_decode(imbe_dec_t *d, const uint8_t bits[IMBE_BYTES], int errs, int16_t pcm[IMBE_FRAME])
{
    imbe_params_t cur;
    bool bad = errs < 0 || errs > 5 || imbe_unpack(bits, &d->pred, &cur) != 0;
    if (bad) {
        cur = d->pred;
        if (++d->repeat > 3) {
            memset(pcm, 0, IMBE_FRAME * sizeof pcm[0]);
            imbe_dec_init(d);
            return 2;
        }
    } else {
        d->repeat = 0;
    }
    d->pred = cur;
    imbe_params_t enh = cur;
    spectral_enhance(&enh);
    imbe_synth(d, &enh, pcm);
    return bad ? 1 : 0;
}
