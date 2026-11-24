/*
 * imbe_enc.c - IMBE 7200x4400 encoder: speech analysis and parameter quantisation. Own code for cfw.
 *
 * Per 20 ms frame (one frame look-ahead):
 *   1. DC blocker, 384-sample history; analysis window = 281-sample Hamming centred on the frame.
 *   2. Pitch: normalised autocorrelation of the centre-clipped signal (periods 19.75..123.25 samples
 *      = the b0 grid), sub-multiple and continuity checks, parabolic refinement; then the b0 with the
 *      best harmonic fit within +-1 step. A single dominant tone is placed exactly on a harmonic.
 *   3. 512-point FFT. Per harmonic l: least-squares fit of the window spectrum W(m - l*f1) to the band
 *      (voiced amplitude = 2|c|) and the band energy (unvoiced amplitude = equivalent sinusoid).
 *      Voicing per band of 3 harmonics from the normalised fit error D_k against a threshold.
 *   4. Quantisation with the decoder's tables: residual T_l against the decoder's own predictor
 *      (closed loop: the encoder runs imbe_unpack on its own output), 6 block DCTs, 6-point DCT of
 *      the block means (G1 by codebook, G2..G6 uniform), higher-order coefficients uniform.
 */
#include "imbe.h"
#include "imbe_tables.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef IMBE_DEBUG
#include <stdio.h>
#endif

#define PI_F  3.14159265358979f
#define TWOPI 6.28318530717959f
#define NW    281                  /* analysis window */
#define WC    144                  /* window centre in buf[] (centre of the delayed frame) */
#define NFFT  512
#ifndef IMBE_ENC_RANGE
#define IMBE_ENC_RANGE 8.0f           /* log2 units = 48 dB (speech) */
#endif
#ifndef IMBE_ENC_RANGE_TONE
#define IMBE_ENC_RANGE_TONE 6.0f      /* 36 dB for a single tone: keeps the spike reachable */
#endif
#define WTAB_STEP 16               /* W(x) table: 1/16 bin */
#define WTAB_MAX  6                /* bins */

static float win[NW], wsum2, wtab[WTAB_MAX * WTAB_STEP + 2];
static bool  enc_ready;

static void enc_tables(void)
{
    if (enc_ready) return;
    wsum2 = 0;
    for (int i = 0; i < NW; i++) {
        win[i] = 0.54f - 0.46f * cosf(TWOPI * (float)i / (float)(NW - 1));
        wsum2 += win[i] * win[i];
    }
    for (int k = 0; k <= WTAB_MAX * WTAB_STEP + 1; k++) {
        float x = (float)k / (float)WTAB_STEP, s = 0;
        for (int t = -(NW / 2); t <= NW / 2; t++) s += win[t + NW / 2] * cosf(TWOPI * x * (float)t / (float)NFFT);
        wtab[k] = s;
    }
    enc_ready = true;
}

/* window spectrum (real, even) at an offset of x bins */
static float W(float x)
{
    x = fabsf(x) * (float)WTAB_STEP;
    if (x >= (float)(WTAB_MAX * WTAB_STEP)) return 0;
    int i = (int)x;
    float f = x - (float)i;
    return wtab[i] + f * (wtab[i + 1] - wtab[i]);
}

void imbe_enc_init(imbe_enc_t *e)
{
    enc_tables();
    memset(e, 0, sizeof *e);
    imbe_params_init(&e->pred);
}

/* ------------------------------------------------------------------ pitch */
#define TMIN 19
#define TMAX 124

