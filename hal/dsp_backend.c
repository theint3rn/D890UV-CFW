/* MCU-DSP modem backend: ADC1 ch1 (FM discriminator, 48 kHz ring from drivers/adc.c) -> cfw/dsp demodulator
 * for the current mode -> modem_rx_push(). Runs from modem_poll() in the main loop (every pass, >= every
 * 5 ms; the ring holds 20 ms). Receive only: there is no modulator path for the MCU yet.
 * UNVERIFIED ON HARDWARE: whether ch1 carries a flat (DC-coupled, not de-emphasised) discriminator signal is
 * unknown (docs/FEASIBILITY.md phase 0); the demod copes with DC offset, scale and polarity, and has a
 * switchable inverse de-emphasis (Menu > Demod). */
#include <math.h>
#include "modem_core.h"
#include "hal.h"
#include "dsp_demod.h"
#include "dsp_mod.h"
#include "dsp_backend.h"
#include "voice.h"

static dsp_demod_t s_d;
/* debug / emulator: last demodulated symbols (ring) */
#ifndef CFW_DBG_SYM_SIZE
#define CFW_DBG_SYM_SIZE 16384
#endif
uint8_t g_dbg_sym[CFW_DBG_SYM_SIZE];
volatile uint32_t g_dbg_sym_wr;
static bool s_on;
static uint32_t s_rd;
static dsp_opts_t s_opts;
static uint32_t s_busy_us, s_win_t0, s_load_x10;
static float s_buf[480];
static uint8_t s_sym[256];
static int8_t s_soft[256];

bool dspb_mode_for(const cfw_air_t *a, int *mode)
{
    dsp_mode_t m;
    switch(a->mod) {
    case CFW_MOD_4FSK:
        if(a->symbol_rate == 2400) { m = DSP_MODE_NXDN48; }
        else if(a->deviation_hz == 1944) { m = DSP_MODE_DMR; }
        else if(a->deviation_hz == 1800) { m = DSP_MODE_P25P1; }
        else if(a->deviation_hz >= 2700) { m = DSP_MODE_YSF; }
        else if(a->deviation_hz == 2400) { m = DSP_MODE_NXDN96; }
        else { m = DSP_MODE_DMR; }
        break;
    case CFW_MOD_GMSK:   m = DSP_MODE_DSTAR; break;
    case CFW_MOD_HDQPSK: m = DSP_MODE_P25P2; break;
    default: return false;
    }
    if(mode) { *mode = (int)m; }
    return true;
}

void dspb_set_opts(bool invert, uint16_t deemph_tau_us)
{
    s_opts.invert = invert;
    s_opts.tau_us = (float)deemph_tau_us;
}

void dspb_start(const cfw_air_t *a)
{
    int m;
    s_on = dspb_mode_for(a, &m);
    if(s_on) { dsp_init(&s_d, (dsp_mode_t)m, ADC_FS, &s_opts); }
    s_win_t0 = hal_micros();
    s_busy_us = 0;
}

void dspb_stop(void) { s_on = false; }

/* adc: 12-bit counts, stride = distance between ch1 samples (5 in the scan ring, 1 for a plain stream) */
void dspb_feed(const volatile uint16_t *adc, uint32_t n, uint32_t stride)
{
    if(!s_on) { return; }
    uint32_t t0 = hal_micros();
    while(n) {
        uint32_t k = n > 480u ? 480u : n;
        for(uint32_t i = 0; i < k; i++) { s_buf[i] = (float)adc[i * stride] - 2048.0f; }
        uint32_t done = 0;
        while(done < k) {
            /* at most 480/5 = 96 symbols per 480 samples at 48 kHz (P25P2: 60, NXDN48: 24) */
            size_t ns = dsp_process(&s_d, s_buf + done, k - done, s_sym, s_soft, sizeof s_sym);
            done = k;
            if(ns) {
                for(size_t i = 0; i < ns; i++) { g_dbg_sym[g_dbg_sym_wr++ % sizeof g_dbg_sym] = s_sym[i]; }
                modem_rx_push(s_sym, s_soft, ns);
            }
        }
        adc += k * stride;
        n -= k;
    }
    s_busy_us += hal_micros() - t0;
}

