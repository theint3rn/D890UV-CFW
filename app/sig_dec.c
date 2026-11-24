/* Analog signalling decoders on the 8 kHz FM audio tap (hal/dsp_backend.c fm_rx_poll, every sample, squelch
 * ignored; scale 8 counts per Hz of deviation). Which decoder runs follows the current channel (sig_chan()):
 *   DTMF    Optional Signal = DTMF: Goertzel on the 8 DTMF tones, 205-sample blocks (25.6 ms); a digit is taken
 *           after two equal blocks, the next one after a gap; the string ends after 0.5 s (slow senders: more) without a digit.
 *   5-tone  Optional Signal = 5Tone: frequency of a single tone from zero crossings (interpolated) in 10 ms blocks;
 *           a tone of >= Decode Time that matches the decode standard (5Tone Settings; not set = that of the
 *           channel's 5Tone ID entry; +-1.5 %) is a digit
 *           (repeat tone E = previous digit); the sequence ends after 100 ms without a tone (>= 3 digits).
 *   2-tone  2TONE Decode = entry number: that entry's 1st tone followed by its 2nd tone (each >= 300 ms, or 60 % of the
 *           2Tone Settings duration if that is shorter).
 *   MDC1200 Optional Signal = MDC1200 / QDC1200: FM discriminator around 1500 Hz, one bit integrated at 8 bit phases
 *           (> 0 = 1800 Hz = bit change), 40-bit sync 0x07092A446F with <= 3 bit errors (or inverted), then 112 bits
 *           -> sig_mdc.c. */
#include <string.h>
#include "sig.h"
#include "dsp_backend.h"

#define FS 8000
static uint8_t s_kind;

/* biquad (direct form I), coefficients for 8 kHz: b0 b1 b2 a1 a2 */
typedef struct { float x1, x2, y1, y2; } bq_t;
static float bq(bq_t *s, const float *c, float x)
{
    float y = c[0] * x + c[1] * s->x1 + c[2] * s->x2 - c[3] * s->y1 - c[4] * s->y2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}
static const float k_bp1500[5] = { 0.3159773f, 0.0f, -0.3159773f, -0.5235283f, 0.3680454f };   /* MDC: 1500 Hz, Q 1 */
static const float k_hp280[5] = { 0.8559295f, -1.7118590f, 0.8559295f, -1.6909942f, 0.7327238f }; /* tones: CTCSS out */
static bq_t s_bq_m, s_bq_t;

/* ------------------------------------------------------------------ DTMF */
#define DN 205
static const float k_dcoef[8] = {                               /* 2 cos(2 pi f / 8000) */
    1.7077378f, 1.6452810f, 1.5686869f, 1.4782046f, 1.1641010f, 0.9963702f, 0.7986184f, 0.5685327f };
static float s_q1[8], s_q2[8], s_den;
static int s_dn;
static char s_dlast, s_dprev, s_dstr[SIG_SEQ_MAX + 1];
static int s_dlen, s_dsil, s_dgap = 20;

static void dtmf_block(void)
{
    float p[8];
    for(int k = 0; k < 8; k++) {
        p[k] = s_q1[k] * s_q1[k] + s_q2[k] * s_q2[k] - k_dcoef[k] * s_q1[k] * s_q2[k];
        s_q1[k] = s_q2[k] = 0;
    }
    float e = s_den * (float)DN / 2.0f;                         /* single tone of amplitude A: p = e */
    s_den = 0;
    int r = 0, c = 4;
    for(int k = 1; k < 4; k++) { if(p[k] > p[r]) { r = k; } }
    for(int k = 5; k < 8; k++) { if(p[k] > p[c]) { c = k; } }
    char d = 0;
    if(e > 1.6e9f && p[r] > 0.15f * e && p[c] > 0.15f * e && p[r] + p[c] > 0.6f * e) {
        bool clean = true;
        for(int k = 0; k < 4; k++) { if(k != r && p[k] * 8.0f > p[r]) { clean = false; } }
        for(int k = 4; k < 8; k++) { if(k != c && p[k] * 8.0f > p[c]) { clean = false; } }
        if(clean) { d = "123A456B789C*0#D"[r * 4 + (c - 4)]; }
    }
    if(d && d == s_dprev && d != s_dlast) {                     /* two equal blocks, new key */
        if(s_dlen < SIG_SEQ_MAX) { s_dstr[s_dlen++] = d; s_dstr[s_dlen] = 0; }
        s_dlast = d;
        extern volatile uint32_t g_sig_dbg[8];
        g_sig_dbg[5]++;
    }
    if(!d) { s_dlast = 0; }
    s_dprev = d;
    if(s_dlen && !d) {
        if(++s_dsil >= s_dgap) {                                /* 0.5 s or more */
            sig_decoded(SIG_DTMF, s_dstr, 0, 0, 0, 0, NULL);
            s_dlen = 0; s_dstr[0] = 0; s_dsil = 0;
        }
    } else { s_dsil = 0; }
}

