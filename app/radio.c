#include <string.h>
#include "radio.h"
#include "cfwcp.h"
#include "stockcp.h"
#include "hal.h"
#include "modem_core.h"
#include "dsp_backend.h"
#include "voice.h"
#include "rset.h"
#include "calllog.h"
#include "scan.h"
#include "gps.h"
#include "aprs.h"
#include "spk.h"
#include "sig.h"
#include "stockopt.h"
#include "stockrun.h"
#include "stockfx.h"
#include "emerg.h"
#include "battdisp.h"
#include "bcast.h"
#include "board.h"

radio_t g_radio;

/* TX frequency in effect: talkaround = the RX frequency (simplex on a repeater channel) */
uint32_t radio_tx_hz(const channel_t *c)
{
    if(!c->tx_hz) { return 0; }
    return c->talkaround ? c->rx_hz : c->tx_hz;
}

bool radio_is_simplex(const channel_t *c) { return !c->tx_hz || radio_tx_hz(c) == c->rx_hz; }

void radio_toggle_talkaround(void)
{
    channel_t *c = radio_edit();
    c->talkaround = !c->talkaround;
    radio_commit(true);
}

/* RF chip 0 = main receiver/transmitter (stock uses chip 0 for TX, FUN_0810ea14(x,1,0)). */
#define RF_MAIN 0

/* ---- persisted state (drivers/settings.c blob) ---- */
#define PERSIST_VERSION 9u
typedef struct {
    uint16_t channel;
    uint8_t  vfo;
    uint8_t  n_ovr;
    int16_t  zone;
    uint8_t  n_edits, pad;
    cp_edit_t edits[32];           /* radio menu edits when the codeplug has no journal space (rset.c) */
    msgs_store_t msgs;
    uint32_t step_hz;
    uint32_t my_id;
    char     my_call[9];
    uint8_t  rx_invert;
    uint16_t deemph_us;
    channel_t vfo_ch;
    ch_override_t ovr[CH_MAX_OVERRIDES];
} persist_t;
_Static_assert(sizeof(persist_t) <= SETTINGS_MAX, "persisted state too big for one settings record");

static persist_t s_p;      /* static: too big for the stack */

void radio_init(void)
{
    memset(&g_radio, 0, sizeof g_radio);
    g_radio.step_hz = 12500;
    g_radio.my_id = 9990201;
    strcpy(g_radio.my_call, "N0CALL");
    memset(&g_radio.vfo_ch, 0, sizeof g_radio.vfo_ch);
    strcpy(g_radio.vfo_ch.name, "VFO");
    g_radio.vfo_ch.rx_hz = g_radio.vfo_ch.tx_hz = 446000000;
    g_radio.vfo_ch.mode = CFW_MODE_FM;
    g_radio.vfo_ch.wide = true;
    g_radio.zone = -1;
    cfwcp_apply_radio();        /* CPS codeplug radio settings = defaults; persisted menu edits still win */
    rset_init();
    { uint32_t st = stockopt_step_hz(); if(st) { g_radio.step_hz = st; } }     /* stock Optional Settings Step */
    uint32_t n = settings_load(PERSIST_VERSION, &s_p, sizeof s_p);
    if(n >= offsetof(persist_t, ovr)) {
        if(s_p.zone >= -1 && s_p.zone < zone_count()) { g_radio.zone = s_p.zone; }
        rset_set_edits(s_p.edits, s_p.n_edits <= 32 ? s_p.n_edits : 0);
        msgs_import(&s_p.msgs);
        channels_set_overrides(s_p.ovr, s_p.n_ovr);
        if(s_p.channel < channels_count()) { g_radio.channel = s_p.channel; }
        g_radio.vfo = s_p.vfo & 1;
        if(s_p.step_hz) { g_radio.step_hz = s_p.step_hz; }
        if(s_p.my_id) { g_radio.my_id = s_p.my_id; }
        if(s_p.my_call[0]) { memcpy(g_radio.my_call, s_p.my_call, sizeof g_radio.my_call); g_radio.my_call[8] = 0; }
        g_radio.rx_invert = s_p.rx_invert & 1;
        g_radio.deemph_us = s_p.deemph_us <= 2000 ? s_p.deemph_us : 0;
        if(freq_in_band(s_p.vfo_ch.rx_hz) && s_p.vfo_ch.mode < CFW_MODE_COUNT) { g_radio.vfo_ch = s_p.vfo_ch; }
    }
    int sz, sc;                                  /* stock Default Startup Channel (Optional Settings Power-on) */
    if(stockopt_start_channel(&sz, &sc)) {
        if(sz >= 0 && sz < zone_count() && zone_channel(sz, sc) >= 0) { g_radio.zone = (int16_t)sz; g_radio.channel = zone_channel(sz, sc); }
        else if(sc >= 0 && sc < channels_count()) { g_radio.channel = sc; }
        g_radio.vfo = false;
    }
    channel_load(g_radio.channel, &g_radio.mem);
    radio_update_alias();
    g_radio.last_activity = hal_millis();
    gps_init();
    backlight_set((uint8_t)(rset_get(RS_BKL) + 1u));
}

