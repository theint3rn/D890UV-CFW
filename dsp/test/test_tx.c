/* cfw digital TX end to end on the host, every mode:
 *   440 Hz sine -> test vocoder (hal/voice.c + voc/voc_sine.c) -> proto TX framing (app/protos glue + proto/*)
 *   -> cfw modulator (dsp_mod.c, 48 kHz FM baseband = what the DAC would output) -> channel simulator
 *   (FM, offset, clock error, deviation error, AWGN, IF filter, discriminator, ADC) -> cfw demodulator
 *   -> proto RX -> vocoder decode -> PCM -> tone frequency, SINAD, frame counts.
 * Prints a markdown table (dsp/tx_table.md via `make test`). Exit 1 if the 440 Hz tone is not recovered within
 * +/-2 Hz with SINAD >= 20 dB at 20 dB C/N with all impairments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp_demod.h"
#include "dsp_mod.h"
#include "chansim.h"
#include "modem.h"
#include "voice.h"
#include "tonemeter.h"

extern const cfw_proto_t *const cfw_proto_reg_k_dmr, *const cfw_proto_reg_k_nxdn, *const cfw_proto_reg_k_p25p1,
    *const cfw_proto_reg_k_p25p2, *const cfw_proto_reg_k_dstar, *const cfw_proto_reg_k_ysf;

static uint32_t g_ms, g_tx_frames, g_rx_frames, g_bad;
uint32_t hal_millis(void) { return g_ms; }

static uint32_t pnow(void) { return g_ms; }
static void plog(cfw_log_level_t l, const char *m) { (void)l; (void)m; }
static void pvoice_rx(cfw_vocoder_t v, const uint8_t *b, size_t n, int e) { g_rx_frames++; if(e < 0 || e >= 3) { g_bad++; if(getenv("TXDBG")) fprintf(stderr, "bad frame %u at %u ms errs %d\n", g_rx_frames, g_ms, e); } voice_rx_frame(v, b, n, e); }
static size_t pvoice_tx(cfw_vocoder_t v, uint8_t *b, size_t n) { g_tx_frames++; return voice_tx_frame(v, b, n); }
static void pdata(uint8_t t, const uint8_t *d, size_t n) { (void)t; (void)d; (void)n; }
static cfw_call_info_t g_ci;
static void pcall(const cfw_call_info_t *c) { g_ci = *c; }
static bool ptune(uint32_t rx, uint32_t tx) { (void)rx; (void)tx; return true; }
static bool preq(void) { return false; }
static bool pcp(uint32_t a, void *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
static const cfw_platform_t k_pf = { CFW_MODEM_API_VERSION, pnow, plog, pvoice_rx, pdata, pcall, ptune, preq, pcp, pvoice_tx };

typedef struct { const char *name; dsp_mode_t dm; const cfw_proto_t *const *proto; } tc_t;
static const tc_t k_tc[] = {
    { "DMR",    DSP_MODE_DMR,    &cfw_proto_reg_k_dmr },
    { "P25",    DSP_MODE_P25P1,  &cfw_proto_reg_k_p25p1 },
    { "P25P2",  DSP_MODE_P25P2,  &cfw_proto_reg_k_p25p2 },
    { "NXDN48", DSP_MODE_NXDN48, &cfw_proto_reg_k_nxdn },
    { "DSTAR",  DSP_MODE_DSTAR,  &cfw_proto_reg_k_dstar },
    { "YSF",    DSP_MODE_YSF,    &cfw_proto_reg_k_ysf },
};

static const cfw_proto_t *g_p;
static size_t pull(void *u, uint8_t *sym, size_t max) { (void)u; return g_p->tx_symbols(g_p->ctx, sym, max); }

static float g_hz[1200000];
static uint16_t g_adc[1200000];
static float g_f[4800];
static uint8_t g_sym[4800];
static int8_t g_soft[4800];
static int16_t g_pcm[200000];

static cfw_chan_cfg_t chan(void)
{
    cfw_chan_cfg_t c = { .rx_hz = 441000000, .tx_hz = 441000000, .my_id = 9990201, .talkgroup = 91, .color_code = 1,
                         .slot = 1, .nac = 0x293, .wacn = 0xBEE00, .sysid = 0x3C1, .ran = 1, .dgid = 0,
                         .chan_step_hz = 12500, .direct = true };
    strcpy(c.my_call, "N0CALL");
    strcpy(c.ur, "CQCQCQ");
    return c;
}

typedef struct { float freq, sinad; int good_pct; uint32_t tx, rx, bad; char call[40]; } res_t;

static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

static res_t run(const tc_t *t, const chan_t *ch, float tx_seconds)
{
    res_t r;
    memset(&r, 0, sizeof r);
    cfw_chan_cfg_t cfg = chan();
    /* ---- radio A: TX ---- */
    g_p = *t->proto;
    voice_init();
    voice_set_test_tone(440);
    g_tx_frames = g_rx_frames = g_bad = 0;
    g_p->init(g_p->ctx, &k_pf);
    g_p->rx_reset(g_p->ctx);
    g_p->configure(g_p->ctx, &cfg);
    cfw_tx_req_t req = { .voice = true, .dst_id = cfg.talkgroup, .slot = 1, .color_code = 1 };
    if(g_p->tx_start(g_p->ctx, &req) != 0) { return r; }
    static dsp_mod_t mod;
    dsp_mod_init(&mod, t->dm, 48000);
    size_t n = 0, stop_at = (size_t)(tx_seconds * 48000);
    for(int i = 0; i < 4800; i++) { g_hz[n++] = 0; }                     /* 0.1 s carrier first */
    while(n < sizeof g_hz / sizeof g_hz[0] - 480) {
        if(n >= stop_at && g_p->tx_stop) { g_p->tx_stop(g_p->ctx); stop_at = (size_t)-1; }
        size_t k = dsp_mod_run(&mod, g_hz + n, 480, pull, NULL);
        n += k;
        if(k < 480) { break; }
    }
    for(int i = 0; i < 4800; i++) { g_hz[n++] = 0; }
    r.tx = g_tx_frames;
    /* ---- channel ---- */
    size_t na = chansim_fm(g_hz, n, 48000, ch, g_adc, sizeof g_adc / sizeof g_adc[0]);
    /* ---- radio B: RX ---- */
    voice_meter_reset();
    memset(&g_ci, 0, sizeof g_ci);
    g_rx_frames = g_bad = 0;
    g_p->rx_reset(g_p->ctx);
    g_p->configure(g_p->ctx, &cfg);
    static dsp_demod_t d;
    dsp_init(&d, t->dm, 48000, NULL);
    size_t np = 0;
    uint32_t wr0 = g_emu_pcm_wr;
    for(size_t i = 0; i < na; i += 480) {
        size_t k = na - i < 480 ? na - i : 480;
        for(size_t j = 0; j < k; j++) { g_f[j] = (float)g_adc[i + j] - 2048.0f; }
        size_t ns = dsp_process(&d, g_f, k, g_sym, g_soft, sizeof g_sym);
        if(ns) { g_p->rx_symbols(g_p->ctx, g_sym, g_soft, ns); }
        g_ms = (uint32_t)((i + k) / 48);
        if(getenv("TXDBG") && t->dm == DSP_MODE_DMR && ch->cnr_db > 30 && (i / 480) % 10 == 0 && g_ms < 1500)
            fprintf(stderr, "  %u ms syncs %u locked %d gain %.1f dc %.1f evm %.3f quiet %u tint %.4f\n", g_ms, d.syncs, d.locked, d.gain, d.dc, d.evm, d.quiet, d.t_int);
        if(g_p->tick) { g_p->tick(g_p->ctx, g_ms); }
        while(wr0 != g_emu_pcm_wr && np < sizeof g_pcm / sizeof g_pcm[0]) { g_pcm[np++] = g_emu_pcm[wr0 % 8192u]; wr0++; }
    }
    r.rx = g_rx_frames;
    r.bad = g_bad;
    /* tone in 0.25 s blocks over the middle of the received audio: median frequency / SINAD, % blocks >= 20 dB */
    if(np > 8000) {
        float fq[64], sn[64];
        int nb = 0, good = 0;
        for(size_t o = 2400; o + 2000 + 2400 <= np && nb < 64; o += 2000, nb++) {
            tone_meas_t m = tone_measure(g_pcm + o, 2000, 8000.0f);
            fq[nb] = m.freq; sn[nb] = m.sinad_db;
            if(m.sinad_db >= 20.0f && fabsf(m.freq - 440.0f) < 2.0f) { good++; }
        }
        if(nb) {
            qsort(fq, (size_t)nb, sizeof fq[0], fcmp); qsort(sn, (size_t)nb, sizeof sn[0], fcmp);
            r.freq = fq[nb / 2]; r.sinad = sn[nb / 2] > 60 ? 60 : sn[nb / 2];
            r.good_pct = 100 * good / nb;
        }
    }
    char st[96];
    g_p->status(g_p->ctx, st, sizeof st);
    char *nl = strchr(st, '\n');
    if(nl) { *nl = 0; }
    snprintf(r.call, sizeof r.call, "%s", st);
    return r;
}

