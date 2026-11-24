/* Platform voice path. Vocoder backends (g_voc_select: 0 auto, 1 sine, 2 imbe, 3 SCT3288):
 *   imbe     cfw/voc/imbe (cfw-p25): IMBE 7200x4400 for P25 Phase 1, NXDN EFR, YSF VW (88-bit frames); auto/2/3 use it
 *            for IMBE. Dequantisation follows the published IMBE tables; mbelib decodes its frames. Not checked against
 *            a real P25 radio yet.
 *   SCT3288  hal/voc_sct.c, docs/SCT3288_VOCODER.md. Mode A: microphone and speaker on the chip, channel frames on SPI2
 *            (AMBE+2 3600x2450: DMR, NXDN EHR, YSF DN, P25 Ph2; 72 bits with FEC, converted from / to the 49 bits the
 *            protocol glue uses). Mode B: the same with PCM from the MCU (test tone) instead of the microphone. Mode C:
 *            external AMBE packets, if a chip answers PRODID (also D-STAR). Not proven on a real chip yet.
 *   sine     cfw/voc/voc_sine.c, own harmonic test codec filling the payloads. TEST ONLY - NOT INTEROPERABLE.
 *            Fallback whenever the backend above is not there.
 * TX source: test tone (Menu > Test tone, 440 Hz at -6 dBFS) or the microphone through the SCT3288 (frames, or PCM for
 * the IMBE coder); silence when the chip does not answer. RX: the SCT3288 plays its own frames on the speaker; audio
 * decoded on the MCU (IMBE) goes to the speaker as PCM through the chip. Decoded PCM also goes into g_emu_pcm (8 kHz
 * ring, read by the emulator) and the tone meter; with mode A only when g_sct_tap = 1 (not in HW builds). */
#include <math.h>
#include <string.h>
#include "voice.h"
#include "hal.h"
#include "voc_sine.h"
#include "tonemeter.h"
#include "voc_sct.h"
#include "voc_imbe.h"
#include "ambe_fec.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* vocoder backend choice (scripts / Menu): 0 = auto (best available per mode), 1 = sine (test), 2 = imbe (P25 Ph1;
 * AMBE modes then use sine), 3 = SCT3288 (AMBE+2 / D-STAR AMBE when the chip answers; IMBE stays on imbe). */
volatile uint8_t g_voc_select;

int16_t g_emu_pcm[8192];
volatile uint32_t g_emu_pcm_wr;

static voc_sine_enc_t s_enc;
static voc_sine_dec_t s_dec;
static uint16_t s_tone_hz;
static float s_tone_ph;
static voice_meter_t s_meter;
static uint32_t s_last_rx_ms, s_last_meas_ms, s_frames_since;

static void chan_in(const uint8_t *f, unsigned n);
static void pcm_in(const uint8_t *be);

void voice_init(void)
{
    voc_sine_enc_init(&s_enc);
    voc_sine_dec_init(&s_dec);
    voc_imbe_init();
    sct_set_sinks(chan_in, pcm_in);
    memset(&s_meter, 0, sizeof s_meter);
}

void voice_set_test_tone(uint16_t hz) { s_tone_hz = hz; }
uint16_t voice_test_tone(void) { return s_tone_hz; }
/* which backend codes this vocoder type right now */
typedef enum { VB_SINE, VB_SCT, VB_EXT, VB_IMBE } vback_t;

static vback_t backend_for(cfw_vocoder_t voc)
{
    uint8_t sel = g_voc_select;
    if(voc == CFW_VOC_IMBE_88) { return sel == 1 ? VB_SINE : VB_IMBE; }   /* cfw/voc/imbe */
    if(sel == 1 || sel == 2) { return VB_SINE; }
    /* the internal vocoder has no 3600x2400 (D-STAR) rate: only an external AMBE chip would */
    if(voc != CFW_VOC_AMBE_DSTAR && sct_chip_ok()) { return VB_SCT; }
    return sct_ext_available() ? VB_EXT : VB_SINE;
}