/* talker alias sent with DMR calls: codeplug radio name, else the callsign */
void radio_update_alias(void)
{
    const cp_settings_t *st = cfwcp_settings();
    const char *src = (st && st->radio_name[0] && st->radio_name[0] != ' ') ? st->radio_name : g_radio.my_call;
    int n = st && src == st->radio_name ? 16 : 8, k = 0;
    for(int i = 0; i < n && src[i]; i++) { g_radio.alias[k++] = (src[i] >= 0x20 && src[i] < 0x7F) ? src[i] : '?'; }
    while(k && g_radio.alias[k - 1] == ' ') { k--; }
    g_radio.alias[k] = 0;
}

void radio_save(void)
{
    const ch_override_t *ovr;
    rset_save();                                   /* menu settings -> codeplug journal (if it has room) */
    memset(&s_p, 0, sizeof s_p);
    s_p.zone = g_radio.zone;
    if(!rset_in_codeplug()) {
        const cp_edit_t *e;
        int ne = rset_edits(&e);
        s_p.n_edits = (uint8_t)ne;
        memcpy(s_p.edits, e, sizeof e[0] * (size_t)ne);
    }
    msgs_export(&s_p.msgs);
    s_p.channel = (uint16_t)g_radio.channel;
    s_p.vfo = g_radio.vfo;
    s_p.step_hz = g_radio.step_hz;
    s_p.my_id = g_radio.my_id;
    memcpy(s_p.my_call, g_radio.my_call, sizeof s_p.my_call);
    s_p.rx_invert = g_radio.rx_invert;
    s_p.deemph_us = g_radio.deemph_us;
    s_p.vfo_ch = g_radio.vfo_ch;
    int n = channels_overrides(&ovr);
    s_p.n_ovr = (uint8_t)n;
    memcpy(s_p.ovr, ovr, sizeof ovr[0] * (size_t)n);
    settings_save(PERSIST_VERSION, &s_p, (uint32_t)(offsetof(persist_t, ovr) + sizeof ovr[0] * (size_t)n));
}

const channel_t *radio_current(void) { return g_radio.vfo ? &g_radio.vfo_ch : &g_radio.mem; }
channel_t *radio_edit(void) { return g_radio.vfo ? &g_radio.vfo_ch : &g_radio.mem; }

void radio_chan_cfg(cfw_chan_cfg_t *cfg)
{
    const channel_t *c = radio_current();
    memset(cfg, 0, sizeof *cfg);
    cfg->rx_hz = c->rx_hz;
    cfg->tx_hz = radio_tx_hz(c);
    cfg->direct = cfg->tx_hz == c->rx_hz;
    cfg->dmr_tdma_direct = c->dmr_tdma;
    cfg->my_id = g_radio.my_id;
    if(c->mode == CFW_MODE_DMR && !g_radio.vfo && c->src == CH_SRC_CODEPLUG) {   /* stock Radio ID List entry */
        uint32_t rid = stockcp_channel_radio_id(g_radio.channel);
        if(rid) { cfg->my_id = rid; }
    }
    if(c->mode == CFW_MODE_NXDN) {               /* NXDN unit ID: cfw settings, else stock NX Setting OwnId */
        const cp_settings_t *st = cfwcp_settings();
        uint16_t nx = st && st->nxdn_unit_id ? st->nxdn_unit_id : stockcp_nxdn_own_id();
        if(nx) { cfg->my_id = nx; }
    }
    memcpy(cfg->my_call, g_radio.my_call, sizeof cfg->my_call);
    cfg->talkgroup = c->talkgroup;
    cfg->color_code = c->color_code;
    cfg->slot = c->slot;
    cfg->nac = c->nac;
    cfg->wacn = c->wacn;
    cfg->sysid = c->sysid;
    cfg->ran = c->ran;
    cfg->nxdn96 = c->nxdn96;
    cfg->dgid = c->dgid;
    cfg->dgid_tx = c->dgid_tx;
    memcpy(cfg->ur, c->ur, sizeof cfg->ur);
    memcpy(cfg->rpt1, c->rpt1, sizeof cfg->rpt1);
    memcpy(cfg->rpt2, c->rpt2, sizeof cfg->rpt2);
    cfg->trunk = c->trunk;
    cfg->chan_step_hz = 12500;
}