/* returns pitch period in samples; *voicing = best normalised autocorrelation */
static float pitch_estimate(imbe_enc_t *e, const float *x, float *voicing)
{
    float c[NW], pk = 0;
    for (int i = 0; i < NW; i++) if (fabsf(x[i]) > pk) pk = fabsf(x[i]);
    float clip = 0.3f * pk;
    for (int i = 0; i < NW; i++) c[i] = x[i] > clip ? x[i] - clip : x[i] < -clip ? x[i] + clip : 0.0f;
    /* autocorrelation by FFT (linear: 281 + 125 < 512), energies by prefix sums */
    static float ax[512], are[257], aim[257];
    float pre[NW + 1], r[TMAX + 3];
    memset(ax, 0, sizeof ax);
    pre[0] = 0;
    for (int i = 0; i < NW; i++) { ax[i] = c[i]; pre[i + 1] = pre[i] + c[i] * c[i]; }
    imbe_rfft512(ax, are, aim);
    for (int i = 0; i <= 256; i++) { are[i] = are[i] * are[i] + aim[i] * aim[i]; aim[i] = 0; }
    imbe_irfft512(are, aim, ax);
    float *acr = ax;
    int best = TMIN;
    for (int t = TMIN - 1; t <= TMAX + 1; t++) {
        float e0 = pre[NW - t], e1 = pre[NW] - pre[t];
        r[t] = (e0 > 0 && e1 > 0) ? acr[t] * (1.0f / 512.0f) / sqrtf(e0 * e1) : 0.0f;
        if (t >= TMIN && t <= TMAX && r[t] > r[best]) best = t;
    }
    float rb = r[best];
    /* period multiples: prefer the shortest period that is nearly as periodic */
    for (int d = 4; d >= 2; d--) {
        int t = (int)lrintf((float)best / (float)d);
        if (t < TMIN) continue;
        int tb = t;
        for (int u = t - 1; u <= t + 1; u++) if (u >= TMIN && u <= TMAX && r[u] > r[tb]) tb = u;
        if (r[tb] >= 0.8f * rb) { best = tb; break; }
    }
    /* continuity with the previous frame */
    if (e->prev_period > 0) {
        int tp = (int)lrintf(e->prev_period);
        if (abs(tp - best) > 2 && tp >= TMIN && tp <= TMAX) {
            int tb = tp;
            for (int u = tp - 2; u <= tp + 2; u++) if (u >= TMIN && u <= TMAX && r[u] > r[tb]) tb = u;
            if (r[tb] >= 0.9f * r[best]) best = tb;
        }
    }
    float p = (float)best;
    float a = r[best - 1], b = r[best], cc = r[best + 1], den = a - 2.0f * b + cc;
    if (den < 0) p += 0.5f * (a - cc) / den;
    *voicing = r[best];
    return p;
}

/* ------------------------------------------------------------------ spectrum */
typedef struct {
    float re[NFFT / 2 + 1], im[NFFT / 2 + 1];
    float etot;                    /* energy of bins 1..255 */
} spec_t;

/* least-squares harmonic fit of band l; returns captured energy, fills amplitude/err/energy */
static float band_fit(const spec_t *s, float f1, int l, float *amp, float *err, float *eband)
{
    int m0 = (int)ceilf(((float)l - 0.5f) * f1), m1 = (int)ceilf(((float)l + 0.5f) * f1) - 1;
    if (m0 < 1) m0 = 1;
    if (m1 > 255) m1 = 255;
    float c = (float)l * f1, nr = 0, ni = 0, dw = 0, e = 0;
    for (int m = m0; m <= m1; m++) {
        float w = W((float)m - c);
        nr += s->re[m] * w;
        ni += s->im[m] * w;
        dw += w * w;
        e += s->re[m] * s->re[m] + s->im[m] * s->im[m];
    }
    float cap = dw > 0 ? (nr * nr + ni * ni) / dw : 0.0f;   /* |c|^2 * sum W^2 */
    if (amp) *amp = dw > 0 ? 2.0f * sqrtf(nr * nr + ni * ni) / dw : 0.0f;
    if (err) *err = e - cap > 0 ? e - cap : 0.0f;
    if (eband) *eband = e;
    return cap;
}

static float harmonic_capture(const spec_t *s, float w0)
{
    float f1 = w0 * (float)NFFT / TWOPI, cap = 0;
    int L = imbe_l_of_w0(w0);
    for (int l = 1; l <= L; l++) cap += band_fit(s, f1, l, NULL, NULL, NULL);
    return cap;
}