/* ---- modulator-only helpers ---- */
static uint8_t g_ref[60000];
static size_t g_ref_n, g_ref_rd;
static size_t ref_pull(void *u, uint8_t *sym, size_t max)
{
    (void)u;
    size_t k = g_ref_n - g_ref_rd < max ? g_ref_n - g_ref_rd : max;
    memcpy(sym, g_ref + g_ref_rd, k);
    g_ref_rd += k;
    return k;
}

static int popc(unsigned v) { int c = 0; while(v) { c += v & 1; v >>= 1; } return c; }

static double ber_align(const uint8_t *ref, size_t nref, const uint8_t *out, size_t nout, unsigned mask)
{
    long best = 0; int bm = -1;
    for(long o = -400; o <= 400; o++) {
        int m = 0;
        for(size_t j = 600; j < 900 && j < nout; j++) { long i = (long)j + o; if(i >= 0 && i < (long)nref && ref[i] == out[j]) { m++; } }
        if(m > bm) { bm = m; best = o; }
    }
    long e = 0, b = 0;
    for(size_t j = 600; j < nout; j++) {
        long i = (long)j + best;
        if(i < 0 || i >= (long)nref - 20) { continue; }
        e += popc((unsigned)(ref[i] ^ out[j]) & mask); b += popc(mask);
    }
    return b ? (double)e / b : 1.0;
}