/* power save (spk.c): receiver off / back on at the current frequency */
void radio_rx_power(bool on)
{
    const channel_t *c = radio_current();
    if(!on) { rf_idle(RF_MAIN); return; }
    rf_set_freq(RF_MAIN, modem_active() && modem_rx_hz() ? modem_rx_hz() : c->rx_hz);
    rf_rx_fm(RF_MAIN, c->mode == CFW_MODE_FM ? c->wide : false);
    rf_set_ctcss(RF_MAIN, c->mode == CFW_MODE_FM ? c->ctcss_dhz : 0);
}

void radio_apply(void)
{
    const channel_t *c = radio_current();
    if(g_radio.tx) { radio_ptt(false); }
    rf_set_freq(RF_MAIN, c->rx_hz);
    rf_rx_fm(RF_MAIN, c->mode == CFW_MODE_FM ? c->wide : false);
    rf_set_ctcss(RF_MAIN, c->mode == CFW_MODE_FM ? c->ctcss_dhz : 0);
    cfw_chan_cfg_t cfg;
    radio_chan_cfg(&cfg);
    dspb_set_opts(g_radio.rx_invert, g_radio.deemph_us);
    dac_set_power(radio_tx_power());
    dspb_fm_rx(c->mode == CFW_MODE_FM);
    g_radio.mode_ok = modem_set_mode(c->mode, &cfg);
    stockfx_channel();
}

void radio_commit(bool retune)
{
    if(!g_radio.vfo) { channel_store(g_radio.channel, &g_radio.mem); }
    if(retune) { radio_apply(); return; }
    cfw_chan_cfg_t cfg;
    radio_chan_cfg(&cfg);
    modem_configure(&cfg);
    rf_set_ctcss(RF_MAIN, radio_current()->mode == CFW_MODE_FM ? radio_current()->ctcss_dhz : 0);
}

void radio_goto(int ch)
{
    int n = channels_count();
    if(n <= 0) { return; }
    g_radio.channel = ((ch % n) + n) % n;
    channel_load(g_radio.channel, &g_radio.mem);
    radio_apply();
}

void radio_step(int dir)
{
    if(g_radio.vfo) {
        uint32_t f = g_radio.vfo_ch.rx_hz + (uint32_t)(dir * (int32_t)g_radio.step_hz);
        if(freq_in_band(f)) { radio_set_vfo_freq(f); }
        return;
    }
    if(g_radio.zone >= 0) {
        int n = zone_size(g_radio.zone);
        if(n <= 0) { return; }
        int k = zone_find(g_radio.zone, g_radio.channel);
        k = k < 0 ? (dir > 0 ? 0 : n - 1) : ((k + dir) % n + n) % n;
        int ch = zone_channel(g_radio.zone, k);
        if(ch >= 0) { radio_goto(ch); }
        return;
    }
    radio_goto(g_radio.channel + dir);
}

void radio_set_zone(int z)
{
    if(z < -1 || z >= zone_count()) { z = -1; }
    g_radio.zone = (int16_t)z;
    if(z >= 0 && zone_find(z, g_radio.channel) < 0) {
        int ch = zone_channel(z, 0);
        if(ch >= 0) { g_radio.vfo = false; radio_goto(ch); }
    }
}

void radio_set_target(uint32_t id, bool priv)
{
    channel_t *c = radio_edit();
    if(c->mode == CFW_MODE_YSF) { c->dgid = (uint8_t)(id > 99 ? 99 : id); }
    else { c->talkgroup = id; c->private_call = priv; }
    radio_commit(false);
}

