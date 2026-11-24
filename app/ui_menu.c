/* Menu tree (Menu key on the main screen): zones, channels, contacts, manual dial, call log, SMS, scan, monitor,
 * settings (codeplug section 10 / journal, rset.c), the channel / mode tools of ui.c, GPS, info.
 *
 * Every screen is a list (Up/Down/knob move, Menu selects, Exit goes back) or a small editor. Lists scroll; the
 * row count adapts to the panel (D890UV 160x128 landscape: 9 rows, D168UV 128x160 portrait: 12 rows). */
#include <string.h>
#include "ui.h"
#include "ui_int.h"
#include "radio.h"
#include "rset.h"
#include "scan.h"
#include "calllog.h"
#include "cfwcp.h"
#include "gfx.h"
#include "hal.h"
#include "modem_core.h"
#include "voice.h"
#include "gps.h"
#include "aprs.h"
#include "sig.h"

#define ROW_H   11
#define TOP_Y   12
#define ROWS    ((LCD_H - TOP_Y - 13 - 9) / ROW_H)       /* one help line above the soft keys */

/* um_scr_t: ui_int.h (other UI files add screens: ui_stock.c) */

#define DEPTH 6
static struct { const um_scr_t *scr; int sel, top; } s_st[DEPTH];
static int s_depth;                                      /* 0 = menu closed */

static void push(const um_scr_t *scr, int sel)
{
    if(s_depth >= DEPTH) { s_depth = DEPTH - 1; }
    s_st[s_depth].scr = scr; s_st[s_depth].sel = sel; s_st[s_depth].top = 0;
    s_depth++;
}
static void pop(void) { if(s_depth > 0) { s_depth--; } if(!s_depth) { ui_show_main(); } }
static void close_all(void) { s_depth = 0; ui_show_main(); }
void um_push(const um_scr_t *scr, int sel) { push(scr, sel); }
void um_pop(void) { pop(); }
void um_close(void) { close_all(); }

bool um_active(void) { return s_depth > 0; }
void um_resume(void) { }

