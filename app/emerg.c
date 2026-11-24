/* Emergency alarm and work alone, stock CPS "Alarm Setting" (table 64 of the codeplug, app/stockdlg.c).
 *
 *   Started by: key function "Alarm", Menu > Radio extras > Emergency, man down (app/mandown.c), work alone timeout.
 *   The Digital half of the dialog applies on DMR channels, the Analog half on every other channel.
 *   Type 0 Alarm        local alarm only (red popup + two-tone) for Alarm Time
 *        Digital 1 / 2  transmit, no local alarm (Transpond+Background, Transpond+NoLocalAlarm); 3 both
 *        Analog  1      transmit the microphone, silent locally (Transpond+Background); 2 alarm tone on air
 *                       (Transpond+Alarm); 3 alarm tone on air and local alarm (Both)
 *   Transmit: Duration of TX on, Duration of RX off, Emergency Cycle times (0 = until cancelled); ENI Send Select
 *   Assigned Channel = on the Emergency Channel (when its mode fits), else on the selected one; DMR calls go to the
 *   dialog's TG / DMR ID (0 = the channel's own target). D168UV signalling type MDC1200: an MDC emergency packet (own
 *   ID) opens the first analog transmission. Every transmission goes through radio_ptt() (all TX gates).
 *   HW builds: the transmit part is not compiled (nothing transmits by itself there); transmit types alarm locally.
 *   Any key cancels.
 *   Work Alone (channel column): no key (or no PTT, Work Alone Response = Voice Transmit) for Response Time -> warning
 *   beeps for Warning Time -> emergency. */
#include <string.h>
#include "emerg.h"
#include "stockdlg.h"
#include "stockopt.h"
#include "radio.h"
#include "cfwcp.h"
#include "sig.h"
#include "spk.h"
#include "ui_int.h"
#include "gfx.h"
#include "hal.h"
#include "modem_core.h"
#include "dsp_backend.h"

volatile uint32_t g_emerg_dbg[8];
enum { ED_STATE, ED_START, ED_TX, ED_CYCLE, ED_END, ED_WARN, ED_RX, ED_CANCEL };

static bool s_on, s_local;
static char s_why[16];
static uint32_t s_local_end, s_beep_t;          /* 0 end = no limit */
static uint8_t s_beep_k;

bool emerg_active(void) { return s_on; }

#ifndef CFW_HW_BUILD
enum { TX_NONE, TX_KEY, TX_ON, TX_WAIT };
static uint8_t s_tx, s_tone, s_mdc, s_tries;
static uint32_t s_t, s_tx_ms, s_rx_ms, s_cycles, s_ncyc;
static int s_back = -1;
static bool s_back_vfo, s_moved, s_emcall;
static channel_t s_keep;
static float s_ph;

static float alarm_src(void)                    /* 1000 / 1750 Hz, 0.2 s each */
{
    static uint32_t k;
    float f = (k++ / (ADC_FS / 5u)) & 1u ? 1750.0f : 1000.0f;
    s_ph += 6.2831853f * f / (float)ADC_FS;
    if(s_ph > 6.2831853f) { s_ph -= 6.2831853f; }
    return 2500.0f * dsp_sinf(s_ph);
}

static void tx_down(void)
{
    if(!g_radio.tx) { radio_ptt(false); return; }  /* clears a TX timeout */
    if(s_tone && radio_current()->mode == CFW_MODE_FM) { dspb_fm_set_source(NULL); }
    radio_ptt(false);
}

