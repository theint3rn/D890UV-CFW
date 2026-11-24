/* Platform voice path. Vocoder backends (g_voc_select: 0 auto, 1 sine, 2 imbe, 3 SCT3288):
 *   imbe     cfw/voc/imbe (cfw-p25): IMBE 7200x4400 for P25 Phase 1 (88-bit frames); auto/2/3 use it for IMBE.
 *            Dequantisation follows the published IMBE tables; mbelib decodes its frames. Not checked against a
 *            real P25 radio yet.
 *   SCT3288  AMBE-3000 packets on the baseband SPI2 link (hal/voc_sct.c, docs/SCT3288_VOCODER.md): AMBE+2 49-bit
 *            (DMR, NXDN, YSF, P25 Ph2) and D-STAR AMBE 72-bit, when the chip answers PRODID. Chip replies are
 *            UNVERIFIED on hardware (emulator stand-in only).
 *   sine     cfw/voc/voc_sine.c, own harmonic test codec filling the payloads. TEST ONLY - NOT INTEROPERABLE.
 *            Fallback whenever the backend above is not there.
 * TX source: test tone (Menu > Test tone, 440 Hz at -6 dBFS) or silence (the microphone ADC path is not
 * mapped yet). RX: decoded PCM goes into g_emu_pcm (8 kHz ring, read by the emulator) and the tone meter. */
#include <math.h>
#include <string.h>
#include "voice.h"
#include "hal.h"
#include "voc_sine.h"
#include "tonemeter.h"
#include "voc_sct.h"
#include "voc_imbe.h"

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

void voice_init(void)
{
    voc_sine_enc_init(&s_enc);
    voc_sine_dec_init(&s_dec);
    voc_imbe_init();
    memset(&s_meter, 0, sizeof s_meter);
}

void voice_set_test_tone(uint16_t hz) { s_tone_hz = hz; }
uint16_t voice_test_tone(void) { return s_tone_hz; }
/* which backend codes this vocoder type right now */
typedef enum { VB_SINE, VB_SCT, VB_IMBE } vback_t;

static vback_t backend_for(cfw_vocoder_t voc)
{
    uint8_t sel = g_voc_select;
    if(voc == CFW_VOC_IMBE_88) { return sel == 1 ? VB_SINE : VB_IMBE; }   /* cfw/voc/imbe */
    if(sel == 1) { return VB_SINE; }
    if(sel == 3 || sel == 0) { return sct_voc_available() ? VB_SCT : VB_SINE; }
    return VB_SINE;
}

static int sct_rate(cfw_vocoder_t voc) { return voc == CFW_VOC_AMBE_DSTAR ? SCT_RATE_DSTAR : SCT_RATE_49; }

const char *voice_codec_name_for(cfw_vocoder_t voc)
{
    switch(backend_for(voc)) {
    case VB_SCT:  return "SCT3288";
    case VB_IMBE: return "imbe";
    default:      return "sine TEST";
    }
}
bool voice_codec_interoperable_for(cfw_vocoder_t voc) { vback_t b = backend_for(voc); return b == VB_SCT || b == VB_IMBE; }
const char *voice_codec_name(void) { return voice_codec_name_for(CFW_VOC_AMBE2_49); }
bool voice_codec_interoperable(void) { return voice_codec_interoperable_for(CFW_VOC_AMBE2_49); }

/* TX audio source: test tone or silence (microphone path not mapped) */
static void tx_audio(int16_t pcm[VOC_FRAME])
{
    float w = 2.0f * (float)M_PI * (float)s_tone_hz / 8000.0f;
    for(int i = 0; i < VOC_FRAME; i++) {
        pcm[i] = s_tone_hz ? (int16_t)(16384.0f * sinf(s_tone_ph)) : 0;
        s_tone_ph += w;
        if(s_tone_ph > 2.0f * (float)M_PI) { s_tone_ph -= 2.0f * (float)M_PI; }
    }
}

/* ---- SCT3288 asynchronous pipeline: TX frames encoded ahead, RX frames decoded behind ---- */
#define ENCQ 24     /* P25 Ph2 pulls a whole 4V/2V burst set at once (up to 18 frames per 360 ms superframe) */
#define DECQ 12
typedef struct { uint8_t b[9]; uint8_t nbits; } vframe_t;
static vframe_t s_encq[ENCQ];
static uint8_t s_encq_rd, s_encq_n;
static vframe_t s_decq[DECQ];
static uint8_t s_decq_rd, s_decq_n;
static cfw_vocoder_t s_tx_voc = CFW_VOC_AMBE2_49, s_rx_voc = CFW_VOC_AMBE2_49;
static uint32_t s_tx_demand_ms;
static void push_pcm(const int16_t *pcm);

