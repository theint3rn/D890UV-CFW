/* Audio and RX housekeeping settings that act on the radio (Menu > Settings):
 *
 *   Key beep      short tone on each key press, made by the MCU on its audio output (D890UV TIM4_CH4 / D168UV TMR4_CH4
 *                 PWM on PB9, the path the stock firmware plays its own tones through) with the speaker switched to
 *                 that source as stock (board audio_tone(): D168UV PC12 + PA7 as FUN_08070a70; D890UV FUN_080c32e8
 *                 source 0: RF audio switches off, PC7 low, amplifier PC12 on; no tone with the volume knob at 0).
 *                 Not while transmitting, receiving (squelch open) or sending APRS.
 *   Volume limit  stock Vol Level, applied where the stock firmware applies it (audio_volume_limit()): D168UV = SCT3288
 *                 baseband attenuation frame, D890UV = I2C volume potentiometer value table[limit][knob step] - both
 *                 sit in front of the speaker, so FM receive audio is capped too. Emulator builds also scale the
 *                 MCU's own audio (decoded voice, beeps) so tests can measure it; hardware builds leave that to the pot
 *                 / baseband (no double attenuation).
 *   VOX           hands-free TX from the radio's own detector (vox_hw_*: D168UV RF chip regs 0x12 / 0x13 + PD3,
 *                 D890UV UART4 NR/BT module status): 3 detections in a row (50 ms polls, stock: 3 polls) key the
 *                 channel, 1.5 s without voice releases it.
 *   Power save    when idle (no carrier, no TX / scan / monitor, no key for 5 s) the receiver is switched off and on
 *                 again: 1:1 = 200 ms off / 200 ms on, 2:1 = 400 ms off / 200 ms on (stock option names). A carrier
 *                 seen in an on window keeps it on.
 *   GPS           GPS on (stock setting Gps) -> the stock module set-up lines are sent once (gps_uart_send_cfg).
 *   Man down      app/mandown.c (polled from here).
 * Emulator-only debug: g_spk_dbg (beeps played, VOX key-ups, power-save off windows). */
#include <string.h>
#include "spk.h"
#include "hal.h"
#include "board.h"
#include "radio.h"
#include "rset.h"
#include "scan.h"
#include "aprs.h"
#include "voice.h"
#include "dsp_backend.h"
#include "modem.h"
#include "modem_core.h"
#include "mandown.h"

volatile uint32_t g_spk_dbg[4];             /* beeps, VOX key-ups, VOX releases, power-save sleeps */

/* MCU-side gain: emulator builds only (see Volume limit above) */
static uint16_t mcu_gain(void)
{
#ifdef CFW_EMU
    return rset_volume_q8();
#else
    return 256u;
#endif
}

/* ---------------- beep ---------------- */
static bool s_beep;
static uint32_t s_beep_end;
static float s_ph, s_w, s_amp;

static float beep_src(void)
{
    s_ph += s_w;
    if(s_ph > 6.2831853f) { s_ph -= 6.2831853f; }
    return s_amp * dsp_sinf(s_ph);
}

void spk_beep(uint16_t hz, uint16_t ms)
{
    if(s_beep || g_radio.tx || (g_radio.sq_open && g_radio.carrier) || aprs_busy() || dspb_tx_active()) { return; }
    s_w = 6.2831853f * (float)hz / (float)ADC_FS;
    s_ph = 0;
    if(audio_tone_muted()) { return; }                   /* D890UV: volume knob at 0 - stock plays nothing either */
    s_amp = 1500.0f * (float)mcu_gain() / 256.0f;         /* "deviation" units of the FM TX ring = DAC swing */
    dspb_fm_set_source(beep_src);
    if(!dspb_fm_tx(true, 0, 0)) { dspb_fm_set_source(NULL); return; }
    audio_tone(true);
    s_beep = true;
    s_beep_end = hal_millis() + ms;
    g_spk_dbg[0]++;
}

void spk_stop(void)
{
    if(!s_beep) { return; }
    s_beep = false;
    dspb_fm_tx(false, 0, 0);
    dspb_fm_set_source(NULL);
    audio_tone(false);
}