static void load_update(void)
{
    uint32_t now = hal_micros(), el = now - s_win_t0;
    if(el >= 1000000u) {
        s_load_x10 = (uint32_t)((uint64_t)s_busy_us * 1000u / el);
        s_busy_us = 0;
        s_win_t0 = now;
    }
}

uint32_t dspb_load_x10(void) { return s_load_x10; }
int dspb_quality(void) { return s_on ? dsp_quality(&s_d) : 0; }
bool dspb_locked(void) { return s_on && s_d.locked; }
bool dspb_active(void) { return s_on; }

/* ---- the backend on the real ADC ring ---- */
static bool db_supports(const cfw_air_t *a) { return dspb_mode_for(a, NULL); }

static void db_start_rx(const cfw_air_t *a, uint32_t hz)
{
    rf_set_freq(0, hz);
    rf_rx_fm(0, a->channel_bw_hz > 12500);          /* FM receive path = discriminator on ADC1 ch1 */
    dspb_start(a);
    s_rd = adc_write_frame();
}

/* ---------------- TX ---------------- */
static dsp_mod_t s_mod;
static bool s_tx, s_dac_moving;
static uint32_t s_tx_w, s_drain, s_last_r, s_last_move_us, s_soft_r;
static float s_hz[240];

static size_t tx_pull(void *u, uint8_t *sym, size_t max) { (void)u; return modem_tx_pull(sym, max); }

bool dspb_tx_start(const cfw_air_t *a, uint32_t hz)
{
#ifndef CFW_DIGITAL_TX
    (void)a; (void)hz;
    return false;
#else
    int m;
    if(!dspb_mode_for(a, &m)) { return false; }
    if(safety_tx_block(hz)) { return false; }        /* HW_NOTX / CLK ERR / no RF cal / forced stop (hal/safety.c) */
    s_on = false;                                     /* no RX demod while transmitting */
    dsp_mod_init(&s_mod, (dsp_mode_t)m, ADC_FS);
    rf_set_freq(0, hz);
    rf_tx_fm(0, true);
    dac_apc(true);
    dac_stream_start();
    s_last_r = s_soft_r = dac_read_pos();
    s_last_move_us = hal_micros();
    s_dac_moving = false;
    s_tx_w = (s_last_r + 240u) % DAC_RING;            /* 5 ms lead */
    s_drain = 0;
    s_tx = true;
    return true;
#endif
}

bool dspb_tx_active(void) { return s_tx; }
bool dspb_tx_dac_moving(void) { return s_dac_moving; }

static void tx_end(void)
{
    s_tx = false;
    dac_stream_stop();
    dac_apc(false);
    rf_tx_fm(0, false);
    modem_tx_done();
}

void dspb_tx_poll(void)
{
    if(!s_tx) { return; }
    uint32_t t0 = hal_micros();
    uint32_t r = dac_read_pos();
    if(r != s_last_r) { s_last_r = r; s_last_move_us = t0; s_dac_moving = true; }
    if(!s_dac_moving) {                               /* no DMA progress (no DAC model): pace by time */
        uint32_t ms = (t0 - s_last_move_us) / 1000u;
        s_soft_r = (s_soft_r + ms * (ADC_FS / 1000u)) % DAC_RING;
        s_last_move_us += ms * 1000u;
        r = s_soft_r;
    }
    volatile uint16_t *ring = dac_ring();
    uint32_t free = (r + DAC_RING - 16u - s_tx_w) % DAC_RING;
    while(free) {
        uint32_t k = free > 240u ? 240u : free;
        size_t n = s_mod.done ? 0 : dsp_mod_run(&s_mod, s_hz, k, tx_pull, NULL);
        for(uint32_t i = 0; i < k; i++) {
            float c = i < n ? DSP_MOD_DAC(s_hz[i]) : 2048.0f;
            ring[(s_tx_w + i) % DAC_RING] = (uint16_t)DAC_CODE((uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : c));
        }
        if(s_mod.done) { s_drain += k - (uint32_t)n; }
        s_tx_w = (s_tx_w + k) % DAC_RING;
        free -= k;
    }
    s_busy_us += hal_micros() - t0;
    if(s_drain >= DAC_RING + 240u) { tx_end(); }       /* the tail has left the ring */
}