static void tx_start(bool dig)
{
    int type = (int)stockdlg_num(SD_T_ALARM, dig ? AL_D_TYPE : AL_A_TYPE, 0);
    s_tx = TX_NONE;
    if(!type) { return; }
    s_tone = !dig && type >= 2;
    s_mdc = !dig && stockdlg_num(SD_T_ALARM, AL_SIGTYPE, 0) == 3u;
    s_tx_ms = stockdlg_num(SD_T_ALARM, dig ? AL_D_TX : AL_A_TX, 31) * 1000u;
    s_rx_ms = stockdlg_num(SD_T_ALARM, dig ? AL_D_RX : AL_A_RX, 31) * 1000u;
    s_cycles = stockdlg_num(SD_T_ALARM, dig ? AL_D_CYCLE : AL_A_CYCLE, 0);
    s_ncyc = 0; s_tries = 0;
    if(s_tx_ms < 1000u) { s_tx_ms = 1000u; }
    if(s_rx_ms < 1000u) { s_rx_ms = 1000u; }
    s_back = g_radio.channel; s_back_vfo = g_radio.vfo; s_moved = false;
    if(stockdlg_num(SD_T_ALARM, dig ? AL_D_SEND : AL_A_SEND, 0) == 0) {        /* Assigned Channel */
        int ch = (int)stockdlg_num(SD_T_ALARM, dig ? AL_D_CHAN : AL_A_CHAN, 1) - 1;
        if(ch >= 0 && ch < channels_count()) {
            channel_t c;
            channel_load(ch, &c);
            if((c.mode == CFW_MODE_DMR) == dig && (dig || c.mode == CFW_MODE_FM) && (g_radio.vfo || ch != g_radio.channel)) {
                s_moved = true;
                g_radio.vfo = false;
                radio_goto(ch);
            }
        }
    }
    const channel_t *c = radio_current();
    if(c->mode != CFW_MODE_DMR && c->mode != CFW_MODE_FM) { return; }
    s_keep = *c;
    if(c->mode == CFW_MODE_DMR) {                 /* target for the alarm calls; restored when the alarm ends */
        uint32_t id = stockdlg_num(SD_T_ALARM, AL_D_ID, 0), ct = stockdlg_num(SD_T_ALARM, AL_D_CALL, 0);
        channel_t *e = g_radio.vfo ? &g_radio.vfo_ch : &g_radio.mem;
        if(ct == 2u) { e->talkgroup = 16777215u; e->private_call = false; }
        else if(id) { e->talkgroup = id; e->private_call = ct == 0; }
        s_emcall = true;
        radio_apply();
    }
    s_tx = TX_KEY;
    s_t = hal_millis() + 300u;
}

static void tx_stop(void)
{
    if(s_tx == TX_NONE) { return; }
    if(s_tx == TX_ON) { tx_down(); }
    s_tx = TX_NONE;
    s_emcall = false;
    if(s_moved) { g_radio.vfo = s_back_vfo; radio_goto(s_back); }
    else if(s_keep.mode == CFW_MODE_DMR) {
        channel_t *e = g_radio.vfo ? &g_radio.vfo_ch : &g_radio.mem;
        e->talkgroup = s_keep.talkgroup; e->private_call = s_keep.private_call;
        radio_apply();
    }
}

static void tx_tick(uint32_t now)
{
    if(s_tx == TX_NONE || (int32_t)(now - s_t) < 0) { return; }
    if(s_tx == TX_KEY) {
        if(sig_busy() || g_radio.tx) { return; }
        if(s_mdc && !s_ncyc && radio_current()->mode == CFW_MODE_FM) {
            s_mdc = 0;
            if(sig_send_mdc(0x00, 0x80, (uint16_t)(g_radio.my_id & 0xFFFFu)) == NULL) { s_t = now + 200u; return; }
        }
        spk_stop();
        radio_ptt(true);
        if(!g_radio.tx) {                         /* busy / refused: once a second, 5 times, then the local alarm stays */
            s_t = now + 1000u;
            if(++s_tries >= 5) { tx_stop(); }
            return;
        }
        s_tries = 0;
        if(s_tone && radio_current()->mode == CFW_MODE_FM) {
            dspb_fm_set_source(alarm_src);
            dspb_fm_tx(true, 0, radio_current()->ctcss_dhz);
        }
        g_emerg_dbg[ED_TX]++;
        s_tx = TX_ON;
        s_t = now + s_tx_ms;
    } else if(s_tx == TX_ON) {
        tx_down();
        g_emerg_dbg[ED_CYCLE]++;
        s_ncyc++;
        s_tx = TX_WAIT;
        s_t = now + s_rx_ms;
    } else {
        if(s_cycles && s_ncyc >= s_cycles) { tx_stop(); return; }
        s_tx = TX_KEY;
    }
}
static bool tx_running(void) { return s_tx != TX_NONE; }
bool dmr_tx_emergency(void) { return s_emcall; }
#else
static bool tx_running(void) { return false; }
bool dmr_tx_emergency(void) { return false; }
#endif

void emerg_start(const char *why, bool local)
{
    if(s_on) { return; }
    uint32_t now = hal_millis();
    bool dig = radio_current()->mode == CFW_MODE_DMR;
    int type = (int)stockdlg_num(SD_T_ALARM, dig ? AL_D_TYPE : AL_A_TYPE, 0);
    strncpy(s_why, why, sizeof s_why - 1);
    s_on = true;
    s_local = local && (type == 0 || type == 3);
#ifndef CFW_HW_BUILD
    tx_start(dig);
    if(local && type && s_tx == TX_NONE) { s_local = true; }    /* cannot transmit in this mode */
#else
    s_local = local;                                /* no transmit part: always alarm locally */
#endif
    uint32_t s = stockdlg_num(SD_T_ALARM, dig ? AL_D_TIME : AL_A_TIME, 20);
    s_local_end = s ? ((now + s * 1000u) | 1u) : 0;
    s_beep_t = now - 1000u;
    g_emerg_dbg[ED_START]++;
    g_emerg_dbg[ED_STATE] = 1;
    if(!s_local && local) { ui_popup("EMERGENCY", s_why, C_RED); }
}

