/* Host: decode a captured / streamed .adc16 file (48 kHz) with the cfw demodulator + proto glue + test vocoder.
 *   capdecode MODE FILE [out.pcm]      MODE: dmr p25 p25p2 nxdn dstar ysf
 * Prints the decoded call, frames, per-0.25 s tone frequency / SINAD. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp_demod.h"
#include "modem.h"
#include "voice.h"
#include "tonemeter.h"
extern const cfw_proto_t *const cfw_proto_reg_k_dmr, *const cfw_proto_reg_k_nxdn, *const cfw_proto_reg_k_p25p1,
    *const cfw_proto_reg_k_p25p2, *const cfw_proto_reg_k_dstar, *const cfw_proto_reg_k_ysf;
static uint32_t g_ms, g_rx, g_bad;
uint32_t hal_millis(void) { return g_ms; }
static uint32_t pnow(void) { return g_ms; }
static void plog(cfw_log_level_t l, const char *m) { (void)l; (void)m; }
static void prx(cfw_vocoder_t v, const uint8_t *b, size_t n, int e) { g_rx++; if(getenv("BITS") && g_rx < 80) { printf("  f%u %.2fs b0=%u g=%u bytes", g_rx, g_ms/1000.0, b[0], b[1] >> 3); for(int i = 0; i < 7; i++) printf(" %02x", b[i]); printf("\n"); } if(e < 0 || e >= 3) { g_bad++; printf("  frame %u at %.2fs errs %d\n", g_rx, g_ms / 1000.0, e); } voice_rx_frame(v, b, n, e); }
static void pdata(uint8_t t, const uint8_t *d, size_t n) { (void)t; (void)d; (void)n; }
static void pcall(const cfw_call_info_t *c) { printf("  call at %.2fs: src %u dst %u %s>%s voice %d\n", g_ms / 1000.0, c->src_id, c->dst_id, c->src_call, c->dst_call, c->voice); }
static bool ptune(uint32_t a, uint32_t b) { (void)a; (void)b; return true; }
static bool preq(void) { return false; }
static bool pcp(uint32_t a, void *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
static size_t ptx(cfw_vocoder_t v, uint8_t *b, size_t n) { (void)v; (void)b; (void)n; return 0; }
static const cfw_platform_t k_pf = { CFW_MODEM_API_VERSION, pnow, plog, prx, pdata, pcall, ptune, preq, pcp, ptx };
static uint16_t adc[3000000]; static float f[480]; static uint8_t sym[256]; static int8_t sf[256]; static int16_t pcm[400000];
int main(int argc, char **argv)
{
    if(argc < 3) { return 1; }
    const char *m = argv[1];
    const cfw_proto_t *p = !strcmp(m, "dmr") ? cfw_proto_reg_k_dmr : !strcmp(m, "nxdn") ? cfw_proto_reg_k_nxdn :
        !strcmp(m, "p25") ? cfw_proto_reg_k_p25p1 : !strcmp(m, "p25p2") ? cfw_proto_reg_k_p25p2 :
        !strcmp(m, "dstar") ? cfw_proto_reg_k_dstar : cfw_proto_reg_k_ysf;
    dsp_mode_t dm = !strcmp(m, "dmr") ? DSP_MODE_DMR : !strcmp(m, "nxdn") ? DSP_MODE_NXDN48 : !strcmp(m, "p25") ? DSP_MODE_P25P1 :
        !strcmp(m, "p25p2") ? DSP_MODE_P25P2 : !strcmp(m, "dstar") ? DSP_MODE_DSTAR : DSP_MODE_YSF;
    size_t pad = getenv("PAD") ? (size_t)atol(getenv("PAD")) : 0;      /* leading idle samples (start-offset test) */
    for(size_t i = 0; i < pad; i++) { adc[i] = (uint16_t)(2048 + (int)((i * 2654435761u >> 28) & 7) - 4); }
    FILE *fp = fopen(argv[2], "rb"); size_t n = pad + fread(adc + pad, 2, sizeof adc / 2 - pad, fp); fclose(fp);
    cfw_chan_cfg_t cfg = { .rx_hz = 441000000, .my_id = 1, .talkgroup = 1, .color_code = 1, .slot = 1, .nac = 0x293,
                           .wacn = 0xBEE00, .sysid = 0x3C1, .ran = 1, .chan_step_hz = 12500, .direct = getenv("DIRECT") != NULL };
    voice_init();
    p->init(p->ctx, &k_pf); p->rx_reset(p->ctx); p->configure(p->ctx, &cfg);
    static dsp_demod_t d; dsp_init(&d, dm, 48000, NULL);
    size_t np = 0; uint32_t wr = g_emu_pcm_wr;
    for(size_t i = 0; i < n; i += 480) {
        size_t k = n - i < 480 ? n - i : 480;
        for(size_t j = 0; j < k; j++) { f[j] = (float)adc[i + j] - 2048.0f; }
        size_t ns = dsp_process(&d, f, k, sym, sf, sizeof sym);
        g_ms = (uint32_t)((i + k) / 48);
        if(ns) { p->rx_symbols(p->ctx, sym, sf, ns); if(getenv("SYMOUT")) { static FILE *so; if(!so) so = fopen(getenv("SYMOUT"), "wb"); fwrite(sym, 1, ns, so); fflush(so); } }
        if(getenv("TPH") && i % 2400 == 0) { double ph = fmod(d.t_next - (double)pad / d.dec, d.sps); printf("  t %.3f ph %.2f tint %.4f q %u pq %u gain %.1f dc %.1f syncs %u\n", i / 48000.0, ph, d.t_int, d.quiet, d.post_quiet, d.gain, d.dc, d.syncs); }
        if(i % 48000 == 0) { printf("  %.2fs: syncs %u locked %d q %d gain %.1f dc %.1f\n", i / 48000.0, d.syncs, d.locked, dsp_quality(&d), d.gain, d.dc); }
        while(wr != g_emu_pcm_wr) { pcm[np++] = g_emu_pcm[wr % 8192u]; wr++; }
    }
    printf("%zu samples %.2fs, frames %u bad %u\n", n, n / 48000.0, g_rx, g_bad);
    for(size_t o = 0; o + 2000 <= np; o += 2000) { tone_meas_t t = tone_measure(pcm + o, 2000, 8000); printf("  pcm %.2fs: %.1f Hz %.1f dB\n", o / 8000.0, t.freq, t.sinad_db); }
    if(argc > 3) { FILE *o = fopen(argv[3], "wb"); fwrite(pcm, 2, np, o); fclose(o); }
    return 0;
}
