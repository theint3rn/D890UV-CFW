/* cfw/dsp host test: proto test calls (cfw/build_streams/*.sym, made by the proto TX code) -> channel simulator
 * (chansim.c) -> dsp demodulator -> BER against the transmitted symbols and voice frames decoded by the real
 * proto glue (app/protos/*.c). Prints markdown tables (dsp/ber_table.md via `make test`).
 * Exit code 1 if a mode fails its pass criterion (BER < 1e-3 at 20 dB C/N with all impairments). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp_demod.h"
#include "chansim.h"
#include "modem.h"

extern const cfw_proto_t *const cfw_proto_reg_k_dmr, *const cfw_proto_reg_k_nxdn, *const cfw_proto_reg_k_p25p1,
    *const cfw_proto_reg_k_p25p2, *const cfw_proto_reg_k_dstar, *const cfw_proto_reg_k_ysf;

static uint32_t g_voice, g_now;
static uint32_t pnow(void) { return g_now; }
static void plog(cfw_log_level_t l, const char *m) { (void)l; (void)m; }
static void pvoice(cfw_vocoder_t v, const uint8_t *b, size_t n, int e) { (void)v; (void)b; (void)n; (void)e; g_voice++; }
static void pdata(uint8_t t, const uint8_t *d, size_t n) { (void)t; (void)d; (void)n; }
static void pcall(const cfw_call_info_t *c) { (void)c; }
static bool ptune(uint32_t rx, uint32_t tx) { (void)rx; (void)tx; return true; }
static bool preq(void) { return false; }
static bool pcp(uint32_t a, void *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
static const cfw_platform_t k_pf = { CFW_MODEM_API_VERSION, pnow, plog, pvoice, pdata, pcall, ptune, preq, pcp };

typedef struct { dsp_mode_t mode; const char *file; const cfw_proto_t *const *proto; } tcase_t;

static const tcase_t k_cases[] = {
    { DSP_MODE_DMR,    "dmr_call.sym",   &cfw_proto_reg_k_dmr },
    { DSP_MODE_P25P1,  "p25_call.sym",   &cfw_proto_reg_k_p25p1 },
    { DSP_MODE_YSF,    "ysf_call.sym",   &cfw_proto_reg_k_ysf },
    { DSP_MODE_NXDN48, "nxdn_call.sym",  &cfw_proto_reg_k_nxdn },
    { DSP_MODE_NXDN96, "nxdn_call.sym",  NULL },                 /* BER only (EFR stream not generated) */
    { DSP_MODE_DSTAR,  "dstar_call.sym", &cfw_proto_reg_k_dstar },
    { DSP_MODE_P25P2,  "p25p2_call.sym", &cfw_proto_reg_k_p25p2 },
};

static uint8_t g_ref[400000], g_out[400000];
static int8_t g_soft[400000];
static uint16_t g_adc[1600000];
static float g_f[1600000];

static size_t load(const char *dir, const char *name, uint8_t *buf, size_t max)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if(!f) { fprintf(stderr, "missing %s (run tools/make_streams.sh)\n", p); exit(2); }
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static int popc(unsigned v) { int c = 0; while(v) { c += v & 1; v >>= 1; } return c; }

/* bit errors between ref and out with block-wise re-alignment (out[j] ~ ref[j + off]) */
static double ber(const uint8_t *ref, size_t nref, const uint8_t *out, size_t nout, int bps, long *bits_out)
{
    long errs = 0, bits = 0;
    long off = 0;
    bool first = true;
    const size_t B = 256;
    for(size_t j0 = 800; j0 + B < nout; j0 += B) {      /* skip acquisition (first ~0.2-0.3 s) */
        long lo = first ? -600 : off - 3, hi = first ? 600 : off + 3, best = off;
        int bestm = -1;
        for(long o = lo; o <= hi; o++) {
            int m = 0;
            for(size_t j = j0; j < j0 + B; j++) {
                long i = (long)j + o;
                if(i >= 0 && i < (long)nref && ref[i] == out[j]) { m++; }
            }
            if(m > bestm) { bestm = m; best = o; }
        }
        if(first && bestm < (int)B / 3) { continue; }      /* not acquired yet */
        first = false;
        off = best;
        for(size_t j = j0; j < j0 + B; j++) {
            long i = (long)j + off;
            if(i < 0 || i >= (long)nref - 100) { continue; }
            errs += popc((unsigned)(ref[i] ^ out[j]) & (bps == 2 ? 3u : 1u));
            bits += bps;
        }
    }
    *bits_out = bits;
    return bits ? (double)errs / bits : 1.0;
}

