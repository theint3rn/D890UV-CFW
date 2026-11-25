/* Stock CPS settings at run time, part 2 (see stockfx.h). */
#include <string.h>
#include "stockfx.h"
#include "stockopt.h"
#include "stockcp.h"
#include "emerg.h"
#include "radio.h"
#include "rset.h"
#include "scan.h"
#include "bcast.h"
#include "gps.h"
#include "aprs.h"
#include "sig.h"
#include "spk.h"
#include "cfwcp.h"
#include "hal.h"
#include "dbg.h"
#include "gfx.h"
#include "modem_core.h"
#include "dsp_backend.h"
#include "voice.h"

volatile uint32_t g_stockfx_dbg[8];
enum { FX_IDLE, FX_TBST, FX_NOAA, FX_OPEN, FX_CLOSE, FX_SCAN, FX_BEACON, FX_PAUSE };
enum { OC_PASSWORD = 7, OC_WORKFMCH = 24, OC_FMEN = 28, OC_FMMON = 37, OC_TBST = 40, OC_PWCHAR = 48, OC_IDLETONE = 51,
       OC_BTON = 90, OC_BTMIC = 95, OC_BTSPK = 96, OC_CHCOLOR = 128, OC_WXALARM = 141, OC_STDCOLOR = 159,
       OC_ANAIDLE = 168 };

/* ------------------------------------------------------------------ tone squelch (CTCSS / DCS receive)
 * The 8 kHz tap is averaged 8:1 (1 kHz); CTCSS: Goertzel at the wanted tone and at the two neighbours of the standard
 * list, 0.25 s blocks (4 Hz bins): the tone must be the strongest of the three and hold >= 25 % of the energy below
 * ~300 Hz (a first-order high-pass cannot tell voice apart, so the share is taken after a 2-pole 250 Hz low-pass).
 * DCS: 134.4 bit/s NRZ, bit clock from the zero crossings, the last 23 bits compared with the 23-bit word (<= 1 error).
 * Open after 1 good CTCSS block / 1 DCS word, closed after 2 bad blocks / 0.45 s without a word. */
static struct {
    uint16_t tone;                     /* codeplug value: 0 none, CTCSS 0.1 Hz, 0x8000 | code DCS (b14 inverted) */
    float c[3], q1[3], q2[3], en, l1, l2;
    float acc, bt;                     /* DCS bit clock 0..1 */
    int32_t sum;
    uint16_t n; uint8_t dec, bad; bool ok, last;
    uint32_t sr, word, age;
} s_t;

/* 2-pole Butterworth low-pass, 250 Hz at 8 kHz */
static float s_lx1, s_lx2, s_ly1, s_ly2;
static float lp250(float x)
{
    float y = 0.0084427f * (x + 2.0f * s_lx1 + s_lx2) + 1.7237757f * s_ly1 - 0.7575467f * s_ly2;
    s_lx2 = s_lx1; s_lx1 = x; s_ly2 = s_ly1; s_ly1 = y;
    return y;
}

uint32_t stockfx_dcs_word(uint16_t code)
{
    uint32_t data = 0x800u | (code & 0x1FFu), r = data << 11;       /* 12 data bits: code + 100b */
    for(int i = 22; i >= 11; i--) { if(r >> i & 1u) { r ^= 0xC75u << (i - 11); } }   /* Golay (23,12) */
    return (r << 12) | data;                                          /* sent bit 0 first */
}

static void tone_set(uint16_t tone)
{
    if(s_t.tone == tone) { return; }
    s_t.tone = 0;                                  /* the tap runs in the same context (main loop) */
    memset(&s_t, 0, sizeof s_t);
    s_lx1 = s_lx2 = s_ly1 = s_ly2 = 0;
    if(tone && !(tone & 0x8000u)) {
        uint16_t lo = 0, hi = 0xFFFF;
        for(int i = 0; i < ctcss_count(); i++) {
            uint16_t f = ctcss_at(i);
            if(f < tone && f > lo) { lo = f; }
            if(f > tone && f < hi) { hi = f; }
        }
        if(!lo) { lo = (uint16_t)(tone - 25u); }
        if(hi == 0xFFFF) { hi = (uint16_t)(tone + 50u); }
        uint16_t f[3] = { tone, lo, hi };
        for(int k = 0; k < 3; k++) { s_t.c[k] = 2.0f * dsp_sinf(6.2831853f * (float)f[k] / 10000.0f + 1.5707963f); }
    } else if(tone) {
        s_t.word = stockfx_dcs_word(tone & 0x1FFu);
        if(tone & 0x4000u) { s_t.word ^= 0x7FFFFFu; }
        s_t.age = 1000;
    }
    s_t.tone = tone;
}

