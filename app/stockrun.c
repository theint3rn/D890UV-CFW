/* Runtime of the stock CPS common sections (see stockrun.h, stockopt.h). */
#include <string.h>
#include "stockrun.h"
#include "stockopt.h"
#include "radio.h"
#include "rset.h"
#include "calllog.h"
#include "cfwcp.h"
#include "gps.h"
#include "aprs.h"
#include "spk.h"
#include "sig.h"
#include "hal.h"
#include "gfx.h"
#include "ui_int.h"
#include "bcast.h"

volatile uint32_t g_stockrun_dbg[6], g_stockrun_clock[2];   /* tests: clock = last GPS UTC, local time set */
enum { DBG_RING, DBG_TOTPRE, DBG_CLOCK, DBG_BEACON, DBG_ROAM, DBG_GPSROAM };

/* OptionalSetting roaming columns */
enum { OC_WANDERPER = 99, OC_EFFWAIT = 101, OC_ROAMSTART = 124, OC_TIMEROAM = 131, OC_CURROAMZ = 132, OC_GPS = 34 };

/* ------------------------------------------------------------------ rings */
static void ring_tick(void)
{
    static int s_unread = -1;
    static uint32_t s_seen_t, s_seen_src;
    static bool s_started;                       /* the log is empty at boot: the first call after it must ring */
    int u = sms_unread();
    if(s_unread >= 0 && u > s_unread && stockopt_sms_ring()) { spk_beep(1400, 300); g_stockrun_dbg[DBG_RING]++; }
    s_unread = u;
    const call_rec_t *r = calllog_at(0);
    if(r && (r->t != s_seen_t || r->src != s_seen_src)) {
        s_seen_t = r->t; s_seen_src = r->src;
        if(s_started && !(r->flags & CL_OUT) && (r->flags & CL_PRIVATE) && r->dst == g_radio.my_id && stockopt_call_ring()) {
            spk_beep(1000, 500);                 /* private call to this radio */
            g_stockrun_dbg[DBG_RING]++;
        }
    }
    s_started = true;
}

/* ------------------------------------------------------------------ TOT pre-alert (stock TOT Predict) */
static void tot_tick(void)
{
    static uint32_t s_since;
    static bool s_done;
    uint32_t tot = rset_tot_s();
    if(!g_radio.tx || !tot || tot <= 10u || !stockopt_tot_predict()) { s_done = false; return; }
    if(s_since != g_radio.tx_since) { s_since = g_radio.tx_since; s_done = false; }
    if(!s_done && hal_millis() - g_radio.tx_since >= (tot - 10u) * 1000u) {
        s_done = true;
        g_stockrun_dbg[DBG_TOTPRE]++;
        g_radio.tx_note[0] = "TOT 10 s"; g_radio.tx_note[1] = "TX ends soon"; g_radio.tx_note_seq++;
    }
}

/* ------------------------------------------------------------------ GPS clock (stock: GPS time + Time Zone) */
static void clock_tick(void)
{
    static uint32_t s_last;
    const gps_fix_t *f = gps_fix();
    if(!f->valid || !f->utc || !rset_gps_on()) { return; }
    uint32_t now = hal_millis();
    if(s_last && now - s_last < 60000u) { return; }
    s_last = now ? now : 1;
    uint32_t local = (uint32_t)((int32_t)f->utc + stockopt_tz_min() * 60);
    uint32_t r = rtc_now();
    if(!r || (r > local ? r - local : local - r) > 2u) { rtc_set(local); g_stockrun_dbg[DBG_CLOCK]++; g_stockrun_clock[0] = f->utc; g_stockrun_clock[1] = local; }
}

/* ------------------------------------------------------------------ APRS auto beacon (stock APRS Auto TX Interval) */
static void aprs_tick(void)
{
#ifndef CFW_HW_BUILD                        /* HW builds: no transmission without a key press (pre-flash rules) */
    static uint32_t s_last;
    const so_aprs_t *a = stockopt_aprs();
    if(!a->auto_s) { return; }
    uint32_t now = hal_millis();
    if(!s_last) { s_last = now; return; }
    if(now - s_last < (uint32_t)a->auto_s * 1000u) { return; }
    s_last = now;
    if(g_radio.tx || aprs_busy()) { return; }
    if(!gps_beacon_now()) { g_stockrun_dbg[DBG_BEACON]++; }
#endif
}

/* ------------------------------------------------------------------ roaming */
#define ROAM_MAX 32
static int s_rzone = -1, s_rcur = -1, s_rstep = -1, s_rn, s_rk, s_rwait;
static uint8_t s_rmem[ROAM_MAX];
static int16_t s_rdbm[ROAM_MAX];
static char s_rres[32];
static uint32_t s_rlast;

int roam_zone(void)
{
    if(s_rzone < 0) { s_rzone = (int)stockopt_opt(OC_CURROAMZ, 0); }
    if(s_rzone >= stockopt_roamzone_count()) { s_rzone = 0; }
    return s_rzone;
}
void roam_set_zone(int z) { s_rzone = z; s_rcur = -1; }
bool roam_busy(void) { return s_rstep >= 0; }
int roam_current(void) { return s_rcur; }
const char *roam_result(void) { return s_rres; }
int roam_last_rssi(int k) { return k >= 0 && k < s_rn ? s_rdbm[k] : -200; }
bool roam_auto_on(void) { return stockopt_opt(OC_TIMEROAM, 0) != 0; }

