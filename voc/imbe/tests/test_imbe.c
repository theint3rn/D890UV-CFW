/* IMBE vocoder host tests: mbelib dequantisation known answers, tone and speech-like round trips,
 * robustness, the P25 FEC chain, and a cycle estimate helper. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imbe.h"
#include "p25_p1.h"

static int fails, passes;
#define CHECK(c) do { if (c) passes++; else { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- mbelib known answers */
/* Chained decode of 48 pseudo-random frames (xorshift32 seed 0xC0FFEE, every 5th frame with b0 < 208
 * forced), reference values computed with mbelib 1.3.0 mbe_decodeImbe4400Parms:
 * {L, round(1000 * sum log2 M_l), voiced harmonics, round(1000 * log2 M_1)}; L = 0: invalid frame. */
static const int kat[48][4] = {
    {20, -15358, 12, -3545},
    {49, 365285, 31, 6917},
    {21, 650, 3, 1896},
    {17, 7787, 3, 382},
    {24, 208700, 9, 7665},
    {10, 23272, 3, 3092},
    {0, 0, 0, 0},
    {43, -115185, 18, -6570},
    {47, -58354, 21, -4394},
    {0, 0, 0, 0},
    {15, -43320, 9, -85},
    {12, 79773, 9, 2680},
    {0, 0, 0, 0},
    {37, -94036, 18, -7686},
    {0, 0, 0, 0},
    {18, 26702, 6, -1892},
    {20, 29958, 11, -1164},
    {19, -40887, 15, -5191},
    {39, 143601, 12, 3292},
    {0, 0, 0, 0},
    {15, 36403, 6, -1510},
    {13, 58857, 6, 3942},
    {35, -85233, 23, -2380},
    {41, -31369, 12, -1039},
    {34, -39979, 25, -587},
    {20, -35526, 6, -1577},
    {18, -22703, 9, -694},
    {12, 73052, 0, 6498},
    {12, 36917, 6, 1298},
    {0, 0, 0, 0},
    {14, 66840, 8, 929},
    {0, 0, 0, 0},
    {38, 224713, 3, 4197},
    {0, 0, 0, 0},
    {26, 67738, 14, -371},
    {21, 167156, 9, 2052},
    {0, 0, 0, 0},
    {36, 129063, 18, 100},
    {48, 86469, 24, 1051},
    {38, -92984, 26, -2489},
    {13, 60619, 9, 739},
    {13, 46069, 7, -262},
    {10, 23301, 4, 4224},
    {53, -151501, 38, -2496},
    {25, 74345, 10, 7500},
    {10, 71226, 7, 6761},
    {44, 10762, 35, -4869},
    {18, -20261, 15, -1988},
};
static unsigned ks = 0xC0FFEEu;
static unsigned krnd(void) { ks ^= ks << 13; ks ^= ks >> 17; ks ^= ks << 5; return ks; }

static void t_mbelib_kat(void)
{
    imbe_params_t prev, cur;
    imbe_params_init(&prev);
    int bad = 0;
    for (int t = 0; t < 48; t++) {
        uint8_t b[11];
        for (int i = 0; i < 11; i++) { unsigned v = krnd() & 0xFF; if (t % 5 == 0 && i == 0) v &= 0x3F; b[i] = (uint8_t)v; }
        int r = imbe_unpack(b, &prev, &cur);
        if (kat[t][0] == 0) { if (r == 0) bad++; continue; }
        if (r != 0 || cur.L != kat[t][0]) { bad++; continue; }
        double sm = 0; int nv = 0;
        for (int l = 1; l <= cur.L; l++) { sm += cur.log2ml[l]; nv += cur.vl[l]; }
        if (labs(lround(sm * 1000) - kat[t][1]) > 2 + cur.L / 4 || nv != kat[t][2] ||
            labs(lround(cur.log2ml[1] * 1000) - kat[t][3]) > 2) { bad++; printf("  kat %d: %ld %d %ld\n", t, lround(sm * 1000), nv, lround(cur.log2ml[1] * 1000)); }
        prev = cur;
    }
    CHECK(bad == 0);
}

/* ---------------------------------------------------------------- helpers */
#define MAXS (8000 * 6)
static int16_t sin_[MAXS], sout[MAXS];