bool stockfx_tone_used(void) { return s_t.tone != 0; }
bool stockfx_tone_ok(void) { return !s_t.tone || s_t.ok; }

static void tone_result(bool ok)
{
    if(ok) { s_t.bad = 0; if(!s_t.ok) { s_t.ok = true; g_stockfx_dbg[FX_OPEN]++; } }
    else if(s_t.ok && ++s_t.bad >= 2) { s_t.ok = false; g_stockfx_dbg[FX_CLOSE]++; }
}

static void ctcss_1k(float x)
{
    for(int k = 0; k < 3; k++) { float q = x + s_t.c[k] * s_t.q1[k] - s_t.q2[k]; s_t.q2[k] = s_t.q1[k]; s_t.q1[k] = q; }
    s_t.en += x * x;
    if(++s_t.n < 250) { return; }
    float p[3], e = s_t.en * 125.0f;                /* single tone: p = e */
    for(int k = 0; k < 3; k++) {
        p[k] = s_t.q1[k] * s_t.q1[k] + s_t.q2[k] * s_t.q2[k] - s_t.c[k] * s_t.q1[k] * s_t.q2[k];
        s_t.q1[k] = s_t.q2[k] = 0;
    }
    s_t.en = 0; s_t.n = 0;
    tone_result(e > 1.0e8f && p[0] > 0.25f * e && p[0] >= p[1] && p[0] >= p[2]);   /* > ~100 Hz deviation */
}

static void dcs_1k(float x)
{
    bool b = x > 0;
    s_t.bt += 0.1344f;
    s_t.acc += x;
    if(b != s_t.last) {                              /* zero crossing = bit edge: pull the clock a quarter of the error */
        s_t.last = b;
        float err = s_t.bt < 0.5f ? s_t.bt : s_t.bt - 1.0f;
        s_t.bt -= 0.25f * err;
    }
    s_t.age++;
    if(s_t.bt >= 1.0f) {
        s_t.bt -= 1.0f;
        s_t.sr = (s_t.sr >> 1) | (s_t.acc > 0 ? 0x400000u : 0);     /* first bit sent ends in bit 0 */
        s_t.acc = 0;
        uint32_t d = s_t.sr ^ s_t.word;
        int e = 0;
        while(d && e < 3) { d &= d - 1u; e++; }
        if(e <= 1) { s_t.age = 0; if(!s_t.ok) { s_t.ok = true; g_stockfx_dbg[FX_OPEN]++; } }
    }
    if(s_t.ok && s_t.age > 450) { s_t.ok = false; g_stockfx_dbg[FX_CLOSE]++; }
}

/* NOAA 1050 Hz alert tone: Goertzel, 0.1 s blocks at 8 kHz */
static int s_noaa = -1;
static float s_wq1, s_wq2, s_we;
static uint16_t s_wn; static uint8_t s_whits; static bool s_walarm;

void tone_rx_sample(int16_t x)
{
    if(s_noaa >= 0) {
        float q = (float)x + 1.3568f * s_wq1 - s_wq2;               /* 2 cos(2 pi 1050 / 8000) */
        s_wq2 = s_wq1; s_wq1 = q; s_we += (float)x * (float)x;
        if(++s_wn >= 800) {
            float p = s_wq1 * s_wq1 + s_wq2 * s_wq2 - 1.3568f * s_wq1 * s_wq2, e = s_we * 400.0f;
            bool hit = s_we > 4.0e8f && p > 0.5f * e;                 /* > ~90 Hz rms deviation, mostly 1050 Hz */
            s_wq1 = s_wq2 = s_we = 0; s_wn = 0;
            s_whits = hit ? (uint8_t)(s_whits < 200 ? s_whits + 1 : 200) : 0;
            if(s_whits == 30) { s_walarm = true; }                    /* 3 s */
        }
    }
    if(!s_t.tone) { return; }
    float y = lp250((float)x);
    s_t.l1 += y;
    if(++s_t.dec < 8) { return; }
    y = s_t.l1 * 0.125f; s_t.l1 = 0; s_t.dec = 0;
    if(s_t.tone & 0x8000u) { dcs_1k(y); } else { ctcss_1k(y); }
}