static void dtmf_sample(float x)
{
    for(int k = 0; k < 8; k++) { float q0 = k_dcoef[k] * s_q1[k] - s_q2[k] + x; s_q2[k] = s_q1[k]; s_q1[k] = q0; }
    s_den += x * x;
    if(++s_dn >= DN) { s_dn = 0; dtmf_block(); }
}

/* ------------------------------------------------------------------ single tone (2-tone, 5-tone) */
#define TB 80                                                   /* 10 ms */
static float s_tprev, s_tfirst, s_tlast, s_tpmin, s_tpmax, s_tpl, s_ten;
static int s_tn, s_tcross;
static float s_cur_f;                                           /* frequency of the running tone (0 = none) */
static int s_cur_n;                                             /* its length in blocks */
static bool s_cur_used;
static float s_t2_f1, s_t2_f2;
static int s_t2_state, s_t2_gap, s_t2_n1 = 30, s_t2_n2 = 30, s_t2_gmax = 30;
static char s_5str[SIG_SEQ_MAX + 1];
static int s_5len, s_5sil, s_5std, s_5need = 3;
static char s_5prev;

static bool near(float a, float b) { float d = a - b; if(d < 0) { d = -d; } return d < b * 0.015f + 4.0f; }

static void tone_end(float f, int n)                            /* a tone of n blocks ended */
{
    if(s_t2_f1 > 0) {
        if(s_t2_state == 0 && near(f, s_t2_f1) && n >= s_t2_n1) { s_t2_state = 1; s_t2_gap = 0; }
        else if(s_t2_state == 1 && s_t2_f2 > 0 && near(f, s_t2_f2) && n >= s_t2_n2) {
            sig_decoded(SIG_2TONE, "2TONE", 0, 0, 0, 0, NULL);
            s_t2_state = 0;
        } else if(s_t2_state == 1 && s_t2_f2 <= 0) {
            sig_decoded(SIG_2TONE, "2TONE", 0, 0, 0, 0, NULL);
            s_t2_state = 0;
        } else { s_t2_state = near(f, s_t2_f1) && n >= s_t2_n1 ? 1 : 0; }
    }
}

static void tone_digit(float f)                                 /* 5-tone: tone reached 30 ms */
{
    if(s_kind != SIG_5TONE) { return; }
    const uint16_t *t = sig_5t_tones(s_5std);
    for(int d = 0; d < 16; d++) {
        if(!t[d] || !near(f, (float)t[d])) { continue; }
        char c = d == 14 ? s_5prev : (char)(d < 10 ? '0' + d : 'A' + d - 10);
        if(c && s_5len < SIG_SEQ_MAX) { s_5str[s_5len++] = c; s_5str[s_5len] = 0; }
        s_5prev = d == 14 ? 0 : c;
        extern volatile uint32_t g_sig_dbg[8];
        g_sig_dbg[6]++;
        return;
    }
}

static void tone_block(void)
{
    float f = 0;
    float rms2 = s_ten / (float)TB;
    s_ten = 0;
    if(s_tcross >= 4 && rms2 > 4e5f) {                          /* > ~80 Hz rms deviation */
        float per = (s_tlast - s_tfirst) / (float)(s_tcross - 1);
        if(per > 0 && s_tpmax - s_tpmin < per * 0.35f) { f = (float)FS / per; }   /* steady period: one tone */
    }
    s_tcross = 0;
    s_tpmin = 1e9f; s_tpmax = 0;
    if(f > 0 && s_cur_f > 0 && near(f, s_cur_f)) {
        s_cur_n++;
        s_cur_f += (f - s_cur_f) * 0.2f;
        if(s_cur_n == s_5need && !s_cur_used) { tone_digit(s_cur_f); s_cur_used = true; }
    } else {
        if(s_cur_f > 0 && s_cur_n >= 3) { tone_end(s_cur_f, s_cur_n); }
        s_cur_f = f; s_cur_n = f > 0 ? 1 : 0; s_cur_used = false;
    }
    if(s_t2_state == 1 && s_cur_f <= 0 && ++s_t2_gap > s_t2_gmax) { s_t2_state = 0; }
    if(s_5len) {
        if(s_cur_f <= 0 || !s_cur_used) {
            if(++s_5sil >= 10) {                                /* 100 ms */
                if(s_5len >= 3) { sig_decoded(SIG_5TONE, s_5str, 0, 0, 0, 0, NULL); }
                s_5len = 0; s_5str[0] = 0; s_5sil = 0; s_5prev = 0;
            }
        } else { s_5sil = 0; }
    }
}