/* in-place radix-2 FFT (host only) */
static void fft_d(double *re, double *im, int n)
{
    for(int i = 1, j = 0; i < n; i++) { int bt = n >> 1; for(; j & bt; bt >>= 1) { j ^= bt; } j |= bt;
        if(i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for(int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for(int i = 0; i < n; i += len) for(int k = 0; k < len / 2; k++) {
            double wr = cos(a * k), wi = sin(a * k);
            double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi, xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
            re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi; re[i + k] += xr; im[i + k] += xi;
        }
    }
}

static void mod_row(dsp_mode_t m, const char *pulse_name, int *fails)
{
    const dsp_mode_info_t *mi = dsp_mode_info(m);
    g_ref_n = mi->sym_rate * 4u;
    uint32_t x = 99u + (uint32_t)m;
    for(size_t k = 0; k < g_ref_n; k++) { x = x * 1103515245u + 12345u; g_ref[k] = (uint8_t)((x >> 16) & (mi->levels == 4 ? 3u : 1u)); }
    g_ref_rd = 0;
    static dsp_mod_t md;
    dsp_mod_init(&md, m, 48000);
    size_t n = 0;
    while(n < sizeof g_hz / sizeof g_hz[0] - 480) { size_t k = dsp_mod_run(&md, g_hz + n, 480, ref_pull, NULL); n += k; if(k < 480) { break; } }
    float pk = 0;
    for(size_t i = 4800; i + 4800 < n; i++) { if(fabsf(g_hz[i]) > pk) { pk = fabsf(g_hz[i]); } }
    /* 99 % power bandwidth of the FM signal (averaged 8192-point spectra) */
    enum { NF = 8192 };
    static double re[NF], im[NF], psd[NF];
    memset(psd, 0, sizeof psd);
    double ph = 0;
    size_t pos = 4800;
    int segs = 0;
    for(; pos + NF < n - 4800; pos += NF, segs++) {
        for(int i = 0; i < NF; i++) {
            ph += 2 * M_PI * g_hz[pos + i] / 48000.0;
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / NF);
            re[i] = cos(ph) * w; im[i] = sin(ph) * w;
        }
        fft_d(re, im, NF);
        for(int i = 0; i < NF; i++) { psd[i] += re[i] * re[i] + im[i] * im[i]; }
    }
    double tot = 0; for(int i = 0; i < NF; i++) { tot += psd[i]; }
    double acc = psd[0]; int bwbins = 0;                  /* grow symmetric around DC until 99 % */
    for(int k = 1; k < NF / 2 && acc < 0.99 * tot; k++) { acc += psd[k] + psd[NF - k]; bwbins = k; }
    double bw = 2.0 * bwbins * 48000.0 / NF / 1000.0;
    printf("| %s | %s | %.0f | %.1f |", mi->name, pulse_name, pk, bw);
    static const chan_t cs[3] = { { 30, 0, 0, 0, 0, 5 }, { 20, 1000, 100, -0.1f, 0, 6 }, { 14, 1000, 100, -0.1f, 0, 7 } };
    for(int c = 0; c < 3; c++) {
        size_t na = chansim_fm(g_hz, n, 48000, &cs[c], g_adc, sizeof g_adc / sizeof g_adc[0]);
        static dsp_demod_t d;
        dsp_init(&d, m, 48000, NULL);
        static uint8_t out[60000];
        static int8_t sf[60000];
        size_t no = 0;
        for(size_t i = 0; i < na; i += 480) {
            size_t k = na - i < 480 ? na - i : 480;
            for(size_t j = 0; j < k; j++) { g_f[j] = (float)g_adc[i + j] - 2048.0f; }
            no += dsp_process(&d, g_f, k, out + no, sf + no, sizeof out - no);
        }
        double b = ber_align(g_ref, g_ref_n, out, no, mi->levels == 4 ? 3u : 1u);
        printf(" %.1e |", b);
        if(c == 1 && m != DSP_MODE_P25P2 && b > 1e-3) { (*fails)++; fprintf(stderr, "FAIL modulator %s BER %.2e\n", mi->name, b); }
    }
    printf("\n");
}

int main(void)
{
    static const struct { const char *name; chan_t ch; } k_ch[] = {
        { "clean 40 dB",    { 40, 0, 0, 0, 0, 11 } },
        { "20 dB hard",     { 20, 1000, 100, -0.10f, 0, 12 } },
        { "14 dB hard",     { 14, 1000, 100, -0.10f, 0, 13 } },
        { "10 dB hard",     { 10, 1000, 100, -0.10f, 0, 14 } },
    };
    int fails = 0;
    printf("### Digital TX end to end: 440 Hz test tone, 3 s PTT\n\n"
           "Chain: 440 Hz -> cfw `sine` TEST vocoder (not interoperable) -> proto TX framing -> cfw modulator (48 kHz) -> "
           "channel (`hard` = +1 kHz offset, +100 ppm, -10 %% deviation) -> cfw demodulator -> proto RX -> vocoder -> PCM.\n"
           "Cell: median over 0.25 s blocks of the recovered tone Hz / SINAD dB (60+ = numerically perfect sine) / %% of blocks with 440+/-2 Hz and >= 20 dB / voice frames received of sent (concealed: FEC failed or >= 3 corrections). Codec alone: 439.86 Hz, 68 dB "
           "(the tone sits on the IMBE pitch grid: f0 = 16000/145.5, 4th harmonic).\n\n| Mode | decoded call |");
    for(size_t c = 0; c < sizeof k_ch / sizeof k_ch[0]; c++) { printf(" %s |", k_ch[c].name); }
    printf("\n|---|---|");
    for(size_t c = 0; c < sizeof k_ch / sizeof k_ch[0]; c++) { printf("---|"); }
    printf("\n");
    for(size_t t = 0; t < sizeof k_tc / sizeof k_tc[0]; t++) {
        char cells[4][64];
        char call[40] = "";
        for(size_t c = 0; c < sizeof k_ch / sizeof k_ch[0]; c++) {
            res_t r = run(&k_tc[t], &k_ch[c].ch, 3.0f);
            if(c == 0) { snprintf(call, sizeof call, "%s", r.call); }
            snprintf(cells[c], sizeof cells[c], "%.1f Hz / %.0f%s dB / %d %% / %u of %u (%u)", r.freq, r.sinad,
                     r.sinad >= 60 ? "+" : "", r.good_pct, r.rx, r.tx, r.bad);
            if(c == 1 && (fabsf(r.freq - 440.0f) > 2.0f || r.sinad < 20.0f || r.good_pct < 80)) {
                fails++;
                fprintf(stderr, "FAIL %s 20 dB hard: %.2f Hz %.1f dB\n", k_tc[t].name, r.freq, r.sinad);
            }
        }
        printf("| %s | %s |", k_tc[t].name, call);
        for(size_t c = 0; c < sizeof k_ch / sizeof k_ch[0]; c++) { printf(" %s |", cells[c]); }
        printf("\n");
    }
    /* ---- modulators alone: random symbols -> dsp_mod -> channel -> dsp_demod ---- */
    printf("\n### Modulators (cfw/dsp/dsp_mod.c): random symbols -> modulator -> channel -> cfw demodulator\n\n"
           "| Mode | frequency pulse | peak dev Hz | 99%% power BW kHz | BER 30 dB plain | BER 20 dB hard | BER 14 dB hard |\n"
           "|---|---|---|---|---|---|---|\n");
    static const dsp_mode_t mm[] = { DSP_MODE_DMR, DSP_MODE_P25P1, DSP_MODE_YSF, DSP_MODE_NXDN48, DSP_MODE_NXDN96,
                                     DSP_MODE_DSTAR, DSP_MODE_P25P2 };
    static const char *pn[] = { "RRC 0.2", "RC 0.2 x inverse sinc", "RC 0.2 x inverse sinc", "RRC 0.2", "RRC 0.2",
                                "Gaussian BT 0.5", "1RC, CPM h = 1/4" };
    for(size_t i = 0; i < sizeof mm / sizeof mm[0]; i++) {
        mod_row(mm[i], pn[i], &fails);
    }
    fprintf(stderr, fails ? "tx test: %d FAIL\n" : "tx test: all pass\n", fails);
    return fails ? 1 : 0;
}