static void copy_name(char *d, const char *s, int n)
{
    int i = 0;
    for(; i < n && s[i]; i++) { d[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?'; }
    d[i] = 0;
}

/* ------------------------------------------------------------------ option list (stock style) */
static const char *const *s_ch_opt;
static int s_ch_n, s_ch_cur;
static const char *s_ch_title;
static void (*s_ch_set)(int v);
static int ch_count(void) { return s_ch_n; }
static void ch_item(int i, char *l, char *v) { fmt_s(l, s_ch_opt[i]); fmt_s(v, i == s_ch_cur ? "<" : ""); }
static void ch_select(int i) { s_ch_set(i); pop(); }
static um_scr_t k_choice = { NULL, ch_count, ch_item, ch_select, NULL, NULL, NULL };

static void choose(const char *title, const char *const *opt, int n, int cur, void (*set)(int))
{
    s_ch_opt = opt; s_ch_n = n; s_ch_cur = cur; s_ch_set = set; s_ch_title = title;
    k_choice.title = title;
    push(&k_choice, cur);
}
void um_choose(const char *title, const char *const *opt, int n, int cur, void (*set)(int)) { choose(title, opt, n, cur, set); }

/* ------------------------------------------------------------------ number entry (dial, date, time) */
typedef struct {
    const char *title, *hint;
    char d[16];
    int n, max;
    bool priv;                       /* dial: private call ('#' toggles) */
    bool allow_priv;
    const char *mask;                /* "####-##-##" style display mask, NULL = plain */
    void (*done)(void);
} numed_t;
static numed_t s_ne;

static void ne_draw(void)
{
    char t[32];
    gfx_clear(C_BLACK);
    ui_header(s_ne.title);
    if(s_ne.mask) {
        int j = 0, k = 0;
        for(const char *m = s_ne.mask; *m && j < 30; m++) { t[j++] = *m == '#' ? (k < s_ne.n ? s_ne.d[k++] : '_') : *m; }
        t[j] = 0;
    } else {
        memcpy(t, s_ne.d, (size_t)s_ne.n); t[s_ne.n] = '_'; t[s_ne.n + 1] = 0;
    }
    gfx_text_center(LCD_H / 2 - 20, t, C_YELLOW, C_BLACK, 2);
    if(s_ne.allow_priv) { gfx_text_center(LCD_H / 2 + 2, s_ne.priv ? "Private call" : "Group call", C_CYAN, C_BLACK, 1); }
    if(s_ne.hint) { gfx_text_center(LCD_H / 2 + 14, s_ne.hint, C_GREY, C_BLACK, 1); }
    ui_softkeys("OK", "BACK");
}

static bool ne_key(int i, int k)
{
    (void)i;
    if(k >= KEY_0 && k <= KEY_9) { if(s_ne.n < s_ne.max) { s_ne.d[s_ne.n++] = (char)('0' + k - KEY_0); } return true; }
    if(k == KEY_STAR) { if(s_ne.n) { s_ne.n--; } return true; }
    if(k == KEY_HASH && s_ne.allow_priv) { s_ne.priv = !s_ne.priv; return true; }
    if(k == KEY_MENU) { s_ne.d[s_ne.n] = 0; s_ne.done(); return true; }
    return false;
}
static int zero(void) { return 0; }
static const um_scr_t k_numed = { NULL, zero, NULL, NULL, ne_key, ne_draw, NULL };

static void numed(const char *title, const char *hint, int max, const char *mask, bool allow_priv, void (*done)(void))
{
    memset(&s_ne, 0, sizeof s_ne);
    s_ne.title = title; s_ne.hint = hint; s_ne.max = max; s_ne.mask = mask; s_ne.allow_priv = allow_priv; s_ne.done = done;
    push(&k_numed, 0);
}

static uint32_t ne_val(int from, int len)
{
    uint32_t v = 0;
    for(int i = from; i < from + len && i < s_ne.n; i++) { v = v * 10u + (uint32_t)(s_ne.d[i] - '0'); }
    return v;
}

/* ------------------------------------------------------------------ text entry (SMS), multi-tap */
static const char *const k_tap[10] = { " 0", ".,?!1-", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9" };
static struct { char t[SMS_MAX_TEXT + 1]; int n, last_key, tap; uint32_t last_t; bool lower; void (*done)(void); const char *title; } s_te;

static void te_draw(void)
{
    gfx_clear(C_BLACK);
    ui_header(s_te.title);
    int cols = (LCD_W - 8) / 6, y = TOP_Y + 4;
    char line[32];
    for(int p = 0; p <= s_te.n && y < LCD_H - 34; p += cols, y += 10) {
        int k = s_te.n - p < cols ? s_te.n - p : cols;
        memcpy(line, s_te.t + p, (size_t)k); line[k] = 0;
        gfx_text(4, y, line, C_WHITE, C_BLACK, 1);
        if(s_te.n - p < cols) { gfx_rect(4 + k * 6, y + 8, 5, 1, C_YELLOW); break; }
    }
    char h[28], *q = fmt_u(h, (uint32_t)s_te.n, 1);
    q = fmt_s(q, "/"); q = fmt_u(q, SMS_MAX_TEXT, 1); fmt_s(q, s_te.lower ? " abc #" : " ABC #");
    gfx_text(4, LCD_H - 23, h, C_GREY, C_BLACK, 1);
    ui_softkeys("SEND", "BACK");
}

static bool te_key(int i, int k)
{
    (void)i;
    uint32_t now = hal_millis();
    if(k >= KEY_0 && k <= KEY_9) {
        int d = k - KEY_0;
        const char *set = k_tap[d];
        if(d == s_te.last_key && now - s_te.last_t < 1200 && s_te.n) {
            s_te.tap = (s_te.tap + 1) % (int)strlen(set);
            s_te.n--;
        } else {
            s_te.tap = 0;
        }
        if(s_te.n < SMS_MAX_TEXT) {
            char c = set[s_te.tap];
            if(s_te.lower && c >= 'A' && c <= 'Z') { c = (char)(c + 32); }
            s_te.t[s_te.n++] = c;
        }
        s_te.last_key = d; s_te.last_t = now;
        return true;
    }
    if(k == KEY_STAR) { if(s_te.n) { s_te.n--; } s_te.last_key = -1; return true; }
    if(k == KEY_HASH) { s_te.lower = !s_te.lower; s_te.last_key = -1; return true; }
    if(k == KEY_MENU) { s_te.t[s_te.n] = 0; s_te.done(); return true; }
    return false;
}
static const um_scr_t k_texted = { NULL, zero, NULL, NULL, te_key, te_draw, NULL };

static void texted(const char *title, const char *init, void (*done)(void))
{
    memset(&s_te, 0, sizeof s_te);
    s_te.title = title; s_te.done = done; s_te.last_key = -1;
    if(init) { strncpy(s_te.t, init, SMS_MAX_TEXT); s_te.n = (int)strlen(s_te.t); }
    push(&k_texted, 0);
}

/* ------------------------------------------------------------------ zones */
static int zl_count(void) { return zone_count() + 1; }
static void zl_item(int i, char *l, char *v)
{
    fmt_s(l, zone_name(i - 1));
    char *p = fmt_u(v, (uint32_t)zone_size(i - 1), 1);
    if(i - 1 == g_radio.zone) { fmt_s(p, " <"); }
}
static void zl_select(int i)
{
    radio_set_zone(i - 1);
    ui_popup("ZONE", zone_name(i - 1), C_CYAN);
    close_all();
}
static const um_scr_t k_zones = { "ZONE", zl_count, zl_item, zl_select, NULL, NULL, NULL };

/* ------------------------------------------------------------------ channels of the zone */
static int cl_count(void) { return zone_size(g_radio.zone); }
static void cl_item(int i, char *l, char *v)
{
    int ch = zone_channel(g_radio.zone, i);
    channel_t c;
    if(ch < 0 || !channel_load(ch, &c)) { fmt_s(l, "?"); return; }
    char *p = fmt_u(l, (uint32_t)ch + 1, 3); *p++ = ' '; copy_name(p, c.name, 12);
    fmt_s(v, ch == g_radio.channel && !g_radio.vfo ? "<" : modem_mode_name(c.mode));
}
static void cl_select(int i)
{
    int ch = zone_channel(g_radio.zone, i);
    if(ch < 0) { return; }
    scan_stop(true);
    if(g_radio.vfo) { radio_toggle_vfo(); }
    radio_goto(ch);
    close_all();
}
static const char *cl_help(int i)
{
    static char t[28];
    int ch = zone_channel(g_radio.zone, i);
    channel_t c;
    if(ch < 0 || !channel_load(ch, &c)) { return ""; }
    char *p = ui_fmt_freq(t, c.rx_hz); *p++ = ' '; fmt_s(p, modem_mode_name(c.mode));
    return t;
}
static const um_scr_t k_chans = { "CHANNELS", cl_count, cl_item, cl_select, NULL, NULL, cl_help };

/* ------------------------------------------------------------------ contacts */
static int ct_count(void) { return cfwcp_contact_count(); }
static void ct_item(int i, char *l, char *v)
{
    const cp_contact_t *c = cfwcp_contact(i);
    if(!c) { return; }
    copy_name(l, c->name, 13);
    fmt_s(v, c->kind == 1 ? "P" : c->kind == 2 ? "All" : "G");
}
static const char *ct_help(int i)
{
    static char t[28];
    const cp_contact_t *c = cfwcp_contact(i);
    if(!c) { return "no contacts in the codeplug"; }
    char *p = fmt_s(t, c->kind == 1 ? "Private " : c->kind == 2 ? "All call " : "Group ");
    fmt_u(p, c->id, 1);
    return t;
}
static void ct_select(int i)
{
    const cp_contact_t *c = cfwcp_contact(i);
    if(!c) { return; }
    radio_set_target(c->kind == 2 ? 16777215u : c->id, c->kind == 1);
    char n[17];
    copy_name(n, c->name, 16);
    ui_popup("TX TARGET", n, C_CYAN);
    close_all();
}
static const um_scr_t k_contacts = { "CONTACTS", ct_count, ct_item, ct_select, NULL, NULL, ct_help };

/* ------------------------------------------------------------------ manual dial */
static void dial_done(void)
{
    if(!s_ne.n) { pop(); return; }
    uint32_t v = ne_val(0, s_ne.n);
    cfw_mode_t m = radio_current()->mode;
    uint32_t max = m == CFW_MODE_DMR ? 16776415u : m == CFW_MODE_YSF ? 99u : 65535u;
    if(v > max || (m != CFW_MODE_YSF && v == 0)) { ui_popup("INVALID ID", "", C_RED); return; }
    radio_set_target(v, s_ne.priv && m == CFW_MODE_DMR);
    char t[20], *p = fmt_s(t, m == CFW_MODE_YSF ? "DG-ID " : s_ne.priv ? "Private " : "TG ");
    fmt_u(p, v, 1);
    ui_popup("DIAL", t, C_CYAN);
    close_all();
}

static void open_dial(void)
{
    cfw_mode_t m = radio_current()->mode;
    switch(m) {
    case CFW_MODE_DMR:   numed("DIAL DMR ID / TG", "#=group/private *=del", 8, NULL, true, dial_done); break;
    case CFW_MODE_P25P1: case CFW_MODE_P25P2: numed("DIAL P25 TGID", "0-65535  *=del", 5, NULL, false, dial_done); break;
    case CFW_MODE_NXDN:  numed("DIAL NXDN GROUP", "0-65535  *=del", 5, NULL, false, dial_done); break;
    case CFW_MODE_YSF:   numed("YSF DG-ID", "0-99 (0 = all)", 2, NULL, false, dial_done); break;
    case CFW_MODE_DSTAR: ui_popup("D-STAR", "UR: Channel settings", C_ORANGE); break;
    default:             ui_popup("ANALOG FM", "dial: digital modes", C_ORANGE); break;
    }
}

/* ------------------------------------------------------------------ call log */
static int lg_count(void) { return calllog_count() + 1; }
static void lg_item(int i, char *l, char *v)
{
    if(i == calllog_count()) { fmt_s(l, calllog_count() ? "Clear log" : "(empty)"); return; }
    const call_rec_t *r = calllog_at(i);
    char *p = fmt_s(l, (r->flags & CL_OUT) ? "> " : "< ");
    if(r->flags & CL_OUT) { p = fmt_s(p, (r->flags & CL_PRIVATE) ? "P" : r->mode == CFW_MODE_YSF ? "DG" : "TG"); fmt_u(p, r->dst, 1); }
    else if(r->call[0]) { copy_name(p, r->call, 8); }
    else { fmt_u(p, r->src, 1); }
    if(r->t) { ui_fmt_time(v, r->t, false); } else { fmt_s(v, modem_mode_name((cfw_mode_t)r->mode)); }
}
static const char *lg_help(int i)
{
    static char t[32];
    const call_rec_t *r = calllog_at(i);
    if(!r) { return ""; }
    char *p = fmt_s(t, modem_mode_name((cfw_mode_t)r->mode)); *p++ = ' ';
    if(!(r->flags & CL_OUT)) { p = fmt_u(p, r->src, 1); p = fmt_s(p, ">"); }
    p = fmt_s(p, (r->flags & CL_PRIVATE) ? "P" : "TG"); p = fmt_u(p, r->dst, 1);
    if(r->flags & CL_ENC) { fmt_s(p, " ENC"); }
    return t;
}
static void lg_select(int i)
{
    if(i == calllog_count()) { calllog_clear(); return; }
    const call_rec_t *r = calllog_at(i);
    /* call back: a private call -> the caller, a group call -> its talkgroup */
    uint32_t id = (r->flags & CL_PRIVATE) && !(r->flags & CL_OUT) ? r->src : r->dst;
    if(!id && r->src) { id = r->src; }
    if(radio_current()->mode != (cfw_mode_t)r->mode) { radio_set_mode((cfw_mode_t)r->mode); }
    radio_set_target(id, (r->flags & CL_PRIVATE) != 0);
    char t[20], *p = fmt_s(t, (r->flags & CL_PRIVATE) ? "Private " : "TG ");
    fmt_u(p, id, 1);
    ui_popup("CALL BACK", t, C_CYAN);
    close_all();
}
static const um_scr_t k_calllog = { "CALL LOG", lg_count, lg_item, lg_select, NULL, NULL, lg_help };

/* ------------------------------------------------------------------ SMS (DMR) */
static uint32_t s_sms_dst;
static char s_qt[SMS_MAX_TEXT + 1];
static bool s_sms_group, s_sms_box_out;
static int s_sms_view;

static void sms_send_now(void)
{
    if(!s_te.n) { pop(); return; }
    bool ok = radio_send_sms(s_sms_dst, s_sms_group, s_te.t);
    sms_add_out(s_sms_dst, s_sms_group, s_te.t, ok);
    char t[20], *p = fmt_s(t, s_sms_group ? "TG " : "to ");
    fmt_u(p, s_sms_dst, 1);
    ui_popup(ok ? "SMS SENT" : "SMS FAILED", ok ? t : radio_sms_error(), ok ? C_GREEN : C_RED);
    close_all();
}

static void sms_to_done(void)
{
    if(!s_ne.n) { pop(); return; }
    s_sms_dst = ne_val(0, s_ne.n);
    s_sms_group = !s_ne.priv;
    pop();
    texted("NEW SMS", s_qt[0] ? s_qt : NULL, sms_send_now);
}

static void sms_new(void)
{
    if(radio_current()->mode != CFW_MODE_DMR) { ui_popup("SMS", "DMR channels only", C_ORANGE); return; }
    numed("SMS TO (ID / TG)", "#=group/private", 8, NULL, true, sms_to_done);
    const channel_t *c = radio_current();
    s_ne.priv = c->private_call;
    if(c->talkgroup) { s_ne.n = (int)(fmt_u(s_ne.d, c->talkgroup, 1) - s_ne.d); }
}

static void sv_draw(void)
{
    sms_rec_t *r = sms_at(s_sms_box_out, s_sms_view);
    gfx_clear(C_BLACK);
    char t[32], *p = fmt_s(t, s_sms_box_out ? "TO " : "FROM ");
    if(!r) { ui_header("SMS"); ui_softkeys(NULL, "BACK"); return; }
    p = fmt_u(p, r->peer, 1);
    if(r->flags & SMS_GROUP) { fmt_s(p, " (G)"); }
    ui_header(t);
    r->flags &= (uint8_t)~SMS_UNREAD;
    int y = TOP_Y + 4;
    if(r->t) { ui_fmt_time(t, r->t, true); gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 11; }
    int cols = (LCD_W - 8) / 6;
    for(int q = 0; q < r->len; q += cols, y += 10) {
        int k = r->len - q < cols ? r->len - q : cols;
        memcpy(t, r->text + q, (size_t)k); t[k] = 0;
        gfx_text(4, y, t, C_WHITE, C_BLACK, 1);
    }
    if(s_sms_box_out) { gfx_text(4, LCD_H - 23, (r->flags & SMS_SENT) ? "sent" : "not sent", C_GREY, C_BLACK, 1); }
    ui_softkeys(s_sms_box_out ? NULL : "REPLY", "BACK");
}
static bool sv_key(int i, int k)
{
    (void)i;
    sms_rec_t *r = sms_at(s_sms_box_out, s_sms_view);
    if(k == KEY_MENU && r && !s_sms_box_out) {
        s_sms_dst = r->peer; s_sms_group = false;
        pop();
        texted("REPLY SMS", NULL, sms_send_now);
        return true;
    }
    return false;
}
static const um_scr_t k_smsview = { NULL, zero, NULL, NULL, sv_key, sv_draw, NULL };

static int sb_count(void) { return sms_count(s_sms_box_out) + 1; }
static void sb_item(int i, char *l, char *v)
{
    if(i == sms_count(s_sms_box_out)) { fmt_s(l, sms_count(s_sms_box_out) ? "Delete all" : "(empty)"); return; }
    sms_rec_t *r = sms_at(s_sms_box_out, i);
    char *p = fmt_s(l, (r->flags & SMS_UNREAD) ? "*" : " ");
    copy_name(p, r->text, 12);
    fmt_u(v, r->peer, 1);
}
static void sb_select(int i)
{
    if(i == sms_count(s_sms_box_out)) { sms_clear(s_sms_box_out); return; }
    s_sms_view = i;
    push(&k_smsview, 0);
}
static um_scr_t k_smsbox = { NULL, sb_count, sb_item, sb_select, NULL, NULL, NULL };

/* quick texts: stock "PrefabricatedSMS" table (codeplug section 10, table 6, column 1 = text) */
static int qt_count(void)
{
    char b[8];
    if(!cfwcp_stock_value(6, 0xFF, b, sizeof b)) { return 0; }
    uint32_t n = 0;
    for(char *q = b; *q >= '0' && *q <= '9'; q++) { n = n * 10u + (uint32_t)(*q - '0'); }
    return n > 100 ? 100 : (int)n;
}
static void qt_item(int i, char *l, char *v)
{
    char b[24];
    (void)v;
    if(!cfwcp_stock_value_row(6, 1, i, b, sizeof b)) { b[0] = 0; }
    copy_name(l, b, 18);
}
static void qt_select(int i)
{
    if(!cfwcp_stock_value_row(6, 1, i, s_qt, sizeof s_qt)) { s_qt[0] = 0; }
    pop();
    sms_new();                                    /* target first, then the editor starts with the quick text */
}
static const um_scr_t k_quick = { "QUICK TEXT", qt_count, qt_item, qt_select, NULL, NULL, NULL };

static const char *const k_sms_items[] = { "New message", "Inbox", "Outbox", "Quick texts" };
static int sm_count(void) { return 4; }
static void sm_item(int i, char *l, char *v)
{
    fmt_s(l, k_sms_items[i]);
    if(i == 1) { char *p = fmt_u(v, (uint32_t)sms_count(false), 1); if(sms_unread()) { p = fmt_s(p, " ("); p = fmt_u(p, (uint32_t)sms_unread(), 1); fmt_s(p, ")"); } }
    if(i == 2) { fmt_u(v, (uint32_t)sms_count(true), 1); }
    if(i == 3) { fmt_u(v, (uint32_t)qt_count(), 1); }
}
static void sm_select(int i)
{
    s_qt[0] = 0;
    switch(i) {
    case 0: sms_new(); break;
    case 1: s_sms_box_out = false; k_smsbox.title = "INBOX"; push(&k_smsbox, 0); break;
    case 2: s_sms_box_out = true; k_smsbox.title = "OUTBOX"; push(&k_smsbox, 0); break;
    case 3: push(&k_quick, 0); break;
    }
}
static const um_scr_t k_sms = { "SMS (DMR)", sm_count, sm_item, sm_select, NULL, NULL, NULL };

/* ------------------------------------------------------------------ scan */
static int s_scan_pick = -1;          /* scan list chosen in the menu (-1 = channel's list, else zone) */
static int sc_default_list(void)
{
    uint8_t l = radio_current()->scanlist;
    return l < cfwcp_scanlist_count() ? l : -1;
}
static int sc_count(void) { return 3; }
static void sc_item(int i, char *l, char *v)
{
    int list = s_scan_pick >= -1 && s_scan_pick != -2 ? s_scan_pick : sc_default_list();
    switch(i) {
    case 0: fmt_s(l, scan_active() ? "Stop scan" : "Start scan"); fmt_s(v, scan_active() ? "ON" : "off"); break;
    case 1: fmt_s(l, "Scan list"); fmt_s(v, scan_name(scan_active() ? scan_list() : list)); break;
    case 2: fmt_s(l, "Nuisance del."); { char *p = fmt_u(v, (uint32_t)scan_deleted(), 1); (void)p; } break;
    }
}
static const char *sc_help(int i)
{
    if(i == 2) { return scan_active() ? "drops current channel" : "while scanning: * key"; }
    if(i == 1) { return cfwcp_scanlist_count() ? "codeplug lists / zone" : "no lists: zone"; }
    return "Exit/PTT on main stops";
}
static const char *s_sl_names[17];
static void sl_set(int v) { s_scan_pick = v - 1; }
static void sc_select(int i)
{
    int list = s_scan_pick != -2 ? s_scan_pick : sc_default_list();
    switch(i) {
    case 0:
        if(scan_active()) { scan_stop(true); ui_popup("SCAN OFF", "", C_CYAN); }
        else { scan_start(list); ui_popup(scan_active() ? "SCAN" : "NO CHANNELS", scan_name(list), C_CYAN); }
        close_all();
        break;
    case 1: {
        static char names[17][17];
        int n = cfwcp_scanlist_count() + 1;
        if(n > 17) { n = 17; }
        strcpy(names[0], "Zone");
        s_sl_names[0] = names[0];
        for(int k = 1; k < n; k++) { strcpy(names[k], scan_name(k - 1)); s_sl_names[k] = names[k]; }
        choose("SCAN LIST", s_sl_names, n, list + 1, sl_set);
        break;
    }
    case 2: if(scan_active()) { scan_nuisance(); } break;
    }
}
static const um_scr_t k_scan = { "SCAN", sc_count, sc_item, sc_select, NULL, NULL, sc_help };

/* ------------------------------------------------------------------ settings */
enum { ST_POWER = RS_COUNT, ST_KEYS, ST_BATT = ST_KEYS + RK_COUNT, ST_DATE, ST_TIME, ST_ENC, ST_SMSF, ST_N };
static const char *const k_pwr[] = { "Low", "Mid", "High", "Turbo" };
static const char *s_kf_names[KF_COUNT];
static int s_set_id;

static int st_slot(int i) { return i - ST_KEYS; }
static bool st_visible(int i) { return i < ST_KEYS || i >= ST_BATT || rset_key_slot_name(st_slot(i)); }
static int st_map(int i)                    /* visible row -> id */
{
    for(int id = 0, k = 0; id < ST_N; id++) { if(st_visible(id)) { if(k == i) { return id; } k++; } }
    return 0;
}
static int st_count(void) { int n = 0; for(int id = 0; id < ST_N; id++) { n += st_visible(id); } return n; }

static void st_item(int i, char *l, char *v)
{
    int id = st_map(i);
    if(id < RS_COUNT) { fmt_s(l, rset_def((rs_id_t)id)->label); fmt_s(v, rset_text((rs_id_t)id)); return; }
    if(id == ST_POWER) { fmt_s(l, "TX power"); fmt_s(v, k_pwr[radio_tx_power()]); return; }
    if(id == ST_BATT) { fmt_s(l, "Battery"); fmt_s(v, rset_batt_percent() ? "Percent" : "Icon"); return; }
    if(id == ST_ENC) { fmt_s(l, "Encryption"); fmt_s(v, rset_encryption() ? "On" : "Off"); return; }
    if(id == ST_SMSF) { fmt_s(l, "SMS format"); fmt_s(v, rset_sms_mformat() ? "M-SMS" : "cfw"); return; }
    if(id == ST_DATE || id == ST_TIME) {
        uint32_t t = rtc_now();
        fmt_s(l, id == ST_DATE ? "Date" : "Time");
        if(!t) { fmt_s(v, "--"); return; }
        if(id == ST_TIME) { ui_fmt_time(v, t, false); return; }
        int y, m, d;
        cal_date(t / 86400u, &y, &m, &d);
        char *p = fmt_u(v, (uint32_t)y, 4); *p++ = '-'; p = fmt_u(p, (uint32_t)m, 2); *p++ = '-'; fmt_u(p, (uint32_t)d, 2);
        return;
    }
    char *p = fmt_s(l, "Key "); fmt_s(p, rset_key_slot_name(st_slot(id)));
    fmt_s(v, rset_key_name(rset_key(st_slot(id))));
}

static const char *st_help(int i)
{
    int id = st_map(i);
    if(id < RS_COUNT) { return rset_def((rs_id_t)id)->effect; }
    if(id == ST_POWER) { return g_radio.vfo ? "VFO TX power" : "this channel's TX power"; }
    if(id == ST_DATE || id == ST_TIME) { return "RTC (battery backed)"; }
    if(id == ST_BATT) { return "status bar display"; }
    if(id == ST_ENC) { return "privacy keys - not on ham bands"; }
    if(id == ST_SMSF) { return "SMS TX format (RX: both)"; }
    return "side key function";
}

static void set_rs(int v) { rset_set((rs_id_t)s_set_id, (uint8_t)v); if(s_set_id == RS_BKL) { backlight_set((uint8_t)(v + 1)); } }
static const char *const k_smsf[] = { "cfw", "M-SMS" };
static const char *const k_batt[] = { "Icon", "Percent" };
static void set_batt(int v) { rset_set_batt_percent(v == 1); }
static void set_smsf(int v) { rset_set_sms_mformat(v == 1); }
static void set_pwr(int v) { radio_set_power((uint8_t)v); }
static void set_key(int v) { rset_set_key(st_slot(s_set_id), (uint8_t)v); }

static void date_done(void)
{
    if(s_ne.n < 8) { ui_popup("DATE", "YYYYMMDD", C_RED); return; }
    uint32_t y = ne_val(0, 4), m = ne_val(4, 2), d = ne_val(6, 2), t = rtc_now();
    if(y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) { ui_popup("INVALID DATE", "", C_RED); return; }
    rtc_set(cal_days((int)y, (int)m, (int)d) * 86400u + t % 86400u);
    pop();
}
static void time_done(void)
{
    if(s_ne.n < 4) { ui_popup("TIME", "HHMM", C_RED); return; }
    uint32_t h = ne_val(0, 2), mi = ne_val(2, 2), t = rtc_now();
    if(h > 23 || mi > 59) { ui_popup("INVALID TIME", "", C_RED); return; }
    rtc_set(t / 86400u * 86400u + h * 3600u + mi * 60u);
    pop();
}

/* Encryption gate: off by default; switching it on needs the warning screen confirmed (Menu). Even when on, encrypted
 * TX is refused on amateur bands (radio.c cfw_crypt_tx). */
static const char *const k_enc_warn[] = { "Encrypted TX is", "ILLEGAL on amateur", "radio bands (e.g.", "US 47 CFR 97.113).",
                                          "Licensed services", "only. Ham bands", "always TX in clear." };
static void enc_draw(void)
{
    gfx_clear(C_BLACK);
    ui_header("ENCRYPTION");
    int y = TOP_Y + 3;
    gfx_text(4, y, "WARNING", C_RED, C_BLACK, 1); y += 11;
    for(unsigned k = 0; k < sizeof k_enc_warn / sizeof k_enc_warn[0]; k++) {
        gfx_text(4, y, k_enc_warn[k], k == 1 ? C_ORANGE : C_WHITE, C_BLACK, 1); y += 10;
    }
    ui_softkeys("ENABLE", "CANCEL");
}
static bool enc_key(int i, int k)
{
    (void)i;
    if(k == KEY_MENU) { rset_set_encryption(true); radio_apply(); pop(); ui_popup("ENCRYPTION ON", "not on ham bands", C_ORANGE); return true; }
    return false;
}
static const um_scr_t k_enc = { NULL, zero, NULL, NULL, enc_key, enc_draw, NULL };

static void st_select(int i)
{
    int id = st_map(i);
    s_set_id = id;
    if(id < RS_COUNT) {
        const rs_def_t *d = rset_def((rs_id_t)id);
        choose(d->label, d->opt, d->n, rset_get((rs_id_t)id), set_rs);
    } else if(id == ST_POWER) {
        choose("TX POWER", k_pwr, 4, radio_tx_power(), set_pwr);
    } else if(id == ST_BATT) {
        choose("BATTERY", k_batt, 2, rset_batt_percent() ? 1 : 0, set_batt);
    } else if(id == ST_DATE) {
        numed("DATE", "year month day", 8, "####-##-##", false, date_done);
    } else if(id == ST_TIME) {
        numed("TIME", "hour minute", 4, "##:##", false, time_done);
    } else if(id == ST_ENC) {
        if(rset_encryption()) { rset_set_encryption(false); radio_apply(); ui_popup("ENCRYPTION OFF", "", C_GREEN); }
        else { push(&k_enc, 0); }
    } else if(id == ST_SMSF) {
        choose("SMS FORMAT", k_smsf, 2, rset_sms_mformat() ? 1 : 0, set_smsf);
    } else {
        for(int k = 0; k < KF_COUNT; k++) { s_kf_names[k] = rset_key_name((uint8_t)k); }
        choose(rset_key_slot_name(st_slot(id)), s_kf_names, KF_COUNT, rset_key(st_slot(id)), set_key);
    }
}
static const um_scr_t k_settings = { "SETTINGS", st_count, st_item, st_select, NULL, NULL, st_help };

/* ------------------------------------------------------------------ GPS */
static void gps_draw(void)
{
    char t[32], *p;
    const gps_fix_t *g = gps_fix();
    gfx_clear(C_BLACK);
    ui_header("GPS INFO");
    int y = TOP_Y + 4;
    if(!gps_present()) {
        gfx_text(4, y, "no GPS data", C_ORANGE, C_BLACK, 1); y += 10;
        gfx_text(4, y, CFW_GPS_TXT, C_GREY, C_BLACK, 1);
        ui_softkeys(NULL, "BACK");
        return;
    }
    p = fmt_s(t, g->valid ? "FIX  sats " : "NO FIX  sats "); fmt_u(p, g->sats, 1);
    gfx_text(4, y, t, g->valid ? C_GREEN : C_ORANGE, C_BLACK, 1); y += 11;
    gps_fmt_lat(t, g->lat_e7); gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 10;
    gps_fmt_lon(t, g->lon_e7); gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 10;
    p = fmt_s(t, "Alt "); p = fmt_u(p, (uint32_t)(g->alt_m < 0 ? 0 : g->alt_m), 1); fmt_s(p, " m");
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 10;
    gps_fmt_loc(t, g->lat_e7, g->lon_e7); p = t + strlen(t);
    gfx_text(4, y, t, C_CYAN, C_BLACK, 1); y += 10;
    if(g->utc) { p = fmt_s(t, "UTC "); ui_fmt_time(p, g->utc, true); gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 10; }
    p = fmt_s(t, "APRS beacons "); fmt_u(p, gps_beacons(), 1);
    gfx_text(4, y, t, C_GREY, C_BLACK, 1);
    ui_softkeys("BEACON", "BACK");
}
static bool gps_key(int i, int k)
{
    (void)i;
    if(k == KEY_MENU) {
        const char *err = gps_beacon_now();
        ui_popup(err ? "NO BEACON" : "APRS BEACON", err ? err : "sent (AFSK 1200)", err ? C_ORANGE : C_GREEN);
        return true;
    }
    return false;
}
static const um_scr_t k_gps = { NULL, zero, NULL, NULL, gps_key, gps_draw, NULL };

/* ------------------------------------------------------------------ APRS heard */
static int s_heard_sel;
static int hd_count(void) { return aprs_heard_count(); }
static void hd_item(int i, char *l, char *v)
{
    const aprs_heard_t *h = aprs_heard(i);
    if(!h) { return; }
    fmt_s(l, h->call);
    if(h->count > 1) { fmt_u(fmt_s(v, "x"), h->count, 1); }
}
static void hd_draw(void)
{
    const aprs_heard_t *h = aprs_heard(s_heard_sel);
    gfx_clear(C_BLACK);
    ui_header("APRS");
    if(!h) { ui_softkeys(NULL, "BACK"); return; }
    int y = TOP_Y + 3;
    gfx_text(4, y, h->call, C_CYAN, C_BLACK, 1); y += 11;
    char t[24];
    if(h->t) { ui_fmt_time(t, h->t, true); gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 11; }
    const int w = (LCD_W - 8) / 6;
    for(size_t o = 0, n = strlen(h->info); o < n && y < LCD_H - 24; o += (size_t)w, y += 10) {
        size_t k = n - o < (size_t)w ? n - o : (size_t)w;
        memcpy(t, h->info + o, k); t[k] = 0;
        gfx_text(4, y, t, C_WHITE, C_BLACK, 1);
    }
    ui_softkeys(NULL, "BACK");
}
static const um_scr_t k_heard_one = { NULL, zero, NULL, NULL, NULL, hd_draw, NULL };
static void hd_select(int i) { s_heard_sel = i; push(&k_heard_one, 0); }
static const char *hd_help(int i) { (void)i; return "stations heard (AFSK 1200)"; }
static const um_scr_t k_heard = { "APRS HEARD", hd_count, hd_item, hd_select, NULL, NULL, hd_help };

/* ------------------------------------------------------------------ analog signalling (app/sig.c) */
enum { SG_DTMF, SG_2T, SG_5T, SG_MDC, SG_ABOOK, SG_MBOOK, SG_HEARD, SG_CHAN, SG_N };
static int s_sg;                                         /* which list k_sig_list shows */
static int sgl_count(void)
{
    switch(s_sg) {
    case SG_DTMF: return sig_dtmf_count();
    case SG_2T: return sig_2t_count();
    case SG_5T: return sig_5t_count();
    case SG_MDC: return sig_mdc_count();
    case SG_ABOOK: return sig_abook_count();
    case SG_MBOOK: return sig_mbook_count();
    case SG_HEARD: return sig_heard_count();
    default: return 0;
    }
}
static void hex4(char *v, uint16_t u)
{
    static const char hx[] = "0123456789ABCDEF";
    for(int i = 0; i < 4; i++) { v[i] = hx[(u >> (12 - 4 * i)) & 15]; }
    v[4] = 0;
}
static void sgl_item(int i, char *l, char *v)
{
    char a[26], b[18];
    switch(s_sg) {
    case SG_DTMF: sig_dtmf(i, a, sizeof a); fmt_u(fmt_s(l, "DTMF "), (uint32_t)i + 1, 1); copy_name(v, a, 12); break;
    case SG_2T: { sig_2t_t e; sig_2t(i, &e); copy_name(l, e.name[0] ? e.name : "2Tone", 12);
        char *p = fmt_u(v, e.f1, 1); *p++ = '/'; fmt_u(p, e.f2, 1); break; }
    case SG_5T: { sig_5t_t e; sig_5t(i, &e); copy_name(l, e.name[0] ? e.name : sig_5t_std_name(e.std), 12); copy_name(v, e.seq, 10); break; }
    case SG_MDC: case SG_MBOOK: { sig_mdc_t e; if(s_sg == SG_MDC) { sig_mdc(i, &e); } else { sig_mbook(i, &e); }
        copy_name(l, e.name[0] ? e.name : e.group ? "Group" : "Unit", 12); hex4(v, e.unit); break; }
    case SG_ABOOK: sig_abook(i, a, sizeof a, b, sizeof b); copy_name(l, b[0] ? b : a, 14); copy_name(v, a, 10); break;
    case SG_HEARD: { const sig_heard_t *h = sig_heard(i); if(!h) { break; }
        copy_name(l, h->name[0] ? h->name : h->text, 14);
        fmt_s(v, h->kind == SIG_DTMF ? "DTMF" : h->kind == SIG_2TONE ? "2T" : h->kind == SIG_5TONE ? "5T" : "MDC"); break; }
    default: break;
    }
}
static void sgl_select(int i)
{
    const char *err = NULL, *what = "SENT";
    switch(s_sg) {
    case SG_DTMF: err = sig_send(SIG_DTMF, i); break;
    case SG_2T: err = sig_send(SIG_2TONE, i); break;
    case SG_5T: err = sig_send(SIG_5TONE, i); break;
    case SG_MDC: err = sig_send(SIG_MDC, i); break;
    case SG_ABOOK: { char a[26], b[18]; sig_abook(i, a, sizeof a, b, sizeof b); err = sig_send_dtmf(a); break; }
    case SG_MBOOK: { sig_mdc_t e; sig_mbook(i, &e); err = sig_call_mdc(e.unit); what = "CALL ALERT"; break; }
    case SG_HEARD: { const sig_heard_t *h = sig_heard(i); if(h) { ui_popup(h->text, h->to_me ? "call to this radio" : h->name, C_CYAN); } return; }
    default: return;
    }
    ui_popup(err ? "NOT SENT" : what, err ? err : "", err ? C_ORANGE : C_GREEN);
}
static const char *sgl_help(int i) { (void)i; return s_sg == SG_HEARD ? "decoded (newest first)" : "Menu: send on this channel"; }
static const um_scr_t k_sig_list = { "SIGNALLING", sgl_count, sgl_item, sgl_select, NULL, NULL, sgl_help };

static const char *const k_sig_items[SG_N] = { "Send DTMF", "Send 2Tone", "Send 5Tone", "Send " , "Analog book",
                                               " book", "Heard", "This channel" };
static const char *const k_sig_kind[SIG_KIND_N] = { "Off", "DTMF", "2Tone", "5Tone", "" };
static int sg_count(void) { return SG_N; }
static void sg_item(int i, char *l, char *v)
{
    char *p = fmt_s(l, k_sig_items[i]);
    if(i == SG_MDC) { fmt_s(p, sig_mdc_label()); }
    if(i == SG_MBOOK) { fmt_s(fmt_s(l, sig_mdc_label()), " book"); }
    int n = 0;
    switch(i) {
    case SG_CHAN: { const sig_chan_t *c = sig_chan(); fmt_s(v, c->kind == SIG_MDC ? sig_mdc_label() : k_sig_kind[c->kind]); return; }
    default: s_sg = i; n = sgl_count(); break;
    }
    fmt_u(v, (uint32_t)n, 1);
}
static void sg_draw_chan(void)
{
    const sig_chan_t *c = sig_chan();
    gfx_clear(C_BLACK);
    ui_header("SIGNALLING");
    int y = TOP_Y + 3;
    char t[32], *p;
    p = fmt_s(t, "Opt signal "); fmt_s(p, c->kind == SIG_MDC ? sig_mdc_label() : k_sig_kind[c->kind]);
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 11;
    static const char *const pt[4] = { "Off", "Start", "End", "Start+End" };
    p = fmt_s(t, "PTT ID "); fmt_s(p, pt[c->pttid & 3]); gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 11;
    p = fmt_s(t, "DTMF "); p = fmt_u(p, c->dtmf_id, 1); p = fmt_s(p, " 2T "); p = fmt_u(p, c->tone2_id, 1);
    p = fmt_s(p, " 5T "); fmt_u(p, c->tone5_id, 1); gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 11;
    p = fmt_s(t, sig_mdc_label()); p = fmt_s(p, " entry "); fmt_u(p, c->mdc_id, 1); gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 11;
    p = fmt_s(t, "2T decode "); if(c->tone2_dec) { fmt_u(p, c->tone2_dec, 1); } else { fmt_s(p, "off"); }
    gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 11;
    if(radio_current()->mode != CFW_MODE_FM || g_radio.vfo) { gfx_text(4, y, "FM memory channels only", C_ORANGE, C_BLACK, 1); }
    ui_softkeys(NULL, "BACK");
}
static const um_scr_t k_sig_chan = { NULL, zero, NULL, NULL, NULL, sg_draw_chan, NULL };
static void sg_select(int i)
{
    if(i == SG_CHAN) { push(&k_sig_chan, 0); return; }
    s_sg = i;
    if(!sgl_count()) { ui_popup(k_sig_items[i][0] == 'S' ? "LIST EMPTY" : "NONE", i == SG_HEARD ? "nothing decoded" : "CPS: Analog", C_ORANGE); return; }
    push(&k_sig_list, 0);
}
static const char *sg_help(int i) { return i <= SG_MDC ? "TX on this FM channel" : i == SG_CHAN ? "codeplug channel columns" : NULL; }
static const um_scr_t k_sig = { "SIGNALLING", sg_count, sg_item, sg_select, NULL, NULL, sg_help };

/* ------------------------------------------------------------------ root */
enum { R_ZONE, R_CHANNELS, R_CONTACTS, R_DIAL, R_CALLLOG, R_SMS, R_SCAN, R_MONITOR, R_SETTINGS, R_MODE, R_CHSET,
       R_STEP, R_RXPOL, R_DEEMPH, R_TONE, R_SIG, R_EXTRAS, R_GPS, R_INFO, R_SAVE, R_HEARD, R_N };
static const char *const k_root[R_N] = { "Zone", "Channels", "Contacts", "Manual dial", "Call log", "SMS", "Scan",
                                         "Monitor", "Settings", "Mode", "Channel settings", "Step", "RX polarity",
                                         "De-emphasis comp", "Test tone", "Signalling", "Radio extras", "GPS / APRS", "Info",
                                         "Save now", "APRS heard" };   /* tests count the last rows from the end */

static void rt_item(int i, char *l, char *v)
{
    fmt_s(l, k_root[i]);
    switch(i) {
    case R_ZONE: copy_name(v, zone_name(g_radio.zone), 8); break;
    case R_CHANNELS: fmt_u(v, (uint32_t)zone_size(g_radio.zone), 1); break;
    case R_CONTACTS: fmt_u(v, (uint32_t)cfwcp_contact_count(), 1); break;
    case R_DIAL: { const channel_t *c = radio_current();
        if(c->mode == CFW_MODE_YSF) { fmt_u(fmt_s(v, "DG"), c->dgid, 1); }
        else if(c->mode != CFW_MODE_FM && c->mode != CFW_MODE_DSTAR) { fmt_u(fmt_s(v, c->private_call ? "P" : "TG"), c->talkgroup, 1); }
        break; }
    case R_CALLLOG: fmt_u(v, (uint32_t)calllog_count(), 1); break;
    case R_SMS: if(sms_unread()) { fmt_s(fmt_u(v, (uint32_t)sms_unread(), 1), " new"); } break;
    case R_SCAN: fmt_s(v, scan_active() ? "ON" : "off"); break;
    case R_MONITOR: fmt_s(v, g_radio.monitor ? "ON" : "off"); break;
    case R_MODE: fmt_s(v, modem_mode_name(radio_current()->mode)); break;
    case R_STEP: { char *p = v; uint32_t hz = g_radio.step_hz;
        p = fmt_u(p, hz / 1000u, 1); if(hz % 1000u) { *p++ = '.'; uint32_t f = hz % 1000u; while(f) { *p++ = (char)('0' + f / 100u); f = (f % 100u) * 10u; } }
        fmt_s(p, "k"); break; }
    case R_RXPOL: fmt_s(v, g_radio.rx_invert ? "invert" : "normal"); break;
    case R_DEEMPH: if(g_radio.deemph_us) { fmt_s(fmt_u(v, g_radio.deemph_us, 1), "us"); } else { fmt_s(v, "off"); } break;
    case R_TONE: if(voice_test_tone()) { fmt_s(fmt_u(v, voice_test_tone(), 1), "Hz"); } else { fmt_s(v, "off"); } break;
    case R_GPS: fmt_s(v, gps_fix()->valid ? "fix" : gps_present() ? "no fix" : ""); break;
    case R_HEARD: fmt_u(v, (uint32_t)aprs_heard_count(), 1); break;
    case R_SIG: if(sig_heard_count()) { fmt_s(fmt_u(v, (uint32_t)sig_heard_count(), 1), " heard"); } break;
    default: break;
    }
}

static int rt_count(void) { return R_N; }
static void rt_select(int i)
{
    switch(i) {
    case R_ZONE: push(&k_zones, g_radio.zone + 1); break;
    case R_CHANNELS: { int k = zone_find(g_radio.zone, g_radio.channel); push(&k_chans, k < 0 ? 0 : k); break; }
    case R_CONTACTS:
        if(!cfwcp_contact_count()) { ui_popup("NO CONTACTS", "CPS: Talk Groups", C_ORANGE); break; }
        push(&k_contacts, 0); break;
    case R_DIAL: open_dial(); break;
    case R_CALLLOG: push(&k_calllog, 0); break;
    case R_SMS: push(&k_sms, 0); break;
    case R_SCAN: s_scan_pick = -2; push(&k_scan, 0); break;
    case R_MONITOR: g_radio.monitor = !g_radio.monitor; ui_popup("MONITOR", g_radio.monitor ? "squelch open" : "off", C_CYAN); break;
    case R_SETTINGS: push(&k_settings, 0); break;
    case R_MODE: s_depth = 0; ui_show_mode_picker(); break;
    case R_CHSET: s_depth = 0; ui_show_chan_edit(); break;
    case R_STEP: g_radio.step_hz = g_radio.step_hz == 12500 ? 25000 : g_radio.step_hz == 25000 ? 5000 :
                                   g_radio.step_hz == 5000 ? 6250 : 12500; break;
    case R_RXPOL: g_radio.rx_invert = !g_radio.rx_invert; radio_apply(); break;
    case R_DEEMPH: g_radio.deemph_us = g_radio.deemph_us ? 0 : 750; radio_apply(); break;
    case R_TONE: voice_set_test_tone(voice_test_tone() ? 0 : 440); break;
    case R_SIG: push(&k_sig, 0); break;
    case R_GPS: push(&k_gps, 0); break;
    case R_HEARD:
        if(!aprs_heard_count()) { ui_popup("APRS HEARD", "none (FM channel)", C_ORANGE); break; }
        push(&k_heard, 0); break;
    case R_INFO: s_depth = 0; ui_show_info(); break;
    case R_EXTRAS: push(ui_stock_root(), 0); break;      /* ui_stock.c: FM / AM radio, roaming, device info, options */
    case R_SAVE: radio_save(); close_all(); ui_popup("SAVED", rset_in_codeplug() ? "codeplug + radio" : "radio", C_GREEN); break;
    }
}
static const char *rt_help(int i)
{
    switch(i) {
    case R_DIAL: return "TX target of this channel";
    case R_SMS: return "DMR text messages";
    case R_SETTINGS: return rset_in_codeplug() ? "saved to the codeplug" : "saved in the radio";
    case R_MODE: case R_CHSET: return "this channel";
    default: return NULL;
    }
}
static const um_scr_t k_root_scr = { "MENU", rt_count, rt_item, rt_select, NULL, NULL, rt_help };

void um_open(int which)
{
    s_depth = 0;
    push(&k_root_scr, 0);
    switch(which) {
    case UM_CALLLOG: s_st[0].sel = R_CALLLOG; push(&k_calllog, 0); break;
    case UM_GPS: s_st[0].sel = R_GPS; push(&k_gps, 0); break;
    case UM_SMS: s_st[0].sel = R_SMS; push(&k_sms, 0); break;
    case UM_CONTACTS: s_st[0].sel = R_CONTACTS; push(&k_contacts, 0); break;
    case UM_ZONES: s_st[0].sel = R_ZONE; push(&k_zones, g_radio.zone + 1); break;
    case UM_SCAN: s_st[0].sel = R_SCAN; s_scan_pick = -2; push(&k_scan, 0); break;
    case UM_SETTINGS: s_st[0].sel = R_SETTINGS; push(&k_settings, 0); break;
    default: break;
    }
}

/* leaving the settings list writes the edits into the codeplug journal (one flash page) */
static void leaving(const um_scr_t *scr)
{
    if(scr == &k_settings && rset_dirty()) {
        bool ok = rset_save();
        if(rset_in_codeplug()) { ui_popup(ok ? "SAVED" : "NOT SAVED", ok ? "to the codeplug" : "codeplug full / HW", ok ? C_GREEN : C_RED); }
    }
}

void um_key(int k)
{
    if(!s_depth) { return; }
    const um_scr_t *scr = s_st[s_depth - 1].scr;
    int *sel = &s_st[s_depth - 1].sel;
    if(scr->key && scr->key(*sel, k)) { return; }
    if(scr->draw) {                                     /* editor / view: only Exit is left */
        if(k == KEY_EXIT) { pop(); }
        return;
    }
    int n = scr->count();
    switch(k) {
    case KEY_UP: case KEY_KNOB_CCW: if(n) { *sel = (*sel + n - 1) % n; } break;
    case KEY_DOWN: case KEY_KNOB_CW: if(n) { *sel = (*sel + 1) % n; } break;
    case KEY_EXIT: leaving(scr); pop(); break;
    case KEY_MENU: if(n && scr->select) { scr->select(*sel); } break;
    default:
        if(k >= KEY_1 && k <= KEY_9 && k - KEY_1 < n) {     /* digit = row (first 9 rows), like the stock radio */
            *sel = k - KEY_1;
            if(scr->select) { scr->select(*sel); }
        }
        break;
    }
}

void um_draw(void)
{
    if(!s_depth) { return; }
    int lvl = s_depth - 1;
    const um_scr_t *scr = s_st[lvl].scr;
    if(scr->draw) { scr->draw(); return; }
    int n = scr->count(), *sel = &s_st[lvl].sel, *top = &s_st[lvl].top;
    if(*sel >= n) { *sel = n ? n - 1 : 0; }
    if(*sel < *top) { *top = *sel; }
    if(*sel >= *top + ROWS) { *top = *sel - ROWS + 1; }
    gfx_clear(C_BLACK);
    char h[24], *p = fmt_s(h, scr->title ? scr->title : "");
    if(n > ROWS) { *p++ = ' '; p = fmt_u(p, (uint32_t)*sel + 1, 1); *p++ = '/'; fmt_u(p, (uint32_t)n, 1); }
    ui_header(h);
    for(int r = 0; r < ROWS && *top + r < n; r++) {
        int i = *top + r;
        char l[32], v[24];
        l[0] = v[0] = 0;
        scr->item(i, l, v);
        bool s = i == *sel;
        uint16_t bg = s ? C_CYAN : C_BLACK, fg = s ? C_BLACK : C_WHITE;
        int y = TOP_Y + r * ROW_H;
        gfx_rect(0, y, LCD_W, ROW_H, bg);
        int vw = gfx_text_width(v, 1), maxl = (LCD_W - 8 - vw - (vw ? 4 : 0)) / 6;
        if((int)strlen(l) > maxl) { l[maxl > 0 ? maxl : 0] = 0; }
        gfx_text(3, y + 2, l, fg, bg, 1);
        if(vw) { gfx_text(LCD_W - 3 - vw, y + 2, v, s ? C_BLACK : C_CYAN, bg, 1); }
    }
    if(n > ROWS) {                                     /* scroll bar */
        int hgt = ROWS * ROW_H, bh = hgt * ROWS / n, by = TOP_Y + hgt * *top / n;
        gfx_rect(LCD_W - 1, TOP_Y, 1, hgt, C_DGREY);
        gfx_rect(LCD_W - 1, by, 1, bh < 3 ? 3 : bh, C_YELLOW);
    }
    const char *hl = scr->help ? scr->help(*sel) : NULL;
    if(hl && *hl) {
        char t[32];
        copy_name(t, hl, (LCD_W - 6) / 6);
        gfx_text(3, LCD_H - 22, t, C_GREY, C_BLACK, 1);
    }
    ui_softkeys("SELECT", s_depth > 1 ? "BACK" : "EXIT");
}