static void tone_sample(float x)
{
    x = bq(&s_bq_t, k_hp280, x);
    s_ten += x * x;
    if(s_tprev < 0 && x >= 0) {                                 /* rising zero crossing, interpolated */
        float t = (float)s_tn - 1.0f + (-s_tprev) / (x - s_tprev);
        if(s_tcross) {
            float p = t - s_tpl;
            if(p < s_tpmin) { s_tpmin = p; }
            if(p > s_tpmax) { s_tpmax = p; }
        } else { s_tfirst = t; }
        s_tpl = t;
        s_tlast = t;
        s_tcross++;
    }
    s_tprev = x;
    if(++s_tn >= TB) {
        s_tn = 0;
        s_tfirst -= (float)TB; s_tlast -= (float)TB; s_tpl -= (float)TB;
        tone_block();
    }
}

/* ------------------------------------------------------------------ MDC1200 */
/* FM discriminator: audio mixed to baseband at 1500 Hz (1200 Hz -> -300 Hz, 1800 Hz -> +300 Hz), two one-pole low
 * passes, phase step per sample Im(z[n] conj z[n-1]) / |z|^2; summed over one bit at MPH bit phases (integrate and
 * dump): > 0 = 1800 Hz = bit changed. */
#define MPH 8
static const uint64_t k_sync = 0x07092A446FULL;
static struct { float clk, acc; uint64_t sr; int state, nb; uint8_t bits[14]; bool inv; uint8_t prevb, op1, arg1; uint16_t unit1; } s_m[MPH];

static void mdc_event(uint8_t op, uint8_t arg, uint16_t unit, uint16_t target, const uint8_t *ext)
{
    char t[8];
    static const char hx[] = "0123456789ABCDEF";
    uint16_t u = unit ? unit : target;
    for(int i = 0; i < 4; i++) { t[i] = hx[(u >> (12 - 4 * i)) & 15]; }
    t[4] = 0;
    sig_decoded(SIG_MDC, t, op, arg, unit, target, ext);
}
static float s_mre, s_mim, s_mre2, s_mim2, s_mpr, s_mpi, s_mph;

static int popc64(uint64_t v) { int n = 0; while(v) { v &= v - 1; n++; } return n; }

static void mdc_bit(int k, int xorbit)
{
    uint8_t b = (uint8_t)(s_m[k].prevb ^ xorbit);               /* differential: 1800 Hz = bit changed */
    s_m[k].prevb = b;
    if(s_m[k].state == 0) {
        s_m[k].sr = (s_m[k].sr << 1 | b) & 0xFFFFFFFFFFULL;
        int e = popc64(s_m[k].sr ^ k_sync);
        if(e <= 3 || e >= 37) {
            s_m[k].state = 1; s_m[k].nb = 0; s_m[k].inv = e >= 37;
            extern volatile uint32_t g_sig_dbg[8];
            g_sig_dbg[4]++;
        }
        return;
    }
    int n = s_m[k].nb++;                                        /* 112 bits packed (RAM: 8 phases) */
    if(!n) { memset(s_m[k].bits, 0, sizeof s_m[k].bits); }
    if(b ^ (s_m[k].inv ? 1u : 0u)) { s_m[k].bits[n >> 3] |= (uint8_t)(1u << (n & 7)); }
    if(s_m[k].nb < 112) { return; }
    s_m[k].nb = 0;
    uint8_t bits[112];
    for(int i = 0; i < 112; i++) { bits[i] = (uint8_t)((s_m[k].bits[i >> 3] >> (i & 7)) & 1u); }
    uint8_t op, arg; uint16_t unit;
    bool ok = mdc_decode112(bits, &op, &arg, &unit);
    if(s_m[k].state == 1) {
        if(!ok) { s_m[k].state = 0; s_m[k].sr = 0; return; }
        for(int j = 0; j < MPH; j++) { if(j != k) { s_m[j].state = 0; s_m[j].sr = 0; } }   /* same packet */
        if(op == 0x35 || op == 0x55) {                         /* double packet: the second block follows */
            s_m[k].state = 2; s_m[k].op1 = op; s_m[k].arg1 = arg; s_m[k].unit1 = unit;
            return;
        }
        s_m[k].state = 0; s_m[k].sr = 0;
        mdc_event(op, arg, unit, 0, NULL);
        return;
    }
    s_m[k].state = 0; s_m[k].sr = 0;                           /* state 2: second block */
    if(ok) {
        uint8_t ext[4] = { op, arg, (uint8_t)(unit >> 8), (uint8_t)unit };
        mdc_event(s_m[k].op1, s_m[k].arg1, unit, s_m[k].unit1, ext);
    } else {
        mdc_event(s_m[k].op1, s_m[k].arg1, 0, s_m[k].unit1, NULL);   /* first block only: caller unknown */
    }
}