static uint16_t chan_rx_tone(int ch)
{
    uint32_t n = 0;
    const cp_channel_t *t = cfwcp_section(CP_SEC_CHANNELS, sizeof *t, &n);
    return t && ch >= 0 && (uint32_t)ch < n ? t[ch].rx_tone : 0;
}

/* ------------------------------------------------------------------ channel */
static bool s_chg;
static so_chan_t s_so;
void stockfx_channel(void)
{
    const channel_t *c = radio_current();
    bool cp = !g_radio.vfo && c->src == CH_SRC_CODEPLUG && cfwcp_valid();
    memset(&s_so, 0, sizeof s_so);
    if(cp) { stockopt_channel(g_radio.channel, &s_so); }
    uint16_t tone = cp && c->mode == CFW_MODE_FM && s_so.sql_tone ? chan_rx_tone(g_radio.channel) : 0;
    tone_set(tone);
    s_chg = true;
}

bool stockfx_alias_on(const channel_t *c)
{
    return c->src != CH_SRC_CODEPLUG || g_radio.vfo || !cfwcp_valid() || s_so.alias;
}

/* ------------------------------------------------------------------ colours (stock lists, index 0 = first) */
uint16_t stockfx_color(uint8_t which)
{
    static const uint16_t k_name[7] = { C_ORANGE, C_RED, C_YELLOW, C_GREEN, C_CYAN, 0x421F, C_WHITE };
    static const uint16_t k_std[8] = { C_WHITE, C_GREY, C_ORANGE, C_RED, C_YELLOW, C_GREEN, C_CYAN, 0x421F };   /* Black -> grey */
    if(!cfwcp_valid()) { return which ? C_WHITE : C_CYAN; }
    uint32_t v = stockopt_opt(which ? OC_STDCOLOR : OC_CHCOLOR, 0);
    return which ? k_std[v & 7u] : k_name[v < 7u ? v : 0];
}

/* ------------------------------------------------------------------ power-on password */
/* Nobody may be locked out: only Password = On (1) with 1..8 digits locks (the stock length; anything else could not be
 * typed, or is not a stock value). HW builds ignore the password (and say so once): a forgotten or damaged one could only
 * be cleared with SWD or a reflash. make HW=1 HW_SAVE=1 HW_PASSWORD=1 enforces it: that build has the CPS link, which
 * answers on the password screen too, and a codeplug without the password opens the radio at once (stockfx_lock_recheck). */
#if defined(CFW_HW_BUILD) && !defined(CFW_HW_PASSWORD)
#define LOCK_OFF 1
#else
#define LOCK_OFF 0
#endif
#define PW_MAX 8
static char s_pw[PW_MAX + 2], s_in[PW_MAX + 2];
static uint8_t s_lock;                         /* 0 not looked yet, 1 locked, 2 open */
bool stockfx_locked(void)
{
    if(!s_lock) {
        s_lock = 2;
        s_in[0] = 0;
        char t[16];
        if(stockopt_opt(OC_PASSWORD, 0) == 1u && stockopt_opt_text(OC_PWCHAR, t, sizeof t)) {
            int n = (int)strlen(t);
            while(n && t[n - 1] == ' ') { t[--n] = 0; }
            bool ok = n >= 1 && n <= PW_MAX;
            for(int i = 0; i < n; i++) { if(t[i] < '0' || t[i] > '9') { ok = false; } }
            if(ok && LOCK_OFF) {
                g_radio.tx_note[0] = "PASSWORD OFF"; g_radio.tx_note[1] = "HW build"; g_radio.tx_note_seq++;
                dbg_puts("power-on password ignored (HW build)\n");
            } else if(ok) {
                memcpy(s_pw, t, (size_t)n + 1u);
                s_lock = 1;
            }
        }
    }
    return s_lock == 1;
}
/* codeplug changed (boot, CPS write): a locked radio looks again, an open one stays open until the next power on */
void stockfx_lock_recheck(void) { if(s_lock == 1) { s_lock = 0; } }
int stockfx_lock_len(void) { return (int)strlen(s_in); }
void stockfx_lock_key(int digit, bool enter, bool clear)
{
    size_t n = strlen(s_in);
    if(clear) { if(n) { s_in[n - 1] = 0; } return; }
    if(digit >= 0 && n < sizeof s_in - 1) { s_in[n] = (char)('0' + digit); s_in[n + 1] = 0; }
    if(enter) { if(strcmp(s_in, s_pw) == 0) { s_lock = 2; } s_in[0] = 0; }
}