/* Pre-flash audit F7: a HW=1 build transmits at Low until the user picks a TX power in this session (menu or the
 * power key), whatever the codeplug says, so the first key-ups on a real radio are at the lowest level. */
static bool s_pwr_user;

uint8_t radio_tx_power(void)
{
#ifdef CFW_HW_BUILD
    if(!s_pwr_user) { return 0; }
#endif
    return radio_current()->power & 3u;
}

void radio_set_power(uint8_t p)
{
    channel_t *c = radio_edit();
    c->power = p & 3u;
    s_pwr_user = true;
    if(!g_radio.vfo) { rset_set_channel_power(g_radio.channel, c->power); }
    else { radio_commit(false); }
    dac_set_power(radio_tx_power());
}

static void tx_note(const char *l1, const char *l2)
{
    g_radio.tx_note[0] = l1;
    g_radio.tx_note[1] = l2;
    g_radio.tx_note_seq++;
}

void radio_boot_ptt_lock(void)
{
    if(key_ptt_down()) { g_radio.tot_hit = true; tx_note("PTT HELD", "release PTT"); }
}

void radio_activity(void) { g_radio.last_activity = hal_millis(); }

void radio_set_vfo_freq(uint32_t hz)
{
    g_radio.vfo_ch.rx_hz = hz;
    g_radio.vfo_ch.tx_hz = stockopt_auto_rpt_tx(hz);   /* stock Auto Repeater (Optional Settings + offset list) */
    radio_apply();
}

void radio_toggle_vfo(void)
{
    g_radio.vfo = !g_radio.vfo;
    radio_apply();
}

/* Mode is a per-channel setting: in memory mode it is stored as an override of that channel. */
void radio_set_mode(cfw_mode_t m)
{
    if((unsigned)m >= CFW_MODE_COUNT) { return; }
    channel_t *c = radio_edit();
    if(c->mode == m) { return; }
    c->mode = m;
    if(m == CFW_MODE_FM) { c->wide = true; }
    channel_mode_defaults(c);
    radio_commit(true);
}

void radio_cycle_mode(int dir)
{
    int m = (int)radio_current()->mode + dir;
    radio_set_mode((cfw_mode_t)((m + CFW_MODE_COUNT) % CFW_MODE_COUNT));
}

void radio_ptt(bool down)
{
    const channel_t *c = radio_current();
    if(!down) { g_radio.tot_hit = false; }
    if(down && g_radio.tot_hit) { return; }        /* TX timeout: release PTT first */
    if(down && stockfx_locked()) { return; }         /* power-on password */
    if(down == g_radio.tx) { return; }
    if(down) { scan_stop(true); radio_activity(); spk_stop(); g_radio.enc_tx = 0; }
    uint32_t txf = radio_tx_hz(c);
    if(down && (txf == 0 || !freq_in_band(txf))) { return; }   /* RX-only channel */
    const char *why = down ? safety_tx_block(txf) : 0;     /* HW_NOTX, CLK ERR, no RF cal, forced stop */
    if(why) { tx_note("TX REFUSED", why); return; }
    if(down && !g_radio.vfo && c->src == CH_SRC_CODEPLUG) {   /* stock channel columns (app/stockopt.c) */
        so_chan_t so;
        stockopt_channel(g_radio.channel, &so);
        if(so.ptt_prohibit) { tx_note("TX PROHIBITED", "channel setting"); return; }
        /* squelch level 0 (Off) and monitor hold sq_open on without a signal: only the carrier counts then */
        bool sq_busy = g_radio.sq_open && !g_radio.monitor && rset_sql_level();
        if(so.busy_lock && (g_radio.carrier || sq_busy)) { tx_note("CHANNEL BUSY", "busy lock"); return; }
    }
    if(c->mode == CFW_MODE_FM) {
        if(down) {
            rf_set_freq(RF_MAIN, txf);
            rf_set_ctcss(RF_MAIN, c->ctcss_dhz);
            rf_tx_fm(RF_MAIN, true);
            dac_apc(true);                       /* PA control level, as stock */
            sig_tx_begin(c);                     /* PTT ID at the start (app/sig.c): sets the audio source */
            dspb_fm_tx(true, voice_test_tone(), c->ctcss_dhz);   /* test tone on the DAC (emulator FM test) */
        } else {
            if(sig_tx_end(c)) { return; }        /* PTT ID at the end: transmitter stays on until it is sent */
            dspb_fm_tx(false, 0, 0);
            dac_apc(false);
            rf_tx_fm(RF_MAIN, false);
            rf_set_freq(RF_MAIN, c->rx_hz);
            rf_rx_fm(RF_MAIN, c->wide);
            stockfx_ptt_end();
        }
        g_radio.tx = down;
        if(down) { g_radio.tx_since = hal_millis(); }
        return;
    }
    if(!g_radio.mode_ok) { return; }
#ifndef CFW_DIGITAL_TX
    if(down) { return; }                     /* HW build without HW_DTX=1: digital TX compiled out */
#endif
    if(down) { voice_tx_prepare(c->mode == CFW_MODE_P25P1 ? CFW_VOC_IMBE_88 : c->mode == CFW_MODE_DSTAR ? CFW_VOC_AMBE_DSTAR : CFW_VOC_AMBE2_49); }
    cfw_tx_req_t req = { .voice = true, .dst_id = c->talkgroup, .slot = c->slot, .color_code = c->color_code,
                         .private_call = c->private_call, .alias = g_radio.alias[0] && stockfx_alias_on(c) ? g_radio.alias : NULL };
    modem_ptt(down, &req);
    g_radio.tx = down && modem_transmitting();
    if(g_radio.tx) {
        g_radio.tx_since = hal_millis();
        calllog_add_tx(c->mode, c->mode == CFW_MODE_YSF ? c->dgid : c->talkgroup, c->private_call);
    }
}