static void enc_done(uint8_t b0, const uint8_t *d, uint16_t len)
{
    (void)b0;
    if(len < 2 || s_encq_n >= ENCQ) { return; }
    vframe_t *f = &s_encq[(s_encq_rd + s_encq_n) % ENCQ];
    f->nbits = d[0];
    memset(f->b, 0, sizeof f->b);
    memcpy(f->b, d + 1, (size_t)(len - 1 < 9 ? len - 1 : 9));
    s_encq_n++;
}

static void dec_done(uint8_t b0, const uint8_t *d, uint16_t len)
{
    (void)b0;
    if(len < 321 || d[0] != 0xA0) { return; }
    int16_t pcm[VOC_FRAME];
    for(int i = 0; i < VOC_FRAME; i++) { pcm[i] = (int16_t)((d[1 + 2 * i] << 8) | d[2 + 2 * i]); }
    push_pcm(pcm);
}

void voice_poll(void)
{
    if(sct_async_busy()) { return; }
    if(s_decq_n) {                                       /* RX first: received audio must not lag */
        vframe_t *f = &s_decq[s_decq_rd];
        if(sct_async_decode(sct_rate(s_rx_voc), f->b, f->nbits, dec_done)) {
            s_decq_rd = (uint8_t)((s_decq_rd + 1) % DECQ); s_decq_n--;
        }
        return;
    }
    /* keep the encode queue topped up while a transmission pulls frames (or right after PTT) */
    if(s_encq_n < ENCQ - 1 && hal_millis() - s_tx_demand_ms < 300u && backend_for(s_tx_voc) == VB_SCT) {
        int16_t pcm[VOC_FRAME];
        tx_audio(pcm);
        sct_async_encode(sct_rate(s_tx_voc), pcm, enc_done);
    }
}

void voice_tx_prepare(cfw_vocoder_t voc)
{
    s_tx_voc = voc;
    s_encq_n = 0; s_encq_rd = 0;
    s_tx_demand_ms = hal_millis();
}

size_t voice_tx_frame(cfw_vocoder_t voc, uint8_t *bits, size_t nbits)
{
    int16_t pcm[VOC_FRAME];
    if(backend_for(voc) == VB_SCT) {
        /* frames were encoded ahead by voice_poll(); pop the oldest (silence-repeat if the queue ran dry) */
        s_tx_voc = voc;
        s_tx_demand_ms = hal_millis();
        if(s_encq_n) {
            int nb = s_encq[s_encq_rd].nbits;
            if((size_t)nb > nbits) { nb = (int)nbits; }
            memcpy(bits, s_encq[s_encq_rd].b, (size_t)(nb + 7) / 8);     /* never more than the caller's frame */
            s_encq_rd = (uint8_t)((s_encq_rd + 1) % ENCQ); s_encq_n--;
            return (size_t)nb;
        }
        memset(bits, 0, (nbits + 7) / 8);
        return voc == CFW_VOC_AMBE_DSTAR ? 72 : 49;
    }
    if(backend_for(voc) == VB_IMBE) {
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
    if(backend_for(voc) == VB_IMBE) {                    /* own repeat/mute handling */
        if(voc_imbe_decode(bits, errs, pcm) != 0) { s_meter.bad++; }
        push_pcm(pcm);
        s_meter.frames++;
        s_last_rx_ms = hal_millis();
        return;
    }
    const uint8_t *use = bits;
    if(errs < 0 || errs >= 3) {
        s_meter.bad++;
        if(++s_repeats > 3) { memset(s_last, 0, sizeof s_last); }   /* b0 0 + gain 0 = silence */
        use = s_last;
    } else {
        s_repeats = 0;
        memcpy(s_last, bits, nbits >= 72 && voc == CFW_VOC_AMBE_DSTAR ? 9u : (size_t)(nb + 7) / 8);
    }
    bool done = false;
    if(backend_for(voc) == VB_SCT) {                     /* decoded asynchronously by voice_poll() */
        s_rx_voc = voc;
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
    if(!done) { voc_sine_decode(&s_dec, use, nb, pcm); }
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