static int sct_rate(cfw_vocoder_t voc) { return voc == CFW_VOC_AMBE_DSTAR ? SCT_RATE_DSTAR : SCT_RATE_49; }

const char *voice_codec_name_for(cfw_vocoder_t voc)
{
    switch(backend_for(voc)) {
    case VB_SCT:  return "SCT3288";
    case VB_EXT:  return "ext AMBE";
    case VB_IMBE: return "imbe";
    default:      return "sine TEST";
    }
}
bool voice_codec_interoperable_for(cfw_vocoder_t voc) { return backend_for(voc) != VB_SINE; }
const char *voice_codec_name(void) { return voice_codec_name_for(CFW_VOC_AMBE2_49); }
bool voice_codec_interoperable(void) { return voice_codec_interoperable_for(CFW_VOC_AMBE2_49); }

static void push_pcm(const int16_t *pcm);

/* microphone PCM (SCT_MIC_PCM) parks in g_emu_pcm, which only receive uses otherwise (half duplex) */
static uint32_t s_mic_wr, s_mic_rd;
static bool s_mic_go;
static uint16_t s_pull_n, s_pull_max;             /* frames pulled in one go: P25 Ph2 takes 18, P25 Ph1 9, DMR 3 */
static uint32_t s_pull_ms;
static bool mic_source(void) { return !s_tone_hz && g_sct_force != 2 && sct_can(SCT_MIC_PCM); }

static void note_pull(void)
{
    uint32_t now = hal_millis();
    if(now - s_pull_ms > 15u) { s_pull_n = 0; }
    s_pull_ms = now;
    if(++s_pull_n > s_pull_max) { s_pull_max = s_pull_n; }
}

/* TX audio source: test tone, microphone PCM or silence */
static void tx_audio(int16_t pcm[VOC_FRAME])
{
    if(!s_tone_hz) {
        uint32_t have = (s_mic_wr - s_mic_rd) / VOC_FRAME;
        if(!s_mic_go && have >= (uint32_t)s_pull_max + 2u) { s_mic_go = true; }
        if(!s_mic_go || !have) { s_mic_go = false; memset(pcm, 0, VOC_FRAME * sizeof pcm[0]); return; }
        for(int i = 0; i < VOC_FRAME; i++) { pcm[i] = g_emu_pcm[(s_mic_rd + (uint32_t)i) % 8192u]; }
        s_mic_rd += VOC_FRAME;
        return;
    }
    float w = 2.0f * (float)M_PI * (float)s_tone_hz / 8000.0f;
    for(int i = 0; i < VOC_FRAME; i++) {
        pcm[i] = (int16_t)(16384.0f * sinf(s_tone_ph));
        s_tone_ph += w;
        if(s_tone_ph > 2.0f * (float)M_PI) { s_tone_ph -= 2.0f * (float)M_PI; }
    }
}

/* ---- SCT3288 pipeline: TX frames queued ahead, RX frames played / decoded behind ---- */
#define ENCQ 30     /* P25 Ph2 pulls a whole 4V/2V burst set at once (up to 18 frames per 360 ms superframe) */
#define DECQ 30
typedef struct { uint8_t b[9]; uint8_t nbits; } vframe_t;
static vframe_t s_encq[ENCQ];
static uint8_t s_encq_rd, s_encq_n;
static vframe_t s_decq[DECQ];
static uint8_t s_decq_rd, s_decq_n;
static cfw_vocoder_t s_tx_voc = CFW_VOC_AMBE2_49, s_rx_voc = CFW_VOC_AMBE2_49;
static uint32_t s_tx_demand_ms, s_rx_demand_ms;
static bool s_tx_on, s_rx_on, s_enc_go, s_dec_go;
static uint32_t s_play_rd;                        /* SCT_PLAY_PCM: next g_emu_pcm sample for the speaker */
static bool s_play_go;