static uint32_t decode_frames(const cfw_proto_t *p, const uint8_t *s, size_t n)
{
    cfw_chan_cfg_t cfg = { .rx_hz = 441000000, .tx_hz = 441000000, .my_id = 1, .color_code = 1, .slot = 1,
                           .nac = 0x293, .wacn = 0xBEE00, .sysid = 0x3C1, .ran = 1, .talkgroup = 91, .chan_step_hz = 12500 };
    g_voice = 0;
    p->init(p->ctx, &k_pf);
    p->rx_reset(p->ctx);
    if(p->configure) { p->configure(p->ctx, &cfg); }
    for(size_t i = 0; i < n; i += 64) {
        size_t k = n - i < 64 ? n - i : 64;
        p->rx_symbols(p->ctx, s + i, NULL, k);
        g_now = (uint32_t)(i * 1000ull / p->air.symbol_rate);
        if(p->tick) { p->tick(p->ctx, g_now); }
    }
    return g_voice;
}

static size_t demod(dsp_mode_t m, const uint16_t *adc, size_t n, float tau, int *quality)
{
    static dsp_demod_t d;
    dsp_opts_t o = { tau, false };
    dsp_init(&d, m, SIM_FS, &o);
    for(size_t i = 0; i < n; i++) { g_f[i] = (float)adc[i] - 2048.0f; }
    size_t k = dsp_process(&d, g_f, n, g_out, g_soft, sizeof g_out);
    if(quality) { *quality = dsp_quality(&d); }
    return k;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../build_streams";
    static const float cnr[] = { 6, 8, 10, 12, 14, 16, 20, 30 };
    const int ncnr = (int)(sizeof cnr / sizeof cnr[0]);
    int fails = 0;

    printf("### BER vs C/N (12.5 kHz), discriminator demod at 24 kHz\n\n");
    printf("Impairments in the `hard` rows: +1 kHz frequency offset, +100 ppm symbol clock, -10 %% deviation. "
           "Cell = bit error rate (after the first 800 symbols = acquisition) / voice frames decoded by the cfw proto glue, in %% of the frames decoded from the clean symbols (above 100 %% = a duplicate late-entry frame).\n\n");
    printf("| Mode | channel |");
    for(int c = 0; c < ncnr; c++) { printf(" %g dB |", cnr[c]); }
    printf("\n|---|---|");
    for(int c = 0; c < ncnr; c++) { printf("---|"); }
    printf("\n");

    for(size_t t = 0; t < sizeof k_cases / sizeof k_cases[0]; t++) {
        const tcase_t *tc = &k_cases[t];
        const dsp_mode_info_t *mi = dsp_mode_info(tc->mode);
        size_t nref = load(dir, tc->file, g_ref, sizeof g_ref);
        uint32_t clean = tc->proto ? decode_frames(*tc->proto, g_ref, nref) : 0;
        for(int hard = 0; hard < 2; hard++) {
            printf("| %s | %s |", mi->name, hard ? "hard" : "plain");
            for(int c = 0; c < ncnr; c++) {
                chan_t ch = { cnr[c], hard ? 1000.0f : 0.0f, hard ? 100.0f : 0.0f, hard ? -0.10f : 0.0f, 0, 1234u + (uint32_t)c };
                size_t na = chansim_run(tc->mode, g_ref, nref, &ch, g_adc, sizeof g_adc / sizeof g_adc[0]);
                size_t no = demod(tc->mode, g_adc, na, 0, NULL);
                long bits;
                double b = ber(g_ref, nref, g_out, no, mi->levels == 4 ? 2 : 1, &bits);
                if(tc->proto) {
                    uint32_t v = decode_frames(*tc->proto, g_out, no);
                    printf(" %.1e / %u%% |", b, clean ? (unsigned)(100u * v / clean) : 0u);
                } else {
                    printf(" %.1e |", b);
                }
                bool p2 = tc->mode == DSP_MODE_P25P2;        /* discriminator ISI floor: judged on decoded frames */
                if(hard && cnr[c] == 20 && !p2 && b > 1e-3) { fails++; fprintf(stderr, "FAIL %s BER %.2e at 20 dB hard\n", mi->name, b); }
                if(hard && cnr[c] == 20 && p2 && tc->proto) {
                    uint32_t v = decode_frames(*tc->proto, g_out, no);
                    if(v * 100u < clean * 95u) { fails++; fprintf(stderr, "FAIL P25P2 frames %u/%u at 20 dB hard\n", v, clean); }
                }
            }
            printf("\n");
        }
    }

    /* de-emphasis compensation */
    printf("\n### Audio path with 750 us de-emphasis (DMR, 20 dB, plain channel)\n\n| receiver | BER |\n|---|---|\n");
    {
        size_t nref = load(dir, "dmr_call.sym", g_ref, sizeof g_ref);
        chan_t ch = { 20, 0, 0, 0, 750, 99 };
        size_t na = chansim_run(DSP_MODE_DMR, g_ref, nref, &ch, g_adc, sizeof g_adc / sizeof g_adc[0]);
        long bits;
        size_t no = demod(DSP_MODE_DMR, g_adc, na, 0, NULL);
        printf("| no compensation | %.1e |\n", ber(g_ref, nref, g_out, no, 2, &bits));
        no = demod(DSP_MODE_DMR, g_adc, na, 750, NULL);
        double b = ber(g_ref, nref, g_out, no, 2, &bits);
        printf("| inverse de-emphasis 750 us | %.1e |\n", b);
        if(b > 1e-3) { fails++; fprintf(stderr, "FAIL de-emphasis compensation BER %.2e\n", b); }
    }

    /* H-DQPSK: discriminator vs coherent */
    printf("\n### P25 Phase 2 H-DQPSK: discriminator integrate-and-dump vs coherent differential detector\n\n"
           "Coherent reference: matched RRC filter on the complex signal, genie timing, ideal AFC (best case).\n\n"
           "| C/N | discriminator BER | coherent BER |\n|---|---|---|\n");
    {
        size_t nref = load(dir, "p25p2_call.sym", g_ref, sizeof g_ref);
        for(int c = 0; c < ncnr; c++) {
            chan_t ch = { cnr[c], 0, 0, 0, 0, 777u + (uint32_t)c };
            size_t na = chansim_run(DSP_MODE_P25P2, g_ref, nref, &ch, g_adc, sizeof g_adc / sizeof g_adc[0]);
            size_t no = demod(DSP_MODE_P25P2, g_adc, na, 0, NULL);
            long bits;
            double bd = ber(g_ref, nref, g_out, no, 2, &bits);
            size_t nc = chansim_dqpsk_coherent(g_ref, nref, &ch, g_out, sizeof g_out);
            long errs = 0, cb = 0;
            for(size_t i = 50; i + 50 < nc; i++) { errs += popc((unsigned)(g_ref[i] ^ g_out[i]) & 3u); cb += 2; }
            printf("| %g dB | %.1e | %.1e |\n", cnr[c], bd, cb ? (double)errs / cb : 1.0);
        }
    }

    /* CPU cost estimate */
    printf("\n### Work per second of signal (host-independent count)\n\n| Mode | MF taps | MAC/s at 24 kHz |\n|---|---|---|\n");
    for(int m = 0; m < DSP_MODE_COUNT; m++) {
        static dsp_demod_t d;
        dsp_init(&d, (dsp_mode_t)m, 48000, NULL);
        printf("| %s | %u | %u |\n", dsp_mode_info((dsp_mode_t)m)->name, d.ntaps, d.ntaps * 24000u);
    }
    fprintf(stderr, fails ? "dsp test: %d FAIL\n" : "dsp test: all pass\n", fails);
    return fails ? 1 : 0;
}