/* emulator / test hooks: a script writes these (ELF symbols) to give two radios different identities */
volatile uint32_t g_dbg_my_id;
volatile char g_dbg_my_call[9];
/* per-channel test overrides (uint16, script writes value + 1; 0 = untouched), applied to the current channel,
 * not saved: g_dbg_talkaround 1 on / 2 off, g_dbg_cc DMR colour code, g_dbg_nac P25 NAC (0xF7E accept any),
 * g_dbg_ran NXDN RAN (0 any), g_dbg_dgid YSF DG-ID (0 any) */
volatile uint16_t g_dbg_talkaround, g_dbg_cc, g_dbg_nac, g_dbg_ran, g_dbg_dgid;

void radio_tick(void)
{
    sig_tick();
    stockfx_tick();                            /* tone squelch, idle tones, NOAA, auto scan ... (app/stockfx.c) */
    battdisp_tick();                           /* battery percentage (app/battdisp.c) */
    emerg_tick();                              /* emergency alarm, work alone (app/emerg.c) */
    stockrun_tick();                           /* stock common sections: rings, TOT pre-alert, roaming ... */
    if(g_dbg_my_id) { g_radio.my_id = g_dbg_my_id; g_dbg_my_id = 0; radio_commit(false); }
    if(g_dbg_my_call[0]) {
        for(int i = 0; i < 8; i++) { g_radio.my_call[i] = g_dbg_my_call[i]; }
        g_radio.my_call[8] = 0;
        g_dbg_my_call[0] = 0;
        radio_update_alias();
        radio_commit(false);
    }
    if(g_dbg_talkaround | g_dbg_cc | g_dbg_nac | g_dbg_ran | g_dbg_dgid) {
        channel_t *c = radio_edit();
        if(g_dbg_talkaround) { c->talkaround = g_dbg_talkaround == 1; g_dbg_talkaround = 0; }
        if(g_dbg_cc)   { c->color_code = (uint8_t)(g_dbg_cc - 1); g_dbg_cc = 0; }
        if(g_dbg_nac)  { c->nac = (uint16_t)(g_dbg_nac - 1); g_dbg_nac = 0; }
        if(g_dbg_ran)  { c->ran = (uint8_t)(g_dbg_ran - 1); g_dbg_ran = 0; }
        if(g_dbg_dgid) { c->dgid = (uint8_t)(g_dbg_dgid - 1); g_dbg_dgid = 0; }
        radio_apply();
    }
    static uint8_t n;
    if(!g_radio.tx && ++n >= 10) {             /* every 100 ms: RX signal status for the UI */
        n = 0;
        g_radio.rssi = rf_rssi(RF_MAIN);
        g_radio.carrier = rf_carrier(RF_MAIN);
        /* squelch (Settings > Squelch, stock SQL level), stock FUN_080d22f0 / D168 equivalent on RF chip 0: the
         * threshold is the stock calibrated one written to reg 0x49 (rf_set_sql / dac_sql_threshold). Closed -> open
         * after 3 polls with carrier and RSSI >= threshold (not with glitches > 8 at RSSI > 0x14A); open -> closed after
         * 3 polls without carrier and with RSSI < max(threshold - 12, 0x46) or noise > 0x50 (or glitches > 8 at RSSI
         * > 0xFA). Polled every 100 ms here (stock rate not known). */
        static int sql_lvl = -1;
        static uint8_t sql_n;
        int lvl = rset_sql_level();
        if(lvl != sql_lvl) { sql_lvl = lvl; rf_set_sql(lvl); }
        uint16_t thr = rf_sql_thr(), rs = g_radio.rssi, nz = rf_noise(RF_MAIN);
        uint8_t glitch = nz & 0xFu, noise = (uint8_t)(nz >> 8);
        bool open = g_radio.sq_open;
        if(g_radio.monitor || lvl == 0) {
            open = true;
            sql_n = 0;
        } else if(!open) {
            bool up = thr <= rs && !(glitch > 8 && rs > 0x14A);
            if(g_radio.carrier && up) { if(++sql_n > 2) { sql_n = 0; open = true; } }
            else { sql_n = 0; }
        } else {
            uint16_t lo = (uint16_t)(thr - 12u);
            if(lo < 0x46u) { lo = 0x46u; }
            bool keep = rs >= lo && noise <= 0x50u && !(glitch > 8 && rs > 0xFA);
            if(g_radio.carrier || keep) { sql_n = 0; }
            else if(++sql_n > 2) { sql_n = 0; open = false; }
        }
        g_radio.sq_open = open;
        g_dspb_fm_mute = !open || (!g_radio.monitor && lvl && !stockfx_tone_ok());
        static bool spk_muted;                     /* speaker follows squelch / tone squelch (FM audio comes from the RF chip) */
        bool sm = radio_current()->mode == CFW_MODE_FM && g_dspb_fm_mute && bcast_band() == BC_OFF && !spk_ps_sleeping();
        if(sm || spk_muted) {                      /* re-applied while muted: every retune routes the RF audio again */
            spk_muted = sm;
            if(radio_current()->mode == CFW_MODE_FM && bcast_band() == BC_OFF && !spk_ps_sleeping()) { audio_route(sm ? AUDIO_MUTE : AUDIO_RF); }
        }
        calllog_poll();
        if(modem_call_info()->voice && modem_active()) { radio_activity(); }
    }
    scan_tick();
    spk_tick();
    gps_poll();
    if(aprs_done() && g_radio.tx) { radio_ptt(false); }
    if(aprs_busy() && !g_radio.tx) { aprs_end(); }
    int kill = safety_take_kill();             /* SysTick forced the PA off (hal/safety.c): finish the stop here */
    if(kill) {
        if(g_radio.tx) { radio_ptt(false); }
        g_radio.tx = false;
        g_radio.tot_hit = true;                /* PTT must be released before the next TX */
        tx_note("TX STOPPED", kill == 1 ? "TX time limit" : "main loop stall");
    }
    if(g_radio.tx && rset_tot_s() && hal_millis() - g_radio.tx_since >= rset_tot_s() * 1000u) {
        radio_ptt(false);                      /* TX timeout */
        g_radio.tx = false;
        g_radio.tot_hit = true;
    }
    /* digital TX ends when the proto has sent its terminator */
    if(g_radio.tx && radio_current()->mode != CFW_MODE_FM && !modem_transmitting() && !key_ptt_down() &&
       !spk_vox_tx()) {
        g_radio.tx = false;
    }
}