/* AMBE+2 3600x2450 silence, 49 bits (the usual DMR silence frame B9E881526173002A6B without its FEC) */
static const uint8_t k_sil49[7] = { 0xF8, 0x01, 0xA9, 0x9F, 0x8C, 0xE0, 0x80 };
static const uint8_t k_sil_dstar[9] = { 0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };

static void enc_push(const uint8_t *b, unsigned nbytes, uint8_t nbits)
{
    if(s_encq_n >= ENCQ) {                               /* our clock is slower than the codec's: drop the oldest */
        s_encq_rd = (uint8_t)((s_encq_rd + 1) % ENCQ); s_encq_n--;
    }
    vframe_t *f = &s_encq[(s_encq_rd + s_encq_n) % ENCQ];
    f->nbits = nbits;
    memset(f->b, 0, sizeof f->b);
    memcpy(f->b, b, nbytes < 9 ? nbytes : 9);
    s_encq_n++;
}

static void chan_in(const uint8_t *f, unsigned n)        /* frames from the chip's encoder */
{
    for(; n; n--, f += 9) {
        uint8_t d49[7];
        if(sct_raw49()) { memcpy(d49, f, 7); }
        else { sct_note_format(ambe72_decode(f, d49), f); }
        enc_push(d49, 7, 49);
    }
}

static void pcm_in(const uint8_t *be)                    /* microphone PCM, or the decoder's output (g_sct_tap) */
{
    int16_t pcm[VOC_FRAME];
    for(int i = 0; i < VOC_FRAME; i++) { pcm[i] = (int16_t)((be[2 * i] << 8) | be[2 * i + 1]); }
    if(sct_session_now() != SCT_MIC_PCM) { push_pcm(pcm); return; }
    for(int i = 0; i < VOC_FRAME; i++) { g_emu_pcm[(s_mic_wr + (uint32_t)i) % 8192u] = pcm[i]; }
    s_mic_wr += VOC_FRAME;
    if(s_mic_wr - s_mic_rd > 8192u - VOC_FRAME) { s_mic_rd = s_mic_wr - 4096u; }
}

static void enc_done(uint8_t b0, const uint8_t *d, uint16_t len)
{
    (void)b0;
    if(len < 2) { return; }
    enc_push(d + 1, (unsigned)(len - 1), d[0]);
}

static void dec_done(uint8_t b0, const uint8_t *d, uint16_t len)
{
    (void)b0;
    if(len < 321 || d[0] != 0xA0) { return; }
    int16_t pcm[VOC_FRAME];
    for(int i = 0; i < VOC_FRAME; i++) { pcm[i] = (int16_t)((d[1 + 2 * i] << 8) | d[2 + 2 * i]); }
    push_pcm(pcm);
}

static sct_sess_t wanted_session(vback_t txb, vback_t rxb)
{
    if(s_tx_on) {
        if(txb == VB_SCT) {
            bool mic = !s_tone_hz && g_sct_force != 2 && sct_can(SCT_ENC_MIC);
            return mic ? SCT_ENC_MIC : SCT_ENC_PCM;
        }
        return txb == VB_IMBE && mic_source() ? SCT_MIC_PCM : SCT_OFF;
    }
    if(s_rx_on) {
        if(rxb == VB_SCT) { return SCT_DEC; }
        if(rxb == VB_IMBE) { return SCT_PLAY_PCM; }      /* decoded on the MCU: speaker through the chip */
    }
    return SCT_OFF;
}