/* ------------------------------------------------------------------ TBST */
static float s_tb_ph, s_tb_w;
static bool s_tb_on, s_tb_own;
static uint32_t s_tb_end;
static float tbst_src(void)
{
    s_tb_ph += s_tb_w;
    if(s_tb_ph > 6.2831853f) { s_tb_ph -= 6.2831853f; }
    return 2500.0f * dsp_sinf(s_tb_ph);
}
bool stockfx_tbst(void)
{
    static const uint16_t k_hz[4] = { 1000, 1450, 1750, 2100 };
    const channel_t *c = radio_current();
    if(!g_radio.tx || c->mode != CFW_MODE_FM || s_tb_on || sig_busy()) { return false; }
    s_tb_w = 6.2831853f * (float)k_hz[stockopt_opt(OC_TBST, 0) & 3u] / (float)ADC_FS;
    s_tb_ph = 0;
    dspb_fm_set_source(tbst_src);
    s_tb_own = !voice_test_tone();
    if(s_tb_own && !dspb_fm_tx(true, 0, c->ctcss_dhz)) { dspb_fm_set_source(NULL); return false; }
    s_tb_on = true;
    s_tb_end = hal_millis() + 1000u;
    g_stockfx_dbg[FX_TBST]++;
    return true;
}
static void tbst_tick(uint32_t now)
{
    if(!s_tb_on || (g_radio.tx && (int32_t)(now - s_tb_end) < 0)) { return; }
    s_tb_on = false;
    dspb_fm_set_source(NULL);
    if(s_tb_own && g_radio.tx) { dspb_fm_tx(false, 0, 0); }
}

/* ------------------------------------------------------------------ NOAA weather receive */
uint32_t stockfx_noaa_hz(int i)                /* WX1..WX7 */
{
    static const uint32_t k[NOAA_N] = { 162550000u, 162400000u, 162475000u, 162425000u, 162450000u, 162500000u, 162525000u };
    return i >= 0 && i < NOAA_N ? k[i] : 0;
}
static channel_t s_nsave; static bool s_nvfo;
int stockfx_noaa_now(void) { return s_noaa; }
void stockfx_noaa(int i)
{
    if(g_radio.tx) { return; }
    if(i < 0) {
        if(s_noaa < 0) { return; }
        s_noaa = -1;
        g_radio.vfo_ch = s_nsave; g_radio.vfo = s_nvfo;
        radio_apply();
        return;
    }
    if(!stockfx_noaa_hz(i)) { return; }
    if(s_noaa < 0) { s_nsave = g_radio.vfo_ch; s_nvfo = g_radio.vfo; }
    s_noaa = i;
    s_whits = 0; s_walarm = false;
    channel_t *c = &g_radio.vfo_ch;
    c->mode = CFW_MODE_FM; c->wide = true; c->ctcss_dhz = 0; c->talkaround = false;
    c->rx_hz = stockfx_noaa_hz(i); c->tx_hz = 0;                      /* receive only */
    g_radio.vfo = true;
    radio_apply();
}

/* ------------------------------------------------------------------ PTT end: Analog APRS PTT Mode = End */
static bool s_beacon;
void stockfx_ptt_end(void)
{
#ifndef CFW_HW_BUILD                            /* HW builds: no transmission without its own key press */
    if(s_so.aprs_end && !aprs_busy() && !emerg_active()) { s_beacon = true; }   /* not after the beacon's own transmission */
#endif
}