static void mdc_sample(float x)
{
    x = bq(&s_bq_m, k_bp1500, x);
    s_mph += 6.2831853f * 1500.0f / (float)FS;
    if(s_mph > 6.2831853f) { s_mph -= 6.2831853f; }
    float c = dsp_sinf(s_mph + 1.5707963f), sn = dsp_sinf(s_mph);
    s_mre += (x * c - s_mre) * 0.5f;  s_mim += (-x * sn - s_mim) * 0.5f;
    s_mre2 += (s_mre - s_mre2) * 0.5f; s_mim2 += (s_mim - s_mim2) * 0.5f;
    float re = s_mre2, im = s_mim2;
    float m2 = re * re + im * im;
    float d = m2 > 1.0f ? (im * s_mpr - re * s_mpi) / m2 : 0.0f;   /* Im(z conj zprev) / |z|^2 ~ phase step */
    s_mpr = re; s_mpi = im;
    for(int k = 0; k < MPH; k++) {
        s_m[k].acc += d;
        s_m[k].clk += 1200.0f / (float)FS;
        if(s_m[k].clk >= 1.0f) {
            s_m[k].clk -= 1.0f;
            mdc_bit(k, s_m[k].acc > 0 ? 1 : 0);
            s_m[k].acc = 0;
        }
    }
}

/* ------------------------------------------------------------------ dispatch */
void sig_rx_reset(void)
{
    memset(s_q1, 0, sizeof s_q1); memset(s_q2, 0, sizeof s_q2); s_den = 0; s_dn = 0;
    s_dlast = s_dprev = 0; s_dlen = 0; s_dstr[0] = 0; s_dsil = 0;
    s_tn = s_tcross = 0; s_cur_f = 0; s_cur_n = 0; s_t2_state = 0; s_5len = 0; s_5str[0] = 0; s_5prev = 0;
    s_tpmin = 1e9f; s_tpmax = 0; s_ten = 0;
    for(int k = 0; k < MPH; k++) { memset(&s_m[k], 0, sizeof s_m[k]); s_m[k].clk = (float)k / (float)MPH; }
    s_mre = s_mim = s_mre2 = s_mim2 = s_mpr = s_mpi = s_mph = 0;
    memset(&s_bq_m, 0, sizeof s_bq_m); memset(&s_bq_t, 0, sizeof s_bq_t);
    const sig_chan_t *c = sig_chan();
    s_kind = c->kind;
    s_t2_f1 = s_t2_f2 = 0;
    if(c->tone2_dec) {
        sig_2t_t e;
        for(int i = 0, n = sig_2t_count(); i < n; i++) {
            /* entry number = row + 1 (the stock lists are numbered 1..n in order) */
            if(i + 1 == c->tone2_dec && sig_2t(i, &e)) { s_t2_f1 = (float)e.f1; s_t2_f2 = (float)e.f2; }
        }
    }
    const sig_cfg_t *g = sig_cfg();
    s_5std = g->t5_std;
    s_5need = g->t5_dec;
    s_t2_n1 = g->t2_1 < 500 ? (g->t2_1 * 6 / 100 < 3 ? 3 : g->t2_1 * 6 / 100) : 30;
    s_t2_n2 = g->t2_2 < 500 ? (g->t2_2 * 6 / 100 < 3 ? 3 : g->t2_2 * 6 / 100) : 30;
    s_t2_gmax = 30 + g->t2_gap / 10;
    s_dgap = g->dt_ms >= 300 ? 10 + g->dt_ms * 5 / 128 : 20;   /* longer than the pause between two digits */
    if(s_kind == SIG_5TONE && !g->t5_self[0] && !g->t5_std) {   /* dialog not filled in: the channel entry's standard */
        sig_5t_t e;
        int n = sig_5t_count(), r = c->tone5_id >= 1 && c->tone5_id <= n ? c->tone5_id - 1 : 0;
        if(n && sig_5t(r, &e)) { s_5std = e.std; }
    }
}

void sig_rx_sample(int16_t xi)
{
    if(s_kind == SIG_OFF && s_t2_f1 <= 0) { return; }
    float x = (float)xi;
    switch(s_kind) {
    case SIG_DTMF: dtmf_sample(x); break;
    case SIG_MDC: mdc_sample(x); break;
    default: break;
    }
    if(s_kind == SIG_5TONE || s_t2_f1 > 0) { tone_sample(x); }
}