static void dec_feed(uint32_t now)                       /* 4 frames (80 ms) when the chip asks for them */
{
    if(!sct_want_chan()) { return; }
    bool ended = now - s_rx_demand_ms > 150u;
    if(!s_dec_go && s_decq_n >= 6) { s_dec_go = true; }
    if(ended && s_decq_n) { s_dec_go = true; }           /* call over: play the rest, padded with silence */
    if(!s_dec_go || (s_decq_n < 4 && !ended)) { return; }
    if(!s_decq_n) { s_dec_go = false; return; }
    uint8_t p[36];
    for(int i = 0; i < 4; i++) {
        const uint8_t *b = k_sil49;
        if(s_decq_n) { b = s_decq[s_decq_rd].b; s_decq_rd = (uint8_t)((s_decq_rd + 1) % DECQ); s_decq_n--; }
        ambe72_encode(b, p + 9 * i);
    }
    if(!s_decq_n) { s_dec_go = false; }
    (void)sct_put_chan(p);
}

static void play_feed(uint32_t now)
{
    if(!sct_want_pcm()) { return; }
    uint32_t have = g_emu_pcm_wr - s_play_rd;
    if(have > 8192u - 2u * VOC_FRAME) { s_play_rd = g_emu_pcm_wr - 2u * VOC_FRAME; have = 2u * VOC_FRAME; }
    if(!s_play_go && (have >= 6u * VOC_FRAME || (have >= VOC_FRAME && now - s_rx_demand_ms > 150u))) { s_play_go = true; }
    if(have < VOC_FRAME) { s_play_go = false; }
    if(!s_play_go) { return; }
    int16_t pcm[VOC_FRAME];
    for(int i = 0; i < VOC_FRAME; i++) { pcm[i] = g_emu_pcm[(s_play_rd + (uint32_t)i) % 8192u]; }
    if(sct_put_pcm(pcm)) { s_play_rd += VOC_FRAME; }
}

void voice_poll(void)
{
    sct_poll();
    if(sct_bench_active()) { s_tx_on = s_rx_on = false; return; }
    uint32_t now = hal_millis();
    if(s_tx_on && now - s_tx_demand_ms > 400u) { s_tx_on = false; }
    if(s_rx_on && now - s_rx_demand_ms > 900u) { s_rx_on = false; s_decq_n = 0; s_dec_go = false; }
    vback_t txb = backend_for(s_tx_voc), rxb = backend_for(s_rx_voc);
    sct_session(wanted_session(txb, rxb));
    switch(sct_session_now()) {
    case SCT_ENC_PCM:
        if(sct_want_pcm()) {
            int16_t pcm[VOC_FRAME];
            tx_audio(pcm);
            (void)sct_put_pcm(pcm);
        }
        break;
    case SCT_DEC:      dec_feed(now); break;
    case SCT_PLAY_PCM: play_feed(now); break;
    default: break;
    }
    if(sct_async_busy()) { return; }
    if(s_decq_n && rxb == VB_EXT) {                      /* RX first: received audio must not lag */
        vframe_t *f = &s_decq[s_decq_rd];
        if(sct_async_decode(sct_rate(s_rx_voc), f->b, f->nbits, dec_done)) {
            s_decq_rd = (uint8_t)((s_decq_rd + 1) % DECQ); s_decq_n--;
        }
        return;
    }
    /* keep the encode queue topped up while a transmission pulls frames (or right after PTT) */
    if(s_tx_on && txb == VB_EXT && s_encq_n < ENCQ - 1) {
        int16_t pcm[VOC_FRAME];
        tx_audio(pcm);
        sct_async_encode(sct_rate(s_tx_voc), pcm, enc_done);
    }
}

void voice_tx_prepare(cfw_vocoder_t voc)
{
    s_tx_voc = voc;
    s_encq_n = 0; s_encq_rd = 0;
    s_enc_go = false;
    s_mic_rd = s_mic_wr;
    s_mic_go = false;
    s_pull_max = 0;
    s_tx_on = true;
    s_tx_demand_ms = hal_millis();
}

const char *voice_tx_source(void)
{
    if(s_tone_hz) { return "test tone"; }
    vback_t b = backend_for(s_tx_voc);
    if(b == VB_SCT) { return sct_can(SCT_ENC_MIC) && g_sct_force != 2 ? "microphone" : "silence (no mic)"; }
    return b == VB_IMBE && mic_source() ? "microphone" : "silence (no mic)";
}