/* ------------------------------------------------------------------ tick */
void stockfx_tick(void)
{
    uint32_t now = hal_millis();
    tbst_tick(now);
    if(s_walarm) {
        s_walarm = false;
        if(stockopt_opt(OC_WXALARM, 0)) {
            g_stockfx_dbg[FX_NOAA]++;
            g_radio.tx_note[0] = "NOAA ALERT"; g_radio.tx_note[1] = "1050 Hz tone"; g_radio.tx_note_seq++;
        }
    }
    if(s_beacon && !g_radio.tx) { s_beacon = false; if(!gps_beacon_now()) { g_stockfx_dbg[FX_BEACON]++; } }

    /* idle channel tones: end of a digital call / FM squelch closes */
    static bool s_voice, s_open;
    bool v = modem_active() && modem_call_info()->voice;
    if(s_voice && !v && !g_radio.tx) {
        static const uint16_t k[4] = { 0, 800, 1200, 1600 };
        uint32_t t = stockopt_opt(OC_IDLETONE, 0);
        if(t && t < 4u) { spk_beep(k[t], 120); g_stockfx_dbg[FX_IDLE]++; }
    }
    s_voice = v;
    bool open = g_radio.sq_open && radio_current()->mode == CFW_MODE_FM && !g_radio.monitor && rset_sql_level();
    if(s_open && !open && !g_radio.tx && !scan_active() && stockopt_opt(OC_ANAIDLE, 0)) {
        spk_beep(1000, 80); g_stockfx_dbg[FX_IDLE]++;
    }
    s_open = open;

    /* Auto Scan: the scan list of the channel starts 1 s after it was selected */
    static uint32_t s_sel_t;
    static int s_sel_ch = -1;
    if(s_chg) {
        s_chg = false;
        if(scan_active() || g_radio.vfo) { s_sel_ch = -1; }
        else if(g_radio.channel != s_sel_ch) { s_sel_ch = g_radio.channel; s_sel_t = now | 1u; }
    }
    if(s_sel_t && now - s_sel_t >= 1000u) {
        s_sel_t = 0;
        const channel_t *c = radio_current();
        if(s_so.auto_scan && !scan_active() && !g_radio.tx && !g_radio.vfo && c->scanlist < cfwcp_scanlist_count()) {
            scan_start(c->scanlist);
            g_stockfx_dbg[FX_SCAN]++;
        }
    }

    /* broadcast: AM/FM Function at power on, FM Monitor (a busy channel pauses the broadcast) */
    static bool s_boot;
    static uint8_t s_pband; static uint32_t s_phz, s_pt;
    if(!s_boot && now > 2500u) {
        s_boot = true;
        uint32_t fe = stockopt_opt(OC_FMEN, 0);
        so_bmem_t m;
        if(cfwcp_valid() && fe == 1u && stockopt_fm((int)stockopt_opt(OC_WORKFMCH, 0), &m)) { bcast_on(BC_FM, m.hz); }
        else if(cfwcp_valid() && (fe == 2u || fe == 3u) && bcast_has_am() && stockopt_am(0, &m)) { bcast_on(BC_AM, m.hz); }
        bool bt = stockopt_opt(OC_BTON, 0) == 1u;
        bt_config(bt, (uint8_t)stockopt_opt(OC_BTMIC, 0), (uint8_t)stockopt_opt(OC_BTSPK, 0));
    }
    if(stockopt_opt(OC_FMMON, 0)) {
        bool busy = g_radio.sq_open && g_radio.carrier;
        if(bcast_band() != BC_OFF && busy) {
            s_pband = (uint8_t)bcast_band(); s_phz = bcast_hz();
            bcast_off(); radio_apply();
            g_stockfx_dbg[FX_PAUSE]++;
        }
        if(busy || g_radio.tx) { s_pt = now; }
        else if(s_pband && now - s_pt > 1500u) { uint8_t b = s_pband; s_pband = 0; bcast_on(b, s_phz); }
    } else { s_pband = 0; }
    bt_tick();
}

__attribute__((weak)) void bt_config(bool on, uint8_t mic, uint8_t spk) { (void)on; (void)mic; (void)spk; }
__attribute__((weak)) void bt_tick(void) { }
__attribute__((weak)) const bt_status_t *bt_status(void) { static const bt_status_t z; return &z; }