static bool roamable(void)
{
    const channel_t *c = radio_current();
    if(g_radio.vfo || c->mode != CFW_MODE_DMR || c->src != CH_SRC_CODEPLUG || g_radio.tx) { return false; }
    so_chan_t so;
    stockopt_channel(g_radio.channel, &so);
    return !so.no_roam;
}

bool roam_start(void)
{
    if(roam_busy() || stockopt_roamzone_count() == 0) { fmt_s(s_rres, "no roaming zone"); return false; }
    if(!roamable()) { fmt_s(s_rres, "DMR memory channel only"); return false; }
    s_rn = stockopt_roamzone_members(roam_zone(), s_rmem, ROAM_MAX);
    if(!s_rn) { fmt_s(s_rres, "zone has no channels"); return false; }
    for(int k = 0; k < s_rn; k++) { s_rdbm[k] = -200; }
    s_rk = 0; s_rstep = 0; s_rwait = 0;
    fmt_s(s_rres, "searching...");
    return true;
}

/* one roaming channel per 60 ms: tune RF chip 0 to its RX frequency, read RSSI + carrier, then back */
static void roam_tick(void)
{
    if(s_rstep < 0) {
        /* auto roaming at a fixed time (stock Auto Roaming, start condition Fixed Time, period in minutes) */
        if(roam_auto_on() && stockopt_opt(OC_ROAMSTART, 0) == 0u && roamable()) {
            uint32_t per = stockopt_opt(OC_WANDERPER, 0) + 1u, now = hal_millis();
            if(!s_rlast) { s_rlast = now; }
            if(now - s_rlast >= per * 60000u) { s_rlast = now; roam_start(); }
        }
        return;
    }
    if(g_radio.tx) { s_rstep = -1; fmt_s(s_rres, "stopped (TX)"); radio_apply(); return; }
    so_roamch_t rc;
    if(s_rwait == 0) {
        if(!stockopt_roamch(s_rmem[s_rk], &rc)) { s_rwait = 6; }
        else { rf_set_freq(0, rc.rx_hz); }
    }
    if(++s_rwait < 6) { return; }
    s_rwait = 0;
    if(stockopt_roamch(s_rmem[s_rk], &rc) && rf_carrier(0)) { s_rdbm[s_rk] = (int16_t)rf_rssi_dbm(rf_rssi(0)); }
    if(++s_rk < s_rn) { return; }
    s_rstep = -1;
    int best = -1;
    for(int k = 0; k < s_rn; k++) { if(s_rdbm[k] > -200 && (best < 0 || s_rdbm[k] > s_rdbm[best])) { best = k; } }
    if(best < 0) { fmt_s(s_rres, "Repeater not found"); radio_apply(); return; }
    stockopt_roamch(s_rmem[best], &rc);
    channel_t *c = radio_edit();                   /* roam: this channel now uses the repeater (not saved) */
    c->rx_hz = rc.rx_hz; c->tx_hz = rc.tx_hz; c->color_code = rc.cc; c->slot = rc.slot;
    s_rcur = s_rmem[best];
    char *p = fmt_s(s_rres, rc.name); *p++ = ' ';
    int d = s_rdbm[best];
    if(d < 0) { *p++ = '-'; d = -d; }
    fmt_s(fmt_u(p, (uint32_t)d, 1), "dBm");
    g_stockrun_dbg[DBG_ROAM]++;
    radio_apply();
}

/* GPS roaming: inside an entry's circle -> its zone (checked every 10 s with a GPS fix) */
static int s_gpsr = -1;
int gpsroam_active(void) { return s_gpsr; }

static uint32_t dist_m(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2)
{
    /* equirectangular: 1e-7 deg = 0.0111 m; longitude scaled by cos(latitude) (polynomial, |lat| < 80) */
    float lat = (float)la1 * 1e-7f * 0.01745329f, c = 1.0f - lat * lat / 2.0f + lat * lat * lat * lat / 24.0f;
    float dy = (float)(la2 - la1) * 0.011132f, dx = (float)(lo2 - lo1) * 0.011132f * c;
    float d2 = dx * dx + dy * dy, r = d2 > 1.0f ? d2 / 2.0f : 1.0f;
    for(int i = 0; i < 20; i++) { r = 0.5f * (r + d2 / r); }
    return (uint32_t)r;
}