size_t voice_tx_frame(cfw_vocoder_t voc, uint8_t *bits, size_t nbits)
{
    int16_t pcm[VOC_FRAME];
    vback_t be = backend_for(voc);
    s_tx_voc = voc;
    s_tx_on = true;
    s_tx_demand_ms = hal_millis();
    note_pull();
    if(be == VB_SCT || be == VB_EXT) {
        /* frames come from the chip (voice_poll); pop the oldest. The microphone delivers in real time: start only with
         * enough frames for the biggest pull of this mode, silence until then and when the queue runs dry */
        if(!s_enc_go && s_encq_n >= s_pull_max + 4u) { s_enc_go = true; }
        if(!s_enc_go && s_encq_n >= ENCQ - 4u) { s_enc_go = true; }
        if(s_encq_n && s_enc_go) {
            int nb = s_encq[s_encq_rd].nbits;
            if((size_t)nb > nbits) { nb = (int)nbits; }
            memcpy(bits, s_encq[s_encq_rd].b, (size_t)(nb + 7) / 8);     /* never more than the caller's frame */
            s_encq_rd = (uint8_t)((s_encq_rd + 1) % ENCQ); s_encq_n--;
            return (size_t)nb;
        }
        s_enc_go = false;
        bool ds = voc == CFW_VOC_AMBE_DSTAR;
        memset(bits, 0, (nbits + 7) / 8);
        if(nbits >= (ds ? 72u : 49u)) { memcpy(bits, ds ? k_sil_dstar : k_sil49, ds ? 9u : 7u); }
        return ds ? 72 : 49;
    }
    if(be == VB_IMBE) {
        if(nbits < 88) { return 0; }
        tx_audio(pcm);
        return voc_imbe_encode(pcm, bits);
    }
    tx_audio(pcm);
    /* sine test codec: 49 bits (AMBE payload; D-STAR: first 49 of the 72-bit slot, no AMBE FEC) or 88 (IMBE) */
    int nb = (voc == CFW_VOC_IMBE_88) ? 88 : 49;
    if(nbits < (size_t)nb) { return 0; }
    voc_sine_encode(&s_enc, pcm, bits, nb);
    return voc == CFW_VOC_AMBE_DSTAR ? nbits : (size_t)nb;
}

/* error concealment like an AMBE decoder: a frame whose FEC failed (errs < 0), or needed many corrections
 * (the unprotected bits are then likely hit too), is replaced by the last good frame; after 3 repeats: mute */
static uint8_t s_last[11];   /* also holds a 72-bit D-STAR frame (9 bytes) */
static int s_repeats;