static void db_start_tx(const cfw_air_t *a, uint32_t hz) { dspb_tx_start(a, hz); }
static void db_stop(void) { dspb_stop(); }

static void db_poll(void)
{
    if(s_tx) { dspb_tx_poll(); load_update(); return; }
    if(!s_on) { return; }
    uint32_t w = adc_write_frame();
    const volatile uint16_t *ring = adc_ring();
    while(s_rd != w) {
        uint32_t end = w > s_rd ? w : ADC_RING_FRAMES;
        dspb_feed(ring + ADC_FRAME_CH * s_rd, end - s_rd, ADC_FRAME_CH);
        s_rd = end % ADC_RING_FRAMES;
    }
    load_update();
}

const cfw_modem_backend_t g_backend_mcudsp = {
    "MCU-DSP", db_supports, db_start_rx, db_start_tx, db_stop, db_poll,
};

void dspb_poll_load(void) { load_update(); }

/* ---------------- FM (analog) through the MCU: test path ----------------
 * RX: ADC1 ch1 (discriminator) -> 6:1 average (8 kHz) -> DC removal -> voice_rx_pcm (g_emu_pcm + tone meter), only
 *     while the audio is above a noise threshold (no squelch on the MCU side yet).
 * TX: a test tone (and the channel's CTCSS) as FM deviation on the DAC baseband, like the digital modes. On a real
 *     radio FM audio goes mic -> RF chip; this DAC path is for the emulator tone test only (unverified, dummy load). */
static bool s_fm_rx, s_fm_tx;
volatile bool g_dspb_fm_mute;            /* FM squelch closed (app/radio.c): recovered audio not passed on */
static uint32_t s_fm_rd;
static float s_fm_dc, s_fm_acc, s_fm_ph, s_fm_ct_ph;
static uint8_t s_fm_n;
static uint16_t s_fm_tone, s_fm_ctcss;

void dspb_fm_rx(bool on)
{
    s_fm_rx = on;
    s_fm_rd = adc_write_frame();
    s_fm_acc = 0; s_fm_n = 0;
}

/* analog FM audio tap (8 kHz, every sample, squelch ignored): app/aprs_rx.c AFSK receiver; weak default = none */
__attribute__((weak)) void aprs_rx_sample(int16_t x) { (void)x; }
/* same tap for the analog signalling decoders (app/sig_dec.c) */
__attribute__((weak)) void sig_rx_sample(int16_t x) { (void)x; }
/* same tap for the tone squelch / NOAA alert decoders (app/stockfx.c) */
__attribute__((weak)) void tone_rx_sample(int16_t x) { (void)x; }

static void fm_rx_poll(void)
{
    static int16_t pcm[256];
    uint32_t w = adc_write_frame(), np = 0;
    const volatile uint16_t *ring = adc_ring();
    float lvl = 0;
    while(s_fm_rd != w) {
        float v = (float)ring[ADC_FRAME_CH * s_fm_rd] - 2048.0f;
        s_fm_rd = (s_fm_rd + 1u) % ADC_RING_FRAMES;
        s_fm_acc += v;
        if(++s_fm_n < 6) { continue; }
        float x = s_fm_acc / 6.0f;
        s_fm_acc = 0; s_fm_n = 0;
        s_fm_dc += 0.002f * (x - s_fm_dc);
        float y = (x - s_fm_dc) * 32.0f;                 /* 0.25 count/Hz -> 1 kHz deviation = 8000 */
        lvl += y * y;
        pcm[np++] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        aprs_rx_sample(pcm[np - 1]);
        sig_rx_sample(pcm[np - 1]);
        tone_rx_sample(pcm[np - 1]);
        if(np == sizeof pcm / sizeof pcm[0]) {
            if(lvl / (float)np > 1e5f && !g_dspb_fm_mute) { voice_rx_pcm(pcm, np); }   /* > ~40 Hz rms deviation */
            np = 0; lvl = 0;
        }
    }
    if(np && lvl / (float)np > 1e5f && !g_dspb_fm_mute) { voice_rx_pcm(pcm, np); }
}