/* ------------------------------------------------------------------ quantiser */
static void put_bit(uint8_t *b, unsigned i, unsigned v)
{
    if (v) b[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
}

static unsigned uq(float v, float step, unsigned bits)   /* inverse of step*(b - 2^(B-1) + 0.5) */
{
    float q = floorf(v / step + (float)(1u << (bits - 1)));
    float mx = (float)((1u << bits) - 1);
    if (q < 0) q = 0;
    if (q > mx) q = mx;
    return (unsigned)q;
}

static void quantise(const imbe_params_t *prev, unsigned b0, const uint8_t *vband, const float *lm, uint8_t bits[IMBE_BYTES])
{
    float w0 = imbe_w0_of_b0(b0);
    int L = imbe_l_of_w0(w0), L9 = L - 9, K = L < 37 ? (L + 2) / 3 : 12;
    uint16_t bm[IMBE_MAXL + 3];
    memset(bm, 0, sizeof bm);
    for (int k = 0; k < K; k++) if (vband[k]) bm[1] |= (uint16_t)(1u << (K - 1 - k));

    /* residual against the decoder's prediction (same arithmetic as imbe_unpack) */
    float rho = L <= 15 ? 0.4f : L <= 24 ? 0.03f * (float)L - 0.05f : 0.7f;
    float pl[IMBE_MAXL + 3];
    int pL = prev->L;
    memcpy(pl, prev->log2ml, sizeof prev->log2ml);
    for (int l = pL + 1; l <= IMBE_MAXL + 2; l++) pl[l] = prev->log2ml[pL];
    float kl, dl[IMBE_MAXL + 2], sum77 = 0, T[IMBE_MAXL + 2];
    int il[IMBE_MAXL + 2];
    for (int l = 1; l <= L; l++) {
        kl = (float)pL / (float)L * (float)l;
        il[l] = (int)kl;
        dl[l] = kl - (float)il[l];
        sum77 += (1.0f - dl[l]) * pl[il[l]] + dl[l] * pl[il[l] + 1];
    }
    sum77 *= rho / (float)L;
    for (int l = 1; l <= L; l++)
        T[l] = lm[l] - rho * (1.0f - dl[l]) * pl[il[l]] - rho * dl[l] * pl[il[l] + 1] + sum77;

    /* block DCTs */
    float C[7][11], R[7], G[7];
    for (int i = 1, l = 1; i <= 6; i++) {
        int J = imbe_ji[L9][i - 1];
        const float *bj = imbe_dct_basis(J);
        for (int k = 1; k <= J; k++) {
            float s = 0;
            for (int j = 1; j <= J; j++) s += T[l + j - 1] * bj[(k - 1) * J + (j - 1)];
            C[i][k] = s / (float)J;
        }
        R[i] = C[i][1];
        l += J;
    }
    const float *b6 = imbe_dct_basis(6);
    for (int m = 1; m <= 6; m++) {
        float s = 0;
        for (int i = 1; i <= 6; i++) s += R[i] * b6[(m - 1) * 6 + (i - 1)];
        G[m] = s / 6.0f;
    }
    /* G1: codebook */
    unsigned b2 = 0;
    for (unsigned i = 1; i < 64; i++) if (fabsf(imbe_b2[i] - G[1]) < fabsf(imbe_b2[b2] - G[1])) b2 = i;
    bm[2] = (uint16_t)b2;
    for (int i = 2; i <= 6; i++) bm[i + 1] = (uint16_t)uq(G[i], imbe_ba_step[L9][i - 2], imbe_ba_bits[L9][i - 2]);
    int m = 8;
    for (int i = 1; i <= 6; i++)
        for (int k = 2; k <= imbe_ji[L9][i - 1]; k++, m++) {
            unsigned B = imbe_hoba[L9][m - 8];
            if (B) bm[m] = (uint16_t)uq(C[i][k], imbe_quantstep[B - 1] * imbe_standdev[k - 2], B);
        }
    /* pack */
    memset(bits, 0, IMBE_BYTES);
    for (unsigned i = 0; i < 6; i++) put_bit(bits, i, (b0 >> (7 - i)) & 1u);
    put_bit(bits, 85, (b0 >> 1) & 1u);
    put_bit(bits, 86, b0 & 1u);
    for (unsigned i = 0; i < 79; i++) put_bit(bits, 6 + i, (bm[imbe_bo_m[L9][i]] >> imbe_bo_b[L9][i]) & 1u);
}

/* ------------------------------------------------------------------ encoder */
static unsigned b0_of_period(float p)
{
    float b = 2.0f * p - 39.5f;
    if (b < 0) b = 0;
    if (b > 207) b = 207;
    return (unsigned)lrintf(b);
}

void imbe_encode(imbe_enc_t *e, const int16_t pcm[IMBE_FRAME], uint8_t bits[IMBE_BYTES])
{
    enc_tables();
    memmove(e->buf, e->buf + IMBE_FRAME, (IMBE_ENC_BUF - IMBE_FRAME) * sizeof(float));
    for (int i = 0; i < IMBE_FRAME; i++) {                 /* DC blocker */
        float x = (float)pcm[i];
        e->hp_y = x - e->hp_x + 0.995f * e->hp_y;
        e->hp_x = x;
        e->buf[IMBE_ENC_BUF - IMBE_FRAME + i] = e->hp_y;
    }
    e->frames++;
    const float *x = e->buf + WC - NW / 2;

    /* spectrum of the windowed frame, window centre at FFT index 0 (real window spectrum) */
    static spec_t s;
    static float xw[NFFT];
    memset(xw, 0, sizeof xw);
    for (int t = -(NW / 2); t <= NW / 2; t++) xw[(t + NFFT) % NFFT] = x[t + NW / 2] * win[t + NW / 2];
    imbe_rfft512(xw, s.re, s.im);
    float etot = 0, pmax = 0;
    int mmax = 1;
    for (int m = 1; m < 256; m++) {
        float p = s.re[m] * s.re[m] + s.im[m] * s.im[m];
        etot += p;
        if (p > pmax) { pmax = p; mmax = m; }
    }

    float voicing;
    float period = pitch_estimate(e, x, &voicing);
    if (voicing < 0.25f && e->prev_period > 0) period = e->prev_period;
    unsigned b0 = b0_of_period(period);
    /* fine search on the harmonic fit */
    {
        unsigned bb = b0;
        float best = -1;
        for (int d = -1; d <= 1; d++) {
            int c = (int)b0 + d;
            if (c < 0 || c > 207) continue;
            float cap = harmonic_capture(&s, imbe_w0_of_b0((unsigned)c));
            if (cap > best * 1.0001f) { best = cap; bb = (unsigned)c; }
        }
        b0 = bb;
    }
    /* one dominant tone (test tones, carriers): put it exactly on a harmonic */
    bool tone = false;
    if (mmax > 2 && mmax < 254) {
        float pk = 0;
        for (int m = mmax - 3; m <= mmax + 3; m++) pk += s.re[m] * s.re[m] + s.im[m] * s.im[m];
        if (etot > 0 && pk > 0.9f * etot) {
            tone = true;
            float a = sqrtf(s.re[mmax - 1] * s.re[mmax - 1] + s.im[mmax - 1] * s.im[mmax - 1]);
            float b = sqrtf(pmax);
            float c = sqrtf(s.re[mmax + 1] * s.re[mmax + 1] + s.im[mmax + 1] * s.im[mmax + 1]);
            float den = a - 2.0f * b + c, fpk = (float)mmax + (den < 0 ? 0.5f * (a - c) / den : 0.0f);
            /* cost = frequency error (Hz) + 1 Hz per harmonic number: a low harmonic keeps the
             * spike in a short residual block, which the amplitude quantiser can represent */
            float wpk = fpk * TWOPI / (float)NFFT, bcost = 1e9f;
            for (unsigned c0 = 0; c0 <= 207; c0++) {
                float w = imbe_w0_of_b0(c0);
                int k = (int)lrintf(wpk / w);
                if (k < 1 || k > 16) continue;
                float cost = fabsf((float)k * w - wpk) * 4000.0f / PI_F + (float)(k - 1);
                if (cost < bcost) { bcost = cost; b0 = c0; }
            }
        }
    }
    float w0 = imbe_w0_of_b0(b0);
    int L = imbe_l_of_w0(w0), K = L < 37 ? (L + 2) / 3 : 12;
    e->prev_period = voicing >= 0.25f ? (float)(b0 + 39.5f) * 0.5f : e->prev_period;

    /* amplitudes and voicing */
    float f1 = w0 * (float)NFFT / TWOPI;
    float amp_v[IMBE_MAXL + 2], amp_u[IMBE_MAXL + 2], berr[IMBE_MAXL + 2], ben[IMBE_MAXL + 2];
    for (int l = 1; l <= L; l++) {
        band_fit(&s, f1, l, &amp_v[l], &berr[l], &ben[l]);
        amp_u[l] = sqrtf(4.0f * ben[l] / ((float)NFFT * wsum2)) / IMBE_UV_GAIN;
    }
    uint8_t vband[13] = { 0 };
    float lm[IMBE_MAXL + 2], emax = 0, eband[13];
    for (int k = 0; k < K; k++) {
        int l0 = 3 * k + 1, l1 = k == K - 1 ? L : 3 * k + 3;
        eband[k] = 0;
        for (int l = l0; l <= l1; l++) eband[k] += ben[l];
        eband[k] /= (float)(l1 - l0 + 1);
        if (eband[k] > emax) emax = eband[k];
    }
    for (int k = 0; k < K; k++) {
        int l0 = 3 * k + 1, l1 = k == K - 1 ? L : 3 * k + 3;
        float er = 0, en = 0;
        for (int l = l0; l <= l1; l++) { er += berr[l]; en += ben[l]; }
        float D = en > 0 ? er / en : 1.0f;
        float fk = (float)(l0 + l1) * 0.5f * w0 * 4000.0f / PI_F;   /* band centre, Hz */
        /* threshold: lower at high frequency, higher for a strongly periodic frame (pitch glides
         * smear the upper harmonics), hysteresis on the previous decision */
        float th = (0.38f - 0.22f * fk / 4000.0f) * (voicing < 0.3f ? 0.5f : 0.6f + 0.8f * voicing);
        if (e->prev_vband[k]) th += 0.06f;
        bool v = D < th;
        /* weak bands (30 dB below the strongest) follow the frame's periodicity */
        if (eband[k] < 1e-3f * emax) v = voicing > 0.6f;
        vband[k] = tone || v;        /* tone: all voiced, so the decoder adds no phase dispersion */
        for (int l = l0; l <= l1; l++) {
            float a = vband[k] ? amp_v[l] : amp_u[l];
            float mlin = a / 7.0f;
            lm[l] = log2f(mlin > 1e-3f ? mlin : 1e-3f);
        }
    }
    memcpy(e->prev_vband, vband, sizeof vband);
    /* limit the per-frame dynamic range: the residual quantiser cannot follow a deep null next to a
     * strong harmonic, and an unreachable target only spreads the error over the whole block */
    {
        float mx = -1e9f;
        for (int l = 1; l <= L; l++) if (lm[l] > mx) mx = lm[l];
        float range = tone ? IMBE_ENC_RANGE_TONE : IMBE_ENC_RANGE;
        for (int l = 1; l <= L; l++) if (lm[l] < mx - range) lm[l] = mx - range;
    }

#ifdef IMBE_DEBUG
    if (e->frames == 20) { for (int l = 1; l <= L; l++) printf("l%d tgt %.2f ", l, lm[l]); printf("\n"); }
#endif
    quantise(&e->pred, b0, vband, lm, bits);
    imbe_params_t cur;
    if (imbe_unpack(bits, &e->pred, &cur) == 0) e->pred = cur;   /* decoder mirror (closed loop) */
}