void voice_rx_frame(cfw_vocoder_t voc, const uint8_t *bits, size_t nbits, int errs)
{
    int16_t pcm[VOC_FRAME];
    int nb = (voc == CFW_VOC_IMBE_88) ? 88 : 49;      /* D-STAR: the 49 test-codec bits lead the 72-bit frame */
    if(nbits < (size_t)nb) { return; }
    if(voc != s_rx_voc || !s_rx_on) { s_decq_n = 0; s_decq_rd = 0; s_dec_go = false; s_play_rd = g_emu_pcm_wr; s_play_go = false; }
    s_rx_voc = voc;
    s_rx_on = true;
    s_rx_demand_ms = hal_millis();
    vback_t be = backend_for(voc);
    if(be == VB_IMBE) {                                  /* own repeat/mute handling */
        if(voc_imbe_decode(bits, errs, pcm) != 0) { s_meter.bad++; }
        push_pcm(pcm);
        s_meter.frames++;
        s_last_rx_ms = hal_millis();
        return;
    }
    const uint8_t *use = bits;
    if(errs < 0 || errs >= 3) {
        s_meter.bad++;
        if(++s_repeats > 3) {                            /* silence */
            memset(s_last, 0, sizeof s_last);
            if(be == VB_SCT) { memcpy(s_last, k_sil49, 7); }
            else if(be == VB_EXT) { memcpy(s_last, voc == CFW_VOC_AMBE_DSTAR ? k_sil_dstar : k_sil49, voc == CFW_VOC_AMBE_DSTAR ? 9 : 7); }
        }
        use = s_last;
    } else {
        s_repeats = 0;
        memcpy(s_last, bits, nbits >= 72 && voc == CFW_VOC_AMBE_DSTAR ? 9u : (size_t)(nb + 7) / 8);
    }
    if(be == VB_SCT || be == VB_EXT) {                   /* played / decoded by voice_poll() */
        if(s_decq_n < DECQ) {
            vframe_t *f = &s_decq[(s_decq_rd + s_decq_n) % DECQ];
            memset(f->b, 0, sizeof f->b);
            f->nbits = (uint8_t)(voc == CFW_VOC_AMBE_DSTAR ? 72 : 49);
            memcpy(f->b, use, (size_t)(f->nbits + 7) / 8);
            s_decq_n++;
        }
        s_meter.frames++;
        s_last_rx_ms = hal_millis();
        return;
    }
    voc_sine_decode(&s_dec, use, nb, pcm);
    push_pcm(pcm);
    s_meter.frames++;
    s_last_rx_ms = hal_millis();
}

/* FM receive audio (8 kHz PCM from the discriminator, hal/dsp_backend.c): into the same ring + tone meter */
/* Volume limit (app/spk.c, Q8, 256 = 1.0): gain on the decoded audio the MCU outputs (g_emu_pcm = the speaker
 * stand-in in the emulator) */
static uint16_t s_gain_q8 = 256;
void voice_set_gain(uint16_t q8) { s_gain_q8 = q8 > 256u ? 256u : q8; }
static int16_t gain(int16_t v) { return (int16_t)(((int32_t)v * s_gain_q8) / 256); }

void voice_rx_pcm(const int16_t *pcm, size_t n)
{
    for(size_t i = 0; i < n; i++) { g_emu_pcm[(g_emu_pcm_wr + (uint32_t)i) % 8192u] = gain(pcm[i]); }
    g_emu_pcm_wr += (uint32_t)n;
    s_frames_since += (uint32_t)n;                      /* counted in samples */
    s_last_rx_ms = hal_millis();
}

static void push_pcm(const int16_t *pcm)
{
    for(int i = 0; i < VOC_FRAME; i++) { g_emu_pcm[(g_emu_pcm_wr + (uint32_t)i) % 8192u] = gain(pcm[i]); }
    g_emu_pcm_wr += VOC_FRAME;
    s_frames_since += VOC_FRAME;
    s_last_rx_ms = hal_millis();
}

void voice_tick(void)
{
    uint32_t now = hal_millis();
    if(now - s_last_rx_ms > 1500u) { s_meter.valid = false; }
    if(now - s_last_meas_ms < 500u || s_frames_since < 3200u) { return; }   /* >= 0.4 s of new audio (samples) */
    s_last_meas_ms = now;
    s_frames_since = 0;
    static int16_t buf[3200];                          /* last 0.4 s */
    uint32_t wr = g_emu_pcm_wr;
    for(uint32_t i = 0; i < 3200u; i++) { buf[i] = g_emu_pcm[(wr - 3200u + i) % 8192u]; }
    tone_meas_t m = tone_measure(buf, 3200, 8000.0f);
    s_meter.freq = m.freq;
    s_meter.sinad_db = m.sinad_db;
    s_meter.valid = m.freq > 0;
}

const voice_meter_t *voice_meter(void) { return &s_meter; }
void voice_meter_reset(void) { memset(&s_meter, 0, sizeof s_meter); voc_sine_dec_init(&s_dec); memset(s_last, 0, sizeof s_last); s_repeats = 0; }