static size_t codec_run(const int16_t *in, size_t n, int16_t *out, int *nvoiced_frames, unsigned *b0s)
{
    static imbe_enc_t e;
    static imbe_dec_t d;
    imbe_enc_init(&e);
    imbe_dec_init(&d);
    size_t fr = n / IMBE_FRAME;
    for (size_t f = 0; f < fr; f++) {
        uint8_t b[IMBE_BYTES];
        imbe_encode(&e, in + f * IMBE_FRAME, b);
        if (b0s) b0s[f] = imbe_b0(b);
        imbe_decode(&d, b, 0, out + f * IMBE_FRAME);
        if (nvoiced_frames) {
            int v = 0;
            for (int l = 1; l <= d.pred.L; l++) v += d.pred.vl[l];
            nvoiced_frames[f] = v * 100 / d.pred.L;     /* percent voiced harmonics */
        }
    }
    return fr * IMBE_FRAME;
}

/* best sine fit around f: returns SINAD (dB), sets fit frequency */
static double sinad(const int16_t *x, int n0, int n1, double f, double *fhat)
{
    double bestp = 0, bf = f;
    for (double ff = f - 6; ff <= f + 6; ff += 0.02) {
        double c = 0, s = 0;
        for (int n = n0; n < n1; n++) { c += x[n] * cos(2 * M_PI * ff * n / 8000); s += x[n] * sin(2 * M_PI * ff * n / 8000); }
        if (c * c + s * s > bestp) { bestp = c * c + s * s; bf = ff; }
    }
    double tot = 0, len = n1 - n0;
    for (int n = n0; n < n1; n++) tot += (double)x[n] * x[n];
    double sig = 2 * bestp / len;
    *fhat = bf;
    return 10 * log10(sig / (tot - sig + 1e-9));
}

static void t_tones(void)
{
    static const double fr[] = { 300, 440, 1011, 1500 };
    for (unsigned k = 0; k < sizeof fr / sizeof fr[0]; k++) {
        size_t n = 8000 * 2;
        for (size_t i = 0; i < n; i++) sin_[i] = (int16_t)(10000 * sin(2 * M_PI * fr[k] * i / 8000));
        codec_run(sin_, n, sout, NULL, NULL);
        double fh, s = sinad(sout, 4000, 16000, fr[k], &fh);
        double rms = 0;
        for (int i = 4000; i < 16000; i++) rms += (double)sout[i] * sout[i];
        rms = sqrt(rms / 12000);
        printf("  tone %6.1f Hz -> %7.2f Hz  SINAD %5.1f dB  level %+.1f dB\n", fr[k], fh, s, 20 * log10(rms / (10000 / sqrt(2))));
        CHECK(fabs(fh - fr[k]) < 5.0);          /* emu txrx_loop tolerance: 5 Hz */
        CHECK(s > 20);
        CHECK(fabs(20 * log10(rms / (10000 / sqrt(2)))) < 3);
    }
}

/* ---------------------------------------------------------------- speech-like signal */
typedef struct { double a1, a2, g; double y1, y2; } reso_t;
static void reso_init(reso_t *r, double f, double bw)
{
    double rr = exp(-M_PI * bw / 8000), th = 2 * M_PI * f / 8000;
    r->a1 = 2 * rr * cos(th); r->a2 = -rr * rr; r->g = 1 - rr; r->y1 = r->y2 = 0;
}
static double reso(reso_t *r, double x) { double y = r->g * x + r->a1 * r->y1 + r->a2 * r->y2; r->y2 = r->y1; r->y1 = y; return y; }

/* segments: vowel /a/ f0 110->150 Hz (0.6 s), fricative noise 2-4 kHz (0.4 s), vowel /i/ f0 210 Hz
 * (0.6 s), silence (0.2 s), vowel /u/ f0 95 Hz (0.6 s) */
static size_t make_speech(int16_t *x, int *kind)
{
    static double tmp[MAXS];
    size_t n = 0;
    uint32_t s = 1;
    double ph = 0, lp = 0;
    struct { int type; double dur, f0a, f0b, F[3], rms; } seg[] = {
        { 1, 0.6, 110, 150, { 700, 1220, 2600 }, 3000 }, { 2, 0.4, 0, 0, { 0 }, 1500 },
        { 1, 0.6, 210, 200, { 300, 2300, 3000 }, 3000 }, { 0, 0.2, 0, 0, { 0 }, 0 },
        { 1, 0.6, 95, 100, { 300, 870, 2240 }, 2500 } };
    for (unsigned g = 0; g < sizeof seg / sizeof seg[0]; g++) {
        reso_t r[3];
        for (int i = 0; i < 3; i++) reso_init(&r[i], seg[g].F[i] > 0 ? seg[g].F[i] : 1000, 60 + 30 * i);
        reso_t hp; reso_init(&hp, 3000, 1500);
        size_t len = (size_t)(seg[g].dur * 8000), n0 = n;
        double e = 0;
        for (size_t i = 0; i < len; i++, n++) {
            double y = 0;
            if (seg[g].type == 1) {
                double f0 = seg[g].f0a + (seg[g].f0b - seg[g].f0a) * (double)i / (double)len;
                ph += f0 / 8000;
                double ex = 0;
                if (ph >= 1) { ph -= 1; ex = 1; }
                lp = 0.9 * lp + ex;                       /* glottal pulse shaping */
                y = lp;
                for (int k = 0; k < 3; k++) y = reso(&r[k], y);
            } else if (seg[g].type == 2) {
                s = s * 1664525u + 1013904223u;
                y = reso(&hp, (double)(s >> 8) / 16777216.0 - 0.5);
            }
            tmp[n] = y;
            e += y * y;
            kind[n / IMBE_FRAME] = seg[g].type;
        }
        double gsc = e > 0 ? seg[g].rms / sqrt(e / (double)len) : 0;   /* segment level */
        for (size_t i = n0; i < n; i++) {
            double y = tmp[i] * gsc;
            x[i] = (int16_t)(y > 30000 ? 30000 : y < -30000 ? -30000 : y);
        }
    }
    return n;
}