static float (*s_fm_src)(void);
void dspb_fm_set_source(float (*src)(void)) { s_fm_src = src; }
float dsp_sinf(float x) { return sinf(x); }

bool dspb_fm_tx(bool on, uint16_t tone_hz, uint16_t ctcss_dhz)
{
#if !defined(CFW_DIGITAL_TX) && !defined(CFW_MCU_FM_TX)     /* CFW_MCU_FM_TX: board allows MCU audio TX in analog FM */
    (void)on; (void)tone_hz; (void)ctcss_dhz;
    return false;
#else
    if(on && !tone_hz && !s_fm_src) { return false; }
    if(on) {
        s_fm_tone = tone_hz; s_fm_ctcss = ctcss_dhz; s_fm_ph = s_fm_ct_ph = 0;
        dac_stream_start();
        s_last_r = s_soft_r = dac_read_pos();
        s_last_move_us = hal_micros();
        s_dac_moving = false;
        s_tx_w = (s_last_r + 240u) % DAC_RING;
        s_fm_tx = true;
    } else if(s_fm_tx) {
        s_fm_tx = false;
        dac_stream_stop();
    }
    return true;
#endif
}

static void fm_tx_poll(void)
{
    uint32_t t0 = hal_micros();
    uint32_t r = dac_read_pos();
    if(r != s_last_r) { s_last_r = r; s_last_move_us = t0; s_dac_moving = true; }
    if(!s_dac_moving) {
        uint32_t ms = (t0 - s_last_move_us) / 1000u;
        s_soft_r = (s_soft_r + ms * (ADC_FS / 1000u)) % DAC_RING;
        s_last_move_us += ms * 1000u;
        r = s_soft_r;
    }
    volatile uint16_t *ring = dac_ring();
    uint32_t free = (r + DAC_RING - 16u - s_tx_w) % DAC_RING;
    float w = 6.2831853f * (float)s_fm_tone / (float)ADC_FS, wc = 6.2831853f * (float)s_fm_ctcss / 10.0f / (float)ADC_FS;
    for(uint32_t i = 0; i < free; i++) {
        float hz = (s_fm_src ? s_fm_src() : 2500.0f * sinf(s_fm_ph)) +
                   (s_fm_ctcss ? 500.0f * sinf(s_fm_ct_ph) : 0.0f);   /* 2.5 kHz tone / AFSK source, 500 Hz CTCSS */
        s_fm_ph += w; if(s_fm_ph > 6.2831853f) { s_fm_ph -= 6.2831853f; }
        s_fm_ct_ph += wc; if(s_fm_ct_ph > 6.2831853f) { s_fm_ct_ph -= 6.2831853f; }
        ring[(s_tx_w + i) % DAC_RING] = (uint16_t)DAC_CODE((uint16_t)DSP_MOD_DAC(hz));
    }
    s_tx_w = (s_tx_w + free) % DAC_RING;
}

/* local tone (speaker alert, app/sig.c): the PWM plays but the receiver keeps running, so RX decoding goes on */
static bool s_fm_local;
void dspb_fm_local(bool on) { s_fm_local = on; }

void dspb_fm_poll(void)
{
    if(s_fm_tx) { fm_tx_poll(); }
    if(s_fm_rx && (!s_fm_tx || s_fm_local)) { fm_rx_poll(); }
}