static void gpsroam_tick(void)
{
    static uint32_t s_last;
    const gps_fix_t *f = gps_fix();
    uint32_t now = hal_millis();
    if(!f->valid || (s_last && now - s_last < 10000u)) { return; }
    s_last = now ? now : 1;
    for(int i = 0; i < stockopt_gpsroam_count(); i++) {
        so_gpsroam_t g;
        if(!stockopt_gpsroam(i, &g) || g.zone >= zone_count()) { continue; }
        if(dist_m(f->lat_e7, f->lon_e7, g.lat_e7, g.lon_e7) <= g.radius_m) {
            if(s_gpsr != i || g_radio.zone != g.zone) {
                s_gpsr = i;
                if(g_radio.zone != g.zone && !g_radio.tx) { radio_set_zone(g.zone); g_stockrun_dbg[DBG_GPSROAM]++; }
            }
            return;
        }
    }
    s_gpsr = -1;
}

/* ------------------------------------------------------------------ hot keys */
static const struct { const char *name; int8_t um; } k_hk_menus[] = {
    { "SMS", UM_SMS }, { "New SMS", UM_SMS }, { "Hot Text", UM_SMS }, { "Received SMS", UM_SMS }, { "Send SMS", UM_SMS },
    { "Contact List", UM_CONTACTS }, { "Manual Dial", UM_CONTACTS }, { "Call Log", UM_CALLLOG },
    { "Dialed Call", UM_CALLLOG }, { "Received Call", UM_CALLLOG }, { "Missed Call", UM_CALLLOG },
    { "Zone", UM_ZONES }, { "Radio Set", UM_SETTINGS }, { "Scan", UM_SCAN }, { "GPS", UM_GPS },
};

static bool name_eq(const char *a, const char *b)
{
    for(; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if(x != y) { return false; }
    }
    return *a == *b;
}

int stockrun_hotkey(int digit, const char **l1, const char **l2)
{
    static char t1[24], t2[24];
    *l1 = t1; *l2 = t2; t1[0] = t2[0] = 0;
    int n = digit == 0 ? 10 : digit;
    so_hotkey_t h;
    if(!stockopt_hotkey(n, &h) || h.mode == SO_HK_OFF) { return -2; }
    if(h.mode == SO_HK_MENU) {
        for(unsigned i = 0; i < sizeof k_hk_menus / sizeof k_hk_menus[0]; i++) {
            if(name_eq(h.menu, k_hk_menus[i].name)) { return k_hk_menus[i].um; }
        }
        fmt_s(fmt_s(t1, "HOT KEY "), h.menu); fmt_s(t2, "menu not in cfw");
        return -1;
    }
    if(!h.digital) {                                 /* analog: Hot Key Quick Call entry ("1".."4" or "Quick Call N") */
        const char *p = h.object;
        while(*p && (*p < '0' || *p > '9')) { p++; }
        so_qcall_t q;
        if(*p && stockopt_quickcall(*p - '0', &q) && q.entry) {
            const char *e = sig_send((sig_kind_t)q.kind, q.entry - 1);
            fmt_s(t1, "QUICK CALL"); fmt_s(t2, e ? e : "sent");
        } else {
            fmt_s(t1, "QUICK CALL"); fmt_s(t2, "not set");
        }
        return -1;
    }
    /* digital: the contact becomes the TX target (PTT calls it); Content = a Pre-defined SMS sent to it */
    for(int i = 0; i < cfwcp_contact_count(); i++) {
        const cp_contact_t *ct = cfwcp_contact(i);
        char nm[17];
        int k = 0;
        for(; k < 16 && ct->name[k]; k++) { nm[k] = ct->name[k]; }
        nm[k] = 0;
        while(k && nm[k - 1] == ' ') { nm[--k] = 0; }
        if(!name_eq(nm, h.object)) { continue; }
        bool priv = h.priv || ct->kind == 1;
        radio_set_target(ct->id, priv);
        fmt_s(t1, priv ? "PRIVATE " : "GROUP "); fmt_s(t1 + strlen(t1), nm);
        fmt_s(t2, "PTT to call");
        if(h.content[0]) {
            char txt[66];
            for(int r = 0; r < 32; r++) {                /* Content names a Pre-defined SMS by its text or number */
                txt[0] = 0;
                if(!cfwcp_stock_value_row(SO_T_PREFAB, 1, r, txt, sizeof txt) || !txt[0]) { break; }
                char no[8]; no[0] = 0;
                cfwcp_stock_value_row(SO_T_PREFAB, 0, r, no, sizeof no);
                if(name_eq(txt, h.content) || name_eq(no, h.content)) {
                    fmt_s(t2, radio_send_sms(ct->id, !priv, txt) ? "SMS sent" : "SMS failed");
                    break;
                }
            }
        }
        return -1;
    }
    fmt_s(t1, "HOT KEY CALL"); fmt_s(t2, "contact not found");
    return -1;
}

void stockrun_tick(void)
{
    static bool s_booted;
    if(!s_booted && hal_millis() > 2000u) {          /* stock Startup Sound, after the boot screen */
        s_booted = true;
        if(stockopt_boot_sound()) { spk_beep(1200, 250); g_stockrun_dbg[DBG_RING]++; }
    }
    if(g_radio.tx && bcast_band() != BC_OFF) { bcast_off(); }   /* broadcast receive stops on PTT (as stock) */
    ring_tick();
    tot_tick();
    clock_tick();
    aprs_tick();
    roam_tick();
    gpsroam_tick();
}