/* band energies (16 bands of 250 Hz) of a 256-sample Hann frame starting at x */
static void bands(const int16_t *x, double *eb)
{
    for (int b = 0; b < 16; b++) eb[b] = 1e-3;
    for (int k = 1; k < 128; k++) {
        double c = 0, s = 0;
        for (int i = 0; i < 256; i++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / 256);
            c += w * x[i] * cos(2 * M_PI * k * i / 256);
            s += w * x[i] * sin(2 * M_PI * k * i / 256);
        }
        eb[k / 8] += c * c + s * s;
    }
}

static void t_speech(void)
{
    static int kind[MAXS / IMBE_FRAME + 1], vperc[MAXS / IMBE_FRAME + 1];
    static unsigned b0s[MAXS / IMBE_FRAME + 1];
    size_t n = make_speech(sin_, kind);
    size_t m = codec_run(sin_, n, sout, vperc, b0s);
    /* delay: find the lag with the best envelope match */
    int best_lag = 0;
    double best = -1;
    for (int lag = 0; lag <= 480; lag += 8) {
        double c = 0;
        for (size_t i = 0; i + lag < m; i += 4) c += fabs((double)sin_[i]) * fabs((double)sout[i + lag]);
        if (c > best) { best = c; best_lag = lag; }
    }
    double lsd_sum[3] = { 0 }; int cnt[3] = { 0 }, vok[3] = { 0 }, f0ok = 0, f0n = 0;
    for (size_t f = 2; (f + 2) * IMBE_FRAME + best_lag + 256 < m; f++) {
        size_t i0 = f * IMBE_FRAME;
        int k = kind[f];
        if (kind[f - 1] != k || kind[f + 1] != k || kind[f + 2] != k) continue;   /* skip transitions */
        double a[16], b[16], d = 0;
        bands(sin_ + i0, a);
        bands(sout + i0 + best_lag, b);
        double ea = 0, eb2 = 0;
        for (int j = 0; j < 16; j++) { ea += a[j]; eb2 += b[j]; }
        if (k == 0) { lsd_sum[0] += 10 * log10(eb2 / 256.0 + 1); cnt[0]++; continue; }
        int nb = 0;
        for (int j = 0; j < 15; j++) {
            if (a[j] < ea * 1e-4) continue;                                       /* ignore bands 40 dB down */
            double dd = 10 * log10(b[j] / a[j]);
            d += dd * dd; nb++;
        }
        lsd_sum[k] += sqrt(d / (nb ? nb : 1)); cnt[k]++;
        int vf = vperc[f + 1];                       /* encoder delay: frame f decoded one frame later */
        if ((k == 1 && vf >= 50) || (k == 2 && vf <= 30)) vok[k]++;
        if (k == 1) {
            double tf0 = 0;   /* not tracked exactly: check pitch stays inside the segment range */
            (void)tf0;
            double f0 = 4000.0 / M_PI * (4 * M_PI / (b0s[f + 1] + 39.5));
            f0n++;
            if (f0 > 85 && f0 < 230) f0ok++;
        }
    }
    printf("  speech: lag %d, LSD voiced %.1f dB (%d fr), unvoiced %.1f dB (%d fr), silence level %.1f dB\n",
           best_lag, lsd_sum[1] / cnt[1], cnt[1], lsd_sum[2] / cnt[2], cnt[2], lsd_sum[0] / (cnt[0] ? cnt[0] : 1));
    printf("  voicing: voiced frames ok %d/%d, unvoiced frames ok %d/%d, pitch in range %d/%d\n",
           vok[1], cnt[1], vok[2], cnt[2], f0ok, f0n);
    CHECK(lsd_sum[1] / cnt[1] < 6.0);
    CHECK(lsd_sum[2] / cnt[2] < 6.0);
    CHECK(vok[1] >= cnt[1] * 8 / 10);
    CHECK(vok[2] >= cnt[2] * 8 / 10);
    CHECK(f0ok >= f0n * 9 / 10);
    FILE *fp = fopen("build/speech_in.raw", "wb");
    if (fp) { fwrite(sin_, 2, n, fp); fclose(fp); }
    fp = fopen("build/speech_out.raw", "wb");
    if (fp) { fwrite(sout, 2, m, fp); fclose(fp); }
}