/* user-entered P25 keys from the CPS codeplug (section 12, kinds 4 DES, 5 AES-256, 6 ADP) for proto_p25 RX
 * decryption, only while Settings > Encryption is on (like DMR / NXDN). The CPS never generates keys; nothing here
 * guesses one. */
bool cfw_p25_key(int i, uint8_t *algid, uint16_t *kid, const uint8_t **key, uint8_t *len)
{
    int n = 0;
    if(!rset_encryption()) { return false; }
    for(int k = 0; k < cfwcp_key_count(); k++) {
        const cp_key_t *p = cfwcp_key(k);
        if(!p || p->kind < 4 || p->kind > 6) { continue; }
        if(n++ != i) { continue; }
        *algid = p->algid ? p->algid : (p->kind == 4 ? 0x81 : p->kind == 5 ? 0x84 : 0xAA);
        *kid = p->key_id; *key = p->key; *len = p->len;
        return true;
    }
    return false;
}

/* ---- encryption (privacy keys) - Settings > Encryption, off by default ----
 * Encrypted transmissions are illegal on amateur bands (e.g. US 47 CFR 97.113(a)(4)); the radio refuses encrypted TX
 * on the amateur allocations below even when the setting is on (the call goes out in clear and the screen says so). */
static const uint32_t k_ham[][2] = {
    { 28000000u, 29700000u }, { 50000000u, 54000000u }, { 144000000u, 148000000u }, { 219000000u, 225000000u },
    { 420000000u, 450000000u },
};
bool cfw_ham_band(uint32_t hz)
{
    for(unsigned i = 0; i < sizeof k_ham / sizeof k_ham[0]; i++) { if(hz >= k_ham[i][0] && hz <= k_ham[i][1]) { return true; } }
    return false;
}