void emerg_cancel(void)
{
    if(!s_on) { return; }
    s_on = false;
#ifndef CFW_HW_BUILD
    tx_stop();
#endif
    spk_stop();
    g_emerg_dbg[ED_CANCEL]++;
    g_emerg_dbg[ED_STATE] = 0;
}

/* ------------------------------------------------------------------ work alone */
static uint32_t s_wa_t, s_wa_warn;               /* last response; warning started (0 = not warning) */
static int s_wa_ch = -2;
static uint32_t s_wa_gen;
static bool s_wa_on;

bool emerg_warning(void) { return s_wa_warn != 0; }
void emerg_key(void)
{
    if(stockdlg_num(SD_T_ALARM, AL_W_MODE, 0) == 0) { s_wa_t = hal_millis(); s_wa_warn = 0; }
}

static void work_alone(uint32_t now)
{
    int ch = g_radio.vfo || radio_current()->src != CH_SRC_CODEPLUG || !cfwcp_valid() ? -1 : g_radio.channel;
    if(ch != s_wa_ch || cfwcp_generation() != s_wa_gen) {
        so_chan_t o;
        s_wa_ch = ch; s_wa_gen = cfwcp_generation();
        s_wa_on = false;
        if(ch >= 0) { stockopt_channel(ch, &o); s_wa_on = o.work_alone; }
        s_wa_t = now; s_wa_warn = 0;
    }
    if(!s_wa_on || s_on) { s_wa_t = now; s_wa_warn = 0; return; }
    if(g_radio.tx) { s_wa_t = now; s_wa_warn = 0; return; }
    uint32_t resp = (stockdlg_num(SD_T_ALARM, AL_W_RESP, 0) + 1u) * 60000u;
    uint32_t warn = (stockdlg_num(SD_T_ALARM, AL_W_WARN, 0) + 1u) * 1000u;
    if(!s_wa_warn) {
        if(now - s_wa_t >= resp) { s_wa_warn = now | 1u; s_beep_t = now - 1000u; g_emerg_dbg[ED_WARN]++; }
        return;
    }
    if(now - s_wa_warn >= warn) { s_wa_warn = 0; s_wa_t = now; emerg_start("WORK ALONE", true); return; }
    if(now - s_beep_t >= 500u) {
        s_beep_t = now;
        spk_beep(1200, 150);
        ui_popup("WORK ALONE", stockdlg_num(SD_T_ALARM, AL_W_MODE, 0) ? "press PTT" : "press a key", C_ORANGE);
    }
}

/* ------------------------------------------------------------------ receive (DMR emergency calls) */
static uint32_t s_rx_src, s_rx_t;
void dmr_rx_emergency(uint32_t src, uint32_t dst)
{
    (void)dst;
    uint32_t now = hal_millis();
    if(!stockdlg_num(SD_T_ALARM, AL_D_RECV, 0)) { return; }
    if(src == s_rx_src && s_rx_t && now - s_rx_t < 15000u) { return; }        /* once per call */
    s_rx_src = src; s_rx_t = now | 1u;
    g_emerg_dbg[ED_RX]++;
    char t[20];
    fmt_u(fmt_s(t, "from "), src, 1);
    g_radio.tx_note[0] = "EMERGENCY";
    static char s_note[20];
    strcpy(s_note, t);
    g_radio.tx_note[1] = s_note;
    g_radio.tx_note_seq++;
    if(!g_radio.tx) { sig_alert(1); }
}

void emerg_tick(void)
{
    uint32_t now = hal_millis();
    work_alone(now);
    if(!s_on) { return; }
#ifndef CFW_HW_BUILD
    tx_tick(now);
#endif
    if(s_local && s_local_end && (int32_t)(now - s_local_end) >= 0) { s_local = false; }
    if(s_local && now - s_beep_t >= 400u) {
        s_beep_t = now;
        if(!g_radio.tx) { spk_beep(++s_beep_k & 1u ? 1750 : 1000, 250); }
        ui_popup("EMERGENCY", s_why, C_RED);
    }
    if(!s_local && !tx_running()) { s_on = false; g_emerg_dbg[ED_STATE] = 0; g_emerg_dbg[ED_END]++; }
}