/* ---------------------------------------------------------------- robustness, repeats, FEC chain */
static void t_robust(void)
{
    static imbe_dec_t d;
    imbe_dec_init(&d);
    int16_t pcm[160];
    uint32_t s = 99;
    int bad = 0, maxabs = 0;
    for (int t = 0; t < 2000; t++) {
        uint8_t b[11];
        for (int i = 0; i < 11; i++) { s = s * 1103515245u + 12345u; b[i] = (uint8_t)(s >> 16); }
        imbe_decode(&d, b, 0, pcm);
        for (int i = 0; i < 160; i++) { int a = abs(pcm[i]); if (a > maxabs) maxabs = a; }
        for (int l = 0; l <= IMBE_MAXL; l++) if (!isfinite(d.syn.ml[l]) || !isfinite(d.psi[l])) bad++;
    }
    CHECK(bad == 0);
    CHECK(maxabs <= 32760);
    /* repeats: 3 repeated frames, then mute */
    uint8_t b[11] = { 0xFF, 0xFF };
    int r1 = imbe_decode(&d, b, 0, pcm), r2 = imbe_decode(&d, b, 9, pcm), r3 = imbe_decode(&d, b, -1, pcm);
    int r4 = imbe_decode(&d, b, 0, pcm);
    CHECK(r1 == 1 && r2 == 1 && r3 == 1 && r4 == 2);
    int z = 0;
    for (int i = 0; i < 160; i++) z |= pcm[i];
    CHECK(z == 0);
}

static void t_fec_chain(void)
{
    /* encode a tone, protect with the P25 FEC, add 3 errors per Golay word, decode: bits identical */
    static imbe_enc_t e;
    imbe_enc_init(&e);
    int16_t in[160];
    int bad = 0;
    for (int f = 0; f < 50; f++) {
        for (int i = 0; i < 160; i++) in[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * (f * 160 + i) / 8000));
        uint8_t b[11], air[18], back[11];
        imbe_encode(&e, in, b);
        p25_imbe_encode(b, air);
        for (int k = 0; k < 3; k++) { unsigned pos = p25_imbe_il((unsigned)(k * 7)); air[pos >> 3] ^= (uint8_t)(0x80 >> (pos & 7)); }
        int errs = p25_imbe_decode(air, back);
        if (memcmp(b, back, 11) || errs != 3) bad++;
    }
    CHECK(bad == 0);
}

/* ---------------------------------------------------------------- timing (host) */
#include <time.h>
static void t_timing(void)
{
    static imbe_enc_t e;
    static imbe_dec_t d;
    imbe_enc_init(&e);
    imbe_dec_init(&d);
    int kind[MAXS / IMBE_FRAME + 1];
    size_t n = make_speech(sin_, kind);
    clock_t c0 = clock();
    size_t fr = n / IMBE_FRAME;
    uint8_t b[11];
    for (size_t f = 0; f < fr; f++) imbe_encode(&e, sin_ + f * 160, b);
    clock_t c1 = clock();
    for (size_t f = 0; f < fr; f++) { imbe_encode(&e, sin_ + f * 160, b); }
    clock_t c2 = clock();
    imbe_enc_init(&e);
    for (size_t f = 0; f < fr; f++) { imbe_encode(&e, sin_ + f * 160, b); imbe_decode(&d, b, 0, sout); }
    clock_t c3 = clock();
    double enc = (double)(c2 - c1) / CLOCKS_PER_SEC / (double)fr * 1e6, both = (double)(c3 - c2) / CLOCKS_PER_SEC / (double)fr * 1e6;
    printf("  host time per 20 ms frame: encode %.0f us, decode %.0f us\n", enc, both - enc);
    (void)c0;
}

int main(void)
{
    t_mbelib_kat();
    t_tones();
    t_speech();
    t_robust();
    t_fec_chain();
    t_timing();
    printf("imbe: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