/* RX keyring: the current channel's key first (keyrings hold only a few keys), then the others in codeplug order */
bool cfw_crypt_key(int i, uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len)
{
    if(!rset_encryption()) { return false; }
    int n = 0, nk = cfwcp_key_count(), ck = radio_current()->key;
    for(int j = -1; j < nk; j++) {
        int k = j < 0 ? ck : j;
        if(j >= 0 && k == ck) { continue; }
        const cp_key_t *p = cfwcp_key(k);
        if(!p || p->kind < lo || p->kind > hi || !p->len) { continue; }
        if(n++ != i) { continue; }
        *kind = p->kind; *key_id = (uint8_t)p->key_id; *key = p->key; *len = p->len;
        return true;
    }
    return false;
}

bool cfw_crypt_tx(uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len)
{
    const channel_t *c = radio_current();
    g_radio.enc_tx = 0;
    if(!rset_encryption() || c->key == 0xFF) { return false; }
    const cp_key_t *p = cfwcp_key(c->key);
    if(!p || p->kind < lo || p->kind > hi || !p->len) { return false; }
    if(cfw_ham_band(radio_tx_hz(c))) { g_radio.enc_tx = 2; return false; }     /* amateur band: clear */
    *kind = p->kind; *key_id = (uint8_t)p->key_id; *key = p->key; *len = p->len;
    g_radio.enc_tx = 1;
    return true;
}

uint32_t cfw_crypt_iv(void)
{
    static uint32_t x;
    x ^= hal_micros() * 2654435761u ^ g_radio.my_id;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* DMR SMS (Menu > SMS): one unconfirmed data burst sequence on the current channel (cfw SMS format, proto_dmr.c) */
static const char *s_sms_err = "";
const char *radio_sms_error(void) { return s_sms_err; }

bool radio_send_sms(uint32_t dst, bool group, const char *text)
{
    const channel_t *c = radio_current();
    if(c->mode != CFW_MODE_DMR) { s_sms_err = "DMR channels only"; return false; }
    if(!radio_tx_hz(c)) { s_sms_err = "RX only channel"; return false; }
    if(g_radio.tx) { s_sms_err = "busy (TX)"; return false; }
    if(!g_radio.mode_ok) { s_sms_err = "no DMR modem"; return false; }
#ifndef CFW_DIGITAL_TX
    (void)dst; (void)group; (void)text;
    s_sms_err = "digital TX off (HW)";
    return false;
#else
    scan_stop(true);
    cfw_tx_req_t req = { .voice = false, .dst_id = dst, .slot = c->slot, .color_code = c->color_code,
                         .private_call = !group, .data = (const uint8_t *)text, .data_len = (uint16_t)strlen(text),
                         .data_format = rset_sms_mformat() ? 1u : 0u };
    modem_ptt(true, &req);
    g_radio.tx = modem_transmitting();
    if(!g_radio.tx) { s_sms_err = "modem refused"; return false; }
    g_radio.tx_since = hal_millis();
    radio_activity();
    return true;
#endif
}