void spk_key(void)
{
    if(rset_get(RS_BEEP)) { spk_beep(1000, 40); }
}

/* ---------------- VOX ---------------- */
static bool s_vox_tx;
static uint8_t s_vox_cfg = 0xFF, s_vox_hits;
static uint32_t s_vox_last, s_vox_poll;
bool spk_vox_tx(void) { return s_vox_tx; }

static void vox_tick(uint32_t now)
{
    uint8_t lvl = (uint8_t)(rset_get(RS_VOX) & 3u);
    if(lvl != s_vox_cfg) { s_vox_cfg = lvl; s_vox_hits = 0; vox_hw_config(lvl); }
    if(s_vox_tx && !g_radio.tx) { s_vox_tx = false; }           /* ended elsewhere (TOT, PTT) */
    if(!lvl) { if(s_vox_tx) { radio_ptt(false); s_vox_tx = false; } return; }
    if(now - s_vox_poll < 50u) { return; }
    s_vox_poll = now;
    bool voice = vox_hw_active();
    if(voice) { s_vox_last = now; if(s_vox_hits < 3) { s_vox_hits++; } } else { s_vox_hits = 0; }
    if(!g_radio.tx && s_vox_hits >= 3 && !g_radio.tot_hit && !scan_active() && !aprs_busy()) {
        radio_ptt(true);
        if(g_radio.tx) { s_vox_tx = true; g_spk_dbg[1]++; }
    } else if(s_vox_tx && now - s_vox_last > 1500u) {
        radio_ptt(false);
        s_vox_tx = false;
        g_spk_dbg[2]++;
    }
}

/* ---------------- volume limit / GPS set-up ---------------- */
static uint8_t s_vol_cfg = 0xFF;
static bool s_gps_cfg;
static uint32_t s_vol_poll, s_gps_poll;

static void hw_tick(uint32_t now)
{
    uint8_t mv = rset_get(RS_MAXVOL);
    if(mv != s_vol_cfg && bb_ready()) { s_vol_cfg = mv; audio_volume_limit(mv); }
    if(now - s_vol_poll >= 50u) { s_vol_poll = now; audio_poll(); }
    if(now - s_gps_poll >= 1000u || s_gps_poll == 0) {            /* codeplug lookup: once a second */
        s_gps_poll = now | 1u;
        bool gps = rset_gps_on();
        if(gps && !s_gps_cfg) { gps_uart_send_cfg(); }
        s_gps_cfg = gps;
    }
}

/* ---------------- power save ---------------- */
static bool call_on(void)
{
    const cfw_call_info_t *ci = modem_active() ? modem_call_info() : NULL;
    return ci && ci->voice;
}

static bool s_ps_off;
static uint32_t s_ps_t;
bool spk_ps_sleeping(void) { return s_ps_off; }

static void ps_wake(void)
{
    if(!s_ps_off) { return; }
    s_ps_off = false;
    radio_rx_power(true);
}

static void ps_tick(uint32_t now)
{
    uint8_t ps = rset_get(RS_PSAVE);
    bool idle = ps && !g_radio.tx && !g_radio.sq_open && !g_radio.monitor && !scan_active() && !aprs_busy() &&
                !s_beep && !call_on() && !radio_current()->trunk && now - g_radio.last_activity > 5000u;
    if(!idle) { ps_wake(); s_ps_t = now; return; }
    uint32_t off_ms = ps == 1 ? 200u : 400u, on_ms = 200u;
    if(s_ps_off) {
        if(now - s_ps_t >= off_ms) { ps_wake(); s_ps_t = now; }
    } else if(now - s_ps_t >= on_ms) {
        if(rf_carrier(0)) { s_ps_t = now; return; }       /* something on air: stay on */
        s_ps_off = true;
        s_ps_t = now;
        radio_rx_power(false);
        g_spk_dbg[3]++;
    }
}

void spk_tick(void)
{
    uint32_t now = hal_millis();
    if(s_beep && (int32_t)(now - s_beep_end) >= 0) { spk_stop(); }
    voice_set_gain(mcu_gain());
    vox_tick(now);
    hw_tick(now);
    ps_tick(now);
    mandown_tick(now);
}
