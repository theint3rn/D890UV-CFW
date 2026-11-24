/* Menu > Radio extras: the stock CPS common sections in the radio (app/stockopt.c, app/stockrun.c):
 *   FM radio (stock FM memories), AM air (D890UV: AM Air memories + AM Zones), Roaming (Roaming Zone / Channel,
 *   GPS roaming state), Hot keys (what each long-pressed digit does), Options (stock Optional Settings the cfw acts
 *   on, editable: radio edits journal), Device info (stock Device Information + Local Information page). */
#include <string.h>
#include "ui.h"
#include "ui_int.h"
#include "radio.h"
#include "rset.h"
#include "cfwcp.h"
#include "stockopt.h"
#include "stockrun.h"
#include "bcast.h"
#include "stockfx.h"
#include "stockdlg.h"
#include "stockcp.h"
#include "emerg.h"
#include "gps.h"
#include "gfx.h"
#include "hal.h"

#ifndef VERSION
#define VERSION "?"
#endif
#ifndef CFW_BOARD_NAME
#define CFW_BOARD_NAME "D890UV"
#endif
#define TOP_Y 12

static void copy_name(char *d, const char *s, int n)
{
    int i = 0;
    for(; i < n && s[i]; i++) { d[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?'; }
    d[i] = 0;
}

static char *fmt_mhz(char *p, uint32_t hz, int dec)
{
    p = fmt_u(p, hz / 1000000u, 1); *p++ = '.';
    uint32_t f = hz % 1000000u;
    for(int k = 0, d = 100000; k < dec; k++, d /= 10) { *p++ = (char)('0' + f / (uint32_t)d % 10u); }
    *p = 0;
    return p;
}

/* ------------------------------------------------------------------ broadcast player (FM / AM) */
static int s_bband, s_bmem = -1;             /* playing: band, memory index (-1 = VFO) */
static uint32_t s_bhz;
static uint8_t s_bvol = 13;

static uint32_t b_step(void) { return s_bband == BC_FM ? 100000u : 25000u; }
static int b_count(void) { return s_bband == BC_FM ? stockopt_fm_count() : stockopt_am_count(); }
static bool b_mem(int i, so_bmem_t *m) { return s_bband == BC_FM ? stockopt_fm(i, m) : stockopt_am(i, m); }

static void b_tune(void)
{
    if(!bcast_on(s_bband, s_bhz)) { ui_popup("NO RECEIVER", bcast_ack() ? "out of band" : "I2C no answer", C_ORANGE); }
}

static void bp_draw(void)
{
    char t[32], *p;
    so_bmem_t m;
    gfx_clear(C_BLACK);
    ui_header(s_bband == BC_FM ? "FM RADIO" : "AM AIR");
    p = fmt_mhz(t, s_bhz, s_bband == BC_FM ? 1 : 3);
    gfx_text_center(TOP_Y + 14, t, C_YELLOW, C_BLACK, 2);
    if(s_bmem >= 0 && b_mem(s_bmem, &m)) { copy_name(t, m.name, 16); }
    else { fmt_s(t, "VFO"); }
    gfx_text_center(TOP_Y + 36, t, C_CYAN, C_BLACK, 1);
    p = fmt_s(t, "Vol "); fmt_u(p, s_bvol, 1);
    gfx_text_center(TOP_Y + 48, t, C_GREY, C_BLACK, 1);
    gfx_text_center(TOP_Y + 58, bcast_ack() ? (bcast_band() ? "receiving" : "off") : "receiver: no I2C answer",
                    bcast_ack() ? C_GREEN : C_ORANGE, C_BLACK, 1);
    gfx_text_center(LCD_H - 34, "knob tune, Up/Dn memory", C_GREY, C_BLACK, 1);
    gfx_text_center(LCD_H - 24, "*/# volume", C_GREY, C_BLACK, 1);
    ui_softkeys("MEM", "STOP");
}

static bool bp_key(int i, int k)
{
    (void)i;
    int n = b_count();
    so_bmem_t m;
    switch(k) {
    case KEY_KNOB_CW: case KEY_KNOB_CCW: {
        uint32_t f = k == KEY_KNOB_CW ? s_bhz + b_step() : s_bhz - b_step();
        uint32_t lo = s_bband == BC_FM ? BC_FM_MIN : BC_AM_MIN, hi = s_bband == BC_FM ? BC_FM_MAX : BC_AM_MAX;
        if(f >= lo && f <= hi) { s_bhz = f; s_bmem = -1; b_tune(); }
        return true; }
    case KEY_UP: case KEY_DOWN:
        if(n <= 0) { return true; }
        s_bmem = s_bmem < 0 ? 0 : (s_bmem + (k == KEY_DOWN ? 1 : n - 1)) % n;
        if(b_mem(s_bmem, &m)) { s_bhz = m.hz; b_tune(); }
        return true;
    case KEY_STAR: if(s_bvol) { s_bvol--; bcast_volume(s_bvol); } return true;
    case KEY_HASH: if(s_bvol < 15) { s_bvol++; bcast_volume(s_bvol); } return true;
    case KEY_MENU: um_pop(); return true;                     /* back to the memory list, keeps playing */
    case KEY_EXIT: bcast_off(); radio_apply(); um_pop(); return true;
    default: return false;
    }
}
static const um_scr_t k_bplay = { NULL, NULL, NULL, NULL, bp_key, bp_draw, NULL };

static void b_play(int band, int mem, uint32_t hz)
{
    s_bband = band; s_bmem = mem; s_bhz = hz;
    bcast_volume(s_bvol);
    b_tune();
    um_push(&k_bplay, 0);
}

/* memory list: row 0 = VFO, then the memories (stock FM / AM Air table) */
static int bl_count(void) { return b_count() + 1; }
static void bl_item(int i, char *l, char *v)
{
    so_bmem_t m;
    uint32_t hz = 0;
    if(i == 0) {
        fmt_s(l, "VFO");
        if(!(s_bband == BC_FM ? stockopt_fm_vfo(&hz) : stockopt_am_vfo(&hz))) { hz = s_bband == BC_FM ? 88000000u : 118000000u; }
    } else if(b_mem(i - 1, &m)) {
        copy_name(l, m.name, 12); hz = m.hz;
    }
    if(hz) { fmt_mhz(v, hz, s_bband == BC_FM ? 1 : 3); }
}
static void bl_select(int i)
{
    so_bmem_t m;
    uint32_t hz = 0;
    if(i == 0) {
        if(bcast_band() == s_bband && bcast_hz()) { hz = bcast_hz(); }
        else if(!(s_bband == BC_FM ? stockopt_fm_vfo(&hz) : stockopt_am_vfo(&hz))) { hz = s_bband == BC_FM ? 88000000u : 118000000u; }
        b_play(s_bband, -1, hz);
    } else if(b_mem(i - 1, &m)) {
        b_play(s_bband, i - 1, m.hz);
    }
}
static const char *bl_help(int i) { (void)i; return bcast_band() ? "playing - Exit in player stops" : "stock CPS memories"; }
static um_scr_t k_blist = { NULL, bl_count, bl_item, bl_select, NULL, NULL, bl_help };

/* AM zones (stock AM Zone): a zone narrows the AM memory list */
static int s_amz = -1;
static uint16_t s_amzm[64];
static int s_amzn;
static int amz_count(void) { return stockopt_amzone_count() + 1; }
static void amz_item(int i, char *l, char *v)
{
    if(i == 0) { fmt_s(l, "All AM memories"); fmt_u(v, (uint32_t)stockopt_am_count(), 1); return; }
    char nm[17];
    stockopt_amzone(i - 1, nm, sizeof nm);
    fmt_s(l, nm);
    uint16_t tmp[64];
    fmt_u(v, (uint32_t)stockopt_amzone_members(i - 1, tmp, 64), 1);
}
static int amzl_count(void) { return s_amzn; }
static void amzl_item(int i, char *l, char *v)
{
    so_bmem_t m;
    if(i < s_amzn && stockopt_am(s_amzm[i], &m)) { copy_name(l, m.name, 12); fmt_mhz(v, m.hz, 3); }
}
static void amzl_select(int i) { so_bmem_t m; if(i < s_amzn && stockopt_am(s_amzm[i], &m)) { b_play(BC_AM, s_amzm[i], m.hz); } }
static um_scr_t k_amzl = { "AM ZONE", amzl_count, amzl_item, amzl_select, NULL, NULL, NULL };
static void amz_select(int i)
{
    s_bband = BC_AM;
    if(i == 0) { k_blist.title = "AM AIR"; um_push(&k_blist, 0); return; }
    s_amz = i - 1;
    s_amzn = stockopt_amzone_members(s_amz, s_amzm, 64);
    um_push(&k_amzl, 0);
}
static const um_scr_t k_amz = { "AM AIR", amz_count, amz_item, amz_select, NULL, NULL, NULL };

/* ------------------------------------------------------------------ roaming */
enum { RO_ZONE, RO_NOW, RO_AUTO, RO_GPS, RO_N };
static int ro_count(void) { return RO_N + (stockopt_roamzone_count() ? stockopt_roamzone_members(roam_zone(), (uint8_t[32]){0}, 32) : 0); }
static void ro_item(int i, char *l, char *v)
{
    char nm[17];
    switch(i) {
    case RO_ZONE: fmt_s(l, "Roam zone"); if(stockopt_roamzone(roam_zone(), nm, sizeof nm)) { copy_name(v, nm, 10); } else { fmt_s(v, "none"); } return;
    case RO_NOW: fmt_s(l, "Roam now"); fmt_s(v, roam_busy() ? "..." : ""); return;
    case RO_AUTO: fmt_s(l, "Auto roam"); fmt_s(v, roam_auto_on() ? "On" : "Off"); return;
    case RO_GPS: fmt_s(l, "GPS roam zone"); { int g = gpsroam_active(); if(g >= 0) { so_gpsroam_t e; stockopt_gpsroam(g, &e); fmt_u(fmt_s(v, "#"), (uint32_t)e.zone + 1u, 1); } else { fmt_s(v, "-"); } } return;
    default: break;
    }
    uint8_t mem[32];
    int n = stockopt_roamzone_members(roam_zone(), mem, 32), k = i - RO_N;
    so_roamch_t rc;
    if(k < n && stockopt_roamch(mem[k], &rc)) {
        fmt_s(l, rc.name);
        int d = roam_last_rssi(k);
        if(roam_current() == mem[k]) { fmt_s(v, "IN USE"); }
        else if(d > -200) { char *p = v; if(d < 0) { *p++ = '-'; d = -d; } fmt_s(fmt_u(p, (uint32_t)d, 1), "dBm"); }
        else { fmt_mhz(v, rc.rx_hz, 3); }
    }
}
static const char *s_rz_names[16];
static char s_rz_buf[16][17];
static void set_rz(int v) { roam_set_zone(v); }
static void ro_select(int i)
{
    if(i == RO_ZONE) {
        int n = stockopt_roamzone_count();
        if(!n) { ui_popup("NO ROAM ZONE", "CPS: Roaming Zone", C_ORANGE); return; }
        if(n > 16) { n = 16; }
        for(int k = 0; k < n; k++) { stockopt_roamzone(k, s_rz_buf[k], 17); s_rz_names[k] = s_rz_buf[k]; }
        um_choose("ROAM ZONE", s_rz_names, n, roam_zone(), set_rz);
    } else if(i == RO_NOW) {
        if(!roam_start()) { ui_popup("NO ROAMING", roam_result(), C_ORANGE); }
    }
}
static const char *ro_help(int i) { (void)i; return roam_result()[0] ? roam_result() : "DMR repeaters by signal"; }
static const um_scr_t k_roam = { "ROAMING", ro_count, ro_item, ro_select, NULL, NULL, ro_help };

/* ------------------------------------------------------------------ hot keys */
static int hk_count(void) { return 10; }
static void hk_item(int i, char *l, char *v)
{
    so_hotkey_t h;
    char *p = fmt_s(l, "Long "); fmt_u(p, (uint32_t)(i + 1) % 10u, 1);
    if(!stockopt_hotkey(i + 1, &h) || h.mode == SO_HK_OFF) { fmt_s(v, "off"); return; }
    if(h.mode == SO_HK_MENU) { copy_name(v, h.menu, 12); return; }
    copy_name(v, h.object[0] ? h.object : (h.digital ? "call" : "quick call"), 12);
}
static void hk_select(int i)
{
    const char *l1, *l2;
    int um = stockrun_hotkey((i + 1) % 10, &l1, &l2);
    if(um >= 0) { um_open(um); return; }
    if(um == -1 && l1[0]) { ui_popup(l1, l2, C_CYAN); }
}
static const char *hk_help(int i) { (void)i; return "long press digit = hot key"; }
static const um_scr_t k_hk = { "HOT KEYS", hk_count, hk_item, hk_select, NULL, NULL, hk_help };

/* ------------------------------------------------------------------ options (stock Optional Settings) */
static const char *const k_o_step[] = { "2.5K", "5K", "6.25K", "8.33K", "10K", "12.5K", "20K", "25K", "30K", "50K" };   /* V1.05 0x0814A520 */
static const char *const k_o_tbst[] = { "1000Hz", "1450Hz", "1750Hz", "2100Hz" };
static const char *const k_o_vscan[] = { "Time", "Carrier", "Stop" };
static const char *const k_o_idle[] = { "Off", "Type 1", "Type 2", "Type 3" };
static const char *const k_o_fm[] = { "Off", "FM", "AM A", "AM B" };
static const char *const k_o_col[] = { "Orange", "Red", "Yellow", "Green", "Cyan", "Blue", "White" };
static const char *const k_o_bt[] = { "Off", "On" };
static const char *const k_o_lvl[] = { "Level 1", "Level 2", "Level 3", "Level 4", "Level 5" };
static const char *const k_o_onoff[] = { "Off", "On" };
static const char *const k_o_ring[] = { "Off", "Ring" };
static const char *const k_o_start[] = { "Picture", "Character", "Customer's" };
static const char *const k_o_exit[] = { "5S", "10S", "15S", "20S", "25S", "30S", "35S", "40S", "45S", "50S", "55S", "60S" };
static const char *const k_o_rpt[] = { "Off", "Positive", "Negative" };
#if !defined(CFW_BOARD_D168UV)
static const char *const k_o_amoff[] = { "Positive", "Negative" };
#endif
static const char *k_o_tz[34];
static char s_tzbuf[34][10];
typedef struct { const char *label; uint8_t col, n; const char *const *opt; const char *help; uint32_t dflt; } opt_t;
static const opt_t k_opts[] = {
    { "Freq step", 8, 10, k_o_step, "VFO step (Step)", 0xFF },
    { "TBST tone", 40, 4, k_o_tbst, "FM: Up key while PTT", 0 },
    { "VFO scan", 14, 3, k_o_vscan, "resume type", 0 },
    { "Digi idle tone", 51, 4, k_o_idle, "beep after a call", 0 },
    { "Ana idle tone", 168, 2, k_o_onoff, "beep: squelch closes", 0 },
    { "AM/FM at start", 28, 4, k_o_fm, "broadcast at power on", 0 },
    { "FM monitor", 37, 2, k_o_onoff, "busy channel: pause", 0 },
    { "Channel color", 128, 7, k_o_col, "channel name", 0 },
    { "NOAA alert", 141, 2, k_o_onoff, "1050 Hz popup", 0 },
    { "Bluetooth", 90, 2, k_o_bt, "module on (restart)", 0 },
    { "BT mic gain", 95, 5, k_o_lvl, "Bluetooth (restart)", 0 },
    { "BT spk gain", 96, 5, k_o_lvl, "Bluetooth (restart)", 0 },
    { "Time zone", 42, 0, k_o_tz, "GPS clock (TmZone)", 0 },
    { "TOT predict", 161, 2, k_o_onoff, "warn 10 s before TOT", 0 },
    { "SMS ring", 35, 2, k_o_ring, "beep on new SMS", 0 },
    { "Call ring", 41, 2, k_o_ring, "beep on private call", 0 },
    { "Startup sound", 54, 2, k_o_onoff, "beep after power on", 0 },
    { "Power-on disp", 6, 3, k_o_start, "Character: CPS text", 0 },
    { "Menu exit", 120, 12, k_o_exit, "close menus after", 0 },
    { "Auto repeater", 65, 3, k_o_rpt, "VFO TX offset", 0 },
    { "Auto roaming", 131, 2, k_o_onoff, "roam every N min", 0 },
#if !defined(CFW_BOARD_D168UV)
    { "AM offset", 186, 2, k_o_amoff, "AM down-converter", 0 },
#endif
};
#define N_OPTS ((int)(sizeof k_opts / sizeof k_opts[0]))
static int s_opt;

static void tz_names(void)
{
    if(k_o_tz[0]) { return; }
    for(int i = 0; i < stockopt_tz_count(); i++) {
        int m = stockopt_tz_at(i);
        char *p = fmt_s(s_tzbuf[i], "UTC");
        if(m) {
            int a = m < 0 ? -m : m;
            *p++ = m < 0 ? '-' : '+';
            p = fmt_u(p, (uint32_t)a / 60u, 2); *p++ = ':'; fmt_u(p, (uint32_t)a % 60u, 2);
        }
        k_o_tz[i] = s_tzbuf[i];
    }
}

static int op_n(const opt_t *o) { return o->col == 42 ? stockopt_tz_count() : o->n; }
static int op_count(void) { return N_OPTS; }
static void op_item(int i, char *l, char *v)
{
    tz_names();
    const opt_t *o = &k_opts[i];
    fmt_s(l, o->label);
    uint32_t x = stockopt_opt(o->col, o->dflt);
    fmt_s(v, x < (uint32_t)op_n(o) ? o->opt[x] : "-");
}
static void set_op(int v) { rset_set_opt(k_opts[s_opt].col, (uint32_t)v); }
static void op_select(int i)
{
    tz_names();
    s_opt = i;
    uint32_t x = stockopt_opt(k_opts[i].col, 0);
    int n = op_n(&k_opts[i]);
    um_choose(k_opts[i].label, k_opts[i].opt, n, x < (uint32_t)n ? (int)x : 0, set_op);
}
static const char *op_help(int i) { return k_opts[i].help; }
static bool op_key(int i, int k)
{
    (void)i;
    if(k == KEY_EXIT && rset_dirty()) {                      /* leaving: edits -> codeplug journal */
        bool ok = rset_save();
        um_pop();
        ui_popup(ok ? "SAVED" : "NOT SAVED", ok ? "to the codeplug" : "kept in the radio", ok ? C_GREEN : C_ORANGE);
        return true;
    }
    return false;
}
static const um_scr_t k_opt = { "OPTIONS (STOCK)", op_count, op_item, op_select, op_key, NULL, op_help };

/* ------------------------------------------------------------------ device info (stock Device Information) */
static void di_draw(void)
{
    char t[32], *p;
    gfx_clear(C_BLACK);
    ui_header("DEVICE INFO");
    int y = TOP_Y + 3;
    const cp_settings_t *st = cfwcp_settings();
    p = fmt_s(t, "Model "); fmt_s(p, CFW_BOARD_NAME);
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 10;
    gfx_text(4, y, "Firmware cfw " VERSION, C_WHITE, C_BLACK, 1); y += 10;
#if defined(CFW_BOARD_D168UV)
    gfx_text(4, y, "136-174 400-480 MHz", C_WHITE, C_BLACK, 1); y += 10;
    gfx_text(4, y, "FM 76-108 MHz", C_GREY, C_BLACK, 1); y += 10;
#else
    gfx_text(4, y, "136-174 400-480 MHz", C_WHITE, C_BLACK, 1); y += 10;
    gfx_text(4, y, "FM 64-108 AM 108-137", C_GREY, C_BLACK, 1); y += 10;
#endif
    p = fmt_s(t, "Radio ID "); fmt_u(p, g_radio.my_id, 1);
    gfx_text(4, y, t, C_CYAN, C_BLACK, 1); y += 10;
    p = fmt_s(t, "Call "); fmt_s(p, g_radio.my_call);
    gfx_text(4, y, t, C_CYAN, C_BLACK, 1); y += 10;
    if(st && st->radio_name[0] && st->radio_name[0] != ' ') {
        p = fmt_s(t, "Name "); copy_name(p, st->radio_name, 16);
        gfx_text(4, y, t, C_CYAN, C_BLACK, 1); y += 10;
    }
    p = fmt_s(t, "Codeplug gen "); fmt_u(p, cfwcp_generation(), 1);
    gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 10;
    const gps_fix_t *g = gps_fix();
    if(g->valid) { p = fmt_s(t, "Locator "); gps_fmt_loc(p, g->lat_e7, g->lon_e7); gfx_text(4, y, t, C_GREY, C_BLACK, 1); }
    ui_softkeys(NULL, "BACK");
}
static const um_scr_t k_devinfo = { NULL, NULL, NULL, NULL, NULL, di_draw, NULL };

/* ------------------------------------------------------------------ NOAA weather (receive only) */
static int no_count(void) { return NOAA_N + 1; }
static void no_item(int i, char *l, char *v)
{
    if(i == NOAA_N) { fmt_s(l, "Off"); if(stockfx_noaa_now() < 0) { fmt_s(v, "<"); } return; }
    fmt_u(fmt_s(l, "WX"), (uint32_t)i + 1u, 1);
    char *p = fmt_mhz(v, stockfx_noaa_hz(i), 3);
    if(stockfx_noaa_now() == i) { fmt_s(p, " <"); }
}
static void no_select(int i) { stockfx_noaa(i == NOAA_N ? -1 : i); }
static const char *no_help(int i) { (void)i; return "receive only, 1050 Hz alert"; }
static const um_scr_t k_noaa = { "NOAA WEATHER", no_count, no_item, no_select, NULL, NULL, no_help };

/* ------------------------------------------------------------------ Bluetooth module */
static void bt_draw(void)
{
    const bt_status_t *b = bt_status();
    char t[40];
    gfx_clear(C_BLACK);
    ui_header("BLUETOOTH");
    int y = TOP_Y + 4;
    gfx_text(4, y, b->on ? "On (codeplug)" : "Off (codeplug)", b->on ? C_GREEN : C_GREY, C_BLACK, 1); y += 11;
    if(b->on) {
        gfx_text(4, y, b->present ? "module found" : "module: no answer", b->present ? C_GREEN : C_ORANGE, C_BLACK, 1); y += 11;
        fmt_s(fmt_s(t, "Version "), b->ver); gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 11;
        fmt_s(fmt_s(t, "Name "), b->name); gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 11;
        gfx_text(4, y, b->connected ? "connected" : "not connected", C_WHITE, C_BLACK, 1); y += 11;
        fmt_u(fmt_s(fmt_u(fmt_s(t, "Mic gain "), b->mic + 1u, 1), "  Spk gain "), b->spk + 1u, 1);
        gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 11;
        gfx_text(4, y, "no BT audio / PTT in cfw", C_GREY, C_BLACK, 1);
    }
    ui_softkeys(NULL, "BACK");
}
static const um_scr_t k_bt = { NULL, NULL, NULL, NULL, NULL, bt_draw, NULL };

/* ------------------------------------------------------------------ friends list (DMR IDs of the stock Friends List) */
static bool fr_id(int i, uint32_t *id)
{
    char b[16];
    if(!stockdlg_row_text(SD_T_FRIENDS, i, 1, b, sizeof b)) { return false; }
    *id = 0;
    for(const char *p = b; *p >= '0' && *p <= '9'; p++) { *id = *id * 10u + (uint32_t)(*p - '0'); }
    return *id != 0;
}
static int fr_count(void) { int n = stockdlg_rows(SD_T_FRIENDS); return n ? n : 1; }
static void fr_item(int i, char *l, char *v)
{
    uint32_t id;
    char nm[28] = "";
    if(!fr_id(i, &id)) { fmt_s(l, stockdlg_rows(SD_T_FRIENDS) ? "-" : "no friends in codeplug"); return; }
    if(stockcp_id_name(CFW_MODE_DMR, id, false, nm, sizeof nm)) { copy_name(l, nm, 14); fmt_u(v, id, 1); }
    else { fmt_u(l, id, 1); }
}
static void fr_select(int i)                          /* the friend becomes the private call target of the DMR channel */
{
    uint32_t id;
    if(!fr_id(i, &id)) { return; }
    if(radio_current()->mode != CFW_MODE_DMR) { ui_popup("DMR ONLY", "pick a DMR channel", C_ORANGE); return; }
    radio_set_target(id, true);
    um_close();
    char t[20];
    fmt_u(fmt_s(t, "private "), id, 1);
    ui_popup("CALL FRIEND", t, C_CYAN);
}
static const char *fr_help(int i) { (void)i; return "select: private call target"; }
static const um_scr_t k_friends = { "FRIENDS", fr_count, fr_item, fr_select, NULL, NULL, fr_help };

/* ------------------------------------------------------------------ root */
enum { X_FM, X_AM, X_ROAM, X_HOTKEYS, X_OPTIONS, X_DEVINFO, X_NOAA, X_BT, X_FRIENDS, X_EMERG, X_N };
static const char *const k_x[X_N] = { "FM radio", "AM air", "Roaming", "Hot keys", "Options (stock)", "Device info", "NOAA weather", "Bluetooth", "Friends", "Emergency" };
static int x_map(int i) { return !bcast_has_am() && i >= X_AM ? i + 1 : i; }
static int x_count(void) { return bcast_has_am() ? X_N : X_N - 1; }
static void x_item(int i, char *l, char *v)
{
    int id = x_map(i);
    fmt_s(l, k_x[id]);
    if(id == X_FM) { fmt_u(v, (uint32_t)stockopt_fm_count(), 1); }
    if(id == X_AM) { fmt_u(v, (uint32_t)stockopt_am_count(), 1); }
    if((id == X_FM && bcast_band() == BC_FM) || (id == X_AM && bcast_band() == BC_AM)) { fmt_s(v, "ON"); }
    if(id == X_ROAM) { fmt_u(v, (uint32_t)stockopt_roamch_count(), 1); }
}
static void x_select(int i)
{
    switch(x_map(i)) {
    case X_FM: s_bband = BC_FM; k_blist.title = "FM RADIO"; um_push(&k_blist, 0); break;
    case X_AM: s_bband = BC_AM; um_push(&k_amz, 0); break;
    case X_ROAM: um_push(&k_roam, 0); break;
    case X_HOTKEYS: um_push(&k_hk, 0); break;
    case X_OPTIONS: um_push(&k_opt, 0); break;
    case X_DEVINFO: um_push(&k_devinfo, 0); break;
    case X_NOAA: um_push(&k_noaa, 0); break;
    case X_BT: um_push(&k_bt, 0); break;
    case X_FRIENDS: um_push(&k_friends, 0); break;
    case X_EMERG: um_close(); emerg_start("MENU", true); break;
    }
}
static const char *x_help(int i)
{
    switch(x_map(i)) {
    case X_FM: return "broadcast FM (stock FM list)";
    case X_AM: return "air band AM (AM Air / Zone)";
    case X_ROAM: return "DMR repeater roaming";
    default: return "stock CPS common settings";
    }
}
static const um_scr_t k_x_root = { "RADIO EXTRAS", x_count, x_item, x_select, NULL, NULL, x_help };
const um_scr_t *ui_stock_root(void) { return &k_x_root; }
