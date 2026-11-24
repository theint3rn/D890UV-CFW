/* Host loopback test of NXDN96 EFR voice through the proto glue (app/protos/proto_nxdn.c):
 *   tools/efr_loop.sh            (builds into build_streams/ and runs; exit 0 = pass)
 * An NXDN96 direct channel is configured; a 1011 Hz tone is IMBE-encoded (voc/imbe) on every voice_tx pull,
 * the glue builds the 4FSK symbols (EFR 144-bit FEC), and the same symbols are fed back into its RX.
 * Checks: air parameters (4800 sym/s, +/-2400 Hz, 12.5 kHz), IMBE frames come back bit exact as
 * CFW_VOC_IMBE_88 (no AMBE+2 frames), the decoded audio peaks at the tone, and switching the running channel
 * back to NXDN48 changes the air parameters and retunes.
 * Encrypted TX (scramble / DES-OFB / AES-256-OFB, NXDN48 and NXDN96): with the channel key the receiver hands
 * out the plain vocoder frames bit exact; without it no voice comes out (muted). The key hooks of glue.h
 * (cfw_crypt_key / cfw_crypt_tx, weak in crypt_weak.c) are replaced here. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "modem.h"
#include "imbe.h"

#define MAXF 700
#define TONE_HZ 1011

extern const cfw_proto_t *const cfw_proto_reg_k_nxdn;

/* test keys (kinds 7 scramble, 8 DES, 9 AES-256): TS 1-D 7.2.1 key values */
static const uint8_t K_SCR[2] = { 0x00, 0x01 };
static const uint8_t K_DES[8] = { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89 };
static const uint8_t K_AES[32] = { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                                   0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
static int g_tx_kind;       /* 0 = clear, else key kind sent */
static bool g_rx_keys;      /* receiver has the keys */
static bool key_of(uint8_t kind, uint8_t *id, const uint8_t **k, uint8_t *len)
{
    switch(kind) {
    case 7: *id = 1; *k = K_SCR; *len = 2; return true;
    case 8: *id = 2; *k = K_DES; *len = 8; return true;
    case 9: *id = 3; *k = K_AES; *len = 32; return true;
    default: return false;
    }
}
bool cfw_crypt_key(int i, uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len)
{
    if(!g_rx_keys || i < 0 || 7 + i < lo || 7 + i > hi) { return false; }
    *kind = (uint8_t)(7 + i);
    return key_of(*kind, key_id, key, len);
}
bool cfw_crypt_tx(uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len)
{
    if(!g_tx_kind || g_tx_kind < lo || g_tx_kind > hi) { return false; }
    *kind = (uint8_t)g_tx_kind;
    return key_of(*kind, key_id, key, len);
}

static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static int ntx, nrx, nbad, tunes, rx49, calls;
static uint8_t sent[MAXF][11], got[MAXF][11];
static bool g_imbe = true;  /* tone through voc/imbe (EFR test) or random 49-bit frames (crypt tests) */
static unsigned rnd = 12345;
static imbe_enc_t enc;
static imbe_dec_t dec;
static int16_t pcm_out[MAXF * 160];
static int tpos;

static uint32_t p_now(void) { return 0; }
static void p_log(cfw_log_level_t l, const char *m) { (void)l; (void)m; }
static void p_voice(cfw_vocoder_t v, const uint8_t *b, size_t n, int e)
{
    if (v == CFW_VOC_AMBE2_49) { rx49++; if (g_imbe) { return; } }
    else if (v != CFW_VOC_IMBE_88 || n != 88) { nbad++; return; }
    if (nrx < MAXF) {
        memcpy(got[nrx], b, (n + 7) / 8);
        if (g_imbe) { imbe_decode(&dec, b, e, pcm_out + 160 * nrx); }
        nrx++;
    }
}
static void p_data(uint8_t t, const uint8_t *d, size_t n) { (void)t; (void)d; (void)n; }
static void p_call(const cfw_call_info_t *c) { (void)c; calls++; }
static bool p_tune(uint32_t r, uint32_t t) { (void)r; (void)t; tunes++; return true; }
static bool p_req(void) { return false; }
static bool p_cp(uint32_t a, void *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
static size_t p_vtx(cfw_vocoder_t v, uint8_t *b, size_t n)
{
    if (!g_imbe) {                                   /* random vocoder frame, unused bits zero */
        memset(b, 0, (n + 7) / 8);
        for (size_t i = 0; i < n; i++) { rnd = rnd * 1103515245u + 12345u; if ((rnd >> 16) & 1u) { b[i >> 3] |= (uint8_t)(0x80u >> (i & 7u)); } }
        if (ntx < MAXF) { memcpy(sent[ntx], b, (n + 7) / 8); }
        ntx++;
        return n;
    }
    if (v != CFW_VOC_IMBE_88 || n != 88) { nbad++; return 0; }
    int16_t pcm[160];
    for (int i = 0; i < 160; i++, tpos++) pcm[i] = (int16_t)(8000 * sin(2 * M_PI * TONE_HZ * tpos / 8000.0));
    imbe_encode(&enc, pcm, b);
    if (ntx < MAXF) memcpy(sent[ntx], b, 11);
    ntx++;
    return 88;
}
static const cfw_platform_t pf = { CFW_MODEM_API_VERSION, p_now, p_log, p_voice, p_data, p_call, p_tune, p_req, p_cp, p_vtx };

/* strongest frequency (Goertzel, 1 Hz steps) of pcm[0..n) */
static int peak_hz(const int16_t *pcm, int n)
{
    double best = 0; int bf = 0;
    for (int fq = 100; fq < 3900; fq++) {
        double w = 2 * M_PI * fq / 8000, c = 2 * cos(w), s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) { double s0 = pcm[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (pw > best) { best = pw; bf = fq; }
    }
    return bf;
}

int main(void)
{
    const cfw_proto_t *p = cfw_proto_reg_k_nxdn;
    imbe_enc_init(&enc);
    imbe_dec_init(&dec);
    p->init(p->ctx, &pf);
    p->rx_reset(p->ctx);

    cfw_chan_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.rx_hz = cfg.tx_hz = 446000000; cfg.my_id = 4001; cfg.talkgroup = 300; cfg.ran = 5;
    cfg.direct = true; cfg.nxdn96 = true;
    p->configure(p->ctx, &cfg);
    CHECK(p->air.symbol_rate == 4800 && p->air.deviation_hz == 2400 && p->air.channel_bw_hz == 12500,
          "NXDN96 air: %u sym/s, +/-%u Hz, %u Hz bw", p->air.symbol_rate, p->air.deviation_hz, p->air.channel_bw_hz);

    static uint8_t sym[200000];
    size_t n = 0, k;
    for (int i = 0; i < 300; i++) sym[n++] = (uint8_t)(i & 3);                  /* noise before */
    cfw_tx_req_t r = { .voice = true };
    p->tx_start(p->ctx, &r);
    int pulls = 0;
    while ((k = p->tx_symbols(p->ctx, sym + n, 64)) > 0) {
        n += k;
        if (++pulls == 300) p->tx_stop(p->ctx);                                 /* ~100 frames of 192 symbols */
        if (n > sizeof sym - 64) break;
    }
    for (int i = 0; i < 300; i++) sym[n++] = (uint8_t)((i * 7) & 3);            /* noise after */
    p->rx_symbols(p->ctx, sym, NULL, n);

    int eq = 0;
    for (int i = 0; i < nrx && i < ntx && i < MAXF; i++) eq += !memcmp(sent[i], got[i], 11);
    CHECK(ntx > 50, "TX pulled %d IMBE frames (%zu symbols)", ntx, n);
    CHECK(nrx == ntx && eq == ntx, "RX %d IMBE frames, %d bit exact", nrx, eq);
    CHECK(nbad == 0 && rx49 == 0, "no wrong-vocoder frames (bad %d, AMBE+2 %d)", nbad, rx49);
    CHECK(calls > 0, "call reported (%d)", calls);

    int n0 = 160 * 10, nn = 160 * (nrx - 12);                                   /* skip decoder start-up */
    if (nn > 0) {
        double rms = 0;
        for (int i = n0; i < n0 + nn; i++) rms += pcm_out[i] * (double)pcm_out[i];
        rms = sqrt(rms / nn);
        int pk = peak_hz(pcm_out + n0, nn);
        CHECK(rms > 1000 && pk > TONE_HZ - 15 && pk < TONE_HZ + 15, "decoded audio rms %.0f, peak %d Hz (tone %d)",
              rms, pk, TONE_HZ);
    } else {
        CHECK(0, "decoded audio: too few frames");
    }

    int t0 = tunes;
    cfg.nxdn96 = false;
    p->configure(p->ctx, &cfg);                                                 /* running channel -> NXDN48 */
    CHECK(p->air.symbol_rate == 2400 && p->air.deviation_hz == 1050 && p->air.channel_bw_hz == 6250 && tunes > t0,
          "NXDN48 again: %u sym/s, +/-%u Hz, %u Hz bw, retuned %d", p->air.symbol_rate, p->air.deviation_hz,
          p->air.channel_bw_hz, tunes - t0);

    /* encrypted TX: each cipher on NXDN48 and NXDN96, receiver with and without the keys */
    static const char *const cn[4] = { "clear", "scramble", "DES-OFB", "AES-256-OFB" };
    g_imbe = false;
    for (int nx96 = 0; nx96 < 2; nx96++) {
        for (int c = 0; c < 4; c++) {
            for (int rk = 1; rk >= 0; rk--) {
                g_tx_kind = c ? 6 + c : 0;
                g_rx_keys = rk;
                ntx = nrx = nbad = rx49 = 0;
                p->rx_reset(p->ctx);
                cfg.nxdn96 = nx96;
                cfg.ran = (uint8_t)(10 + c);                                    /* new RAN = receiver restart */
                p->configure(p->ctx, &cfg);
                n = 0;
                for (int i = 0; i < 300; i++) sym[n++] = (uint8_t)(i & 3);
                p->tx_start(p->ctx, &r);
                pulls = 0;
                while ((k = p->tx_symbols(p->ctx, sym + n, 64)) > 0) {
                    n += k;
                    if (++pulls == 400) p->tx_stop(p->ctx);                  /* >= 6 superframes = 3 IV sessions */
                    if (n > sizeof sym - 64) break;
                }
                for (int i = 0; i < 300; i++) sym[n++] = (uint8_t)((i * 7) & 3);
                p->rx_symbols(p->ctx, sym, NULL, n);
                int nb = nx96 ? 11 : 7;
                eq = 0;
                for (int i = 0; i < nrx && i < ntx && i < MAXF; i++) eq += !memcmp(sent[i], got[i], (size_t)nb);
                char st[120];
                p->status(p->ctx, st, sizeof st);
                bool enc_shown = strstr(st, "ENC") != NULL;
                if (rk || !c) {
                    CHECK(ntx > 60 && nrx == ntx && eq == ntx && nbad == 0,
                          "%s %s TX, keyed RX: %d/%d frames bit exact", nx96 ? "NXDN96" : "NXDN48", cn[c], eq, ntx);
                } else {
                    CHECK(ntx > 60 && nrx == 0, "%s %s TX, RX without key: muted (%d frames out)",
                          nx96 ? "NXDN96" : "NXDN48", cn[c], nrx);
                }
                (void)enc_shown;
            }
        }
    }
    g_tx_kind = 0;

    printf(fails ? "efr_loop: %d FAILED\n" : "efr_loop: all passed\n", fails);
    return fails != 0;
}
