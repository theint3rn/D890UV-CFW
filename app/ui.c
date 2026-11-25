/* Screens: boot checks, main (frequency / mode / battery / decoded call), menu, mode picker, per-mode channel
 * settings, info. Redraws into the back buffer every frame; gfx_flush() only sends what changed.
 *
 * Mode switching (works in memory and VFO mode; the mode is a per-channel setting):
 *   PF3 (top key) or P1   next mode, short popup says what the mode can do on this build
 *   Menu > Mode           picker with every mode and its state (decoding / no demodulator / no decoder)
 *   Menu > Channel set.   per-mode settings (DMR CC/slot/TG/Tier III, P25 NAC/TG/trunk, NXDN RAN, D-STAR
 *                         UR/RPT1/RPT2/MY, YSF DG-ID); edits are kept per channel and saved with Save / power off */
#include <string.h>
#include "ui.h"
#include "prog.h"
#include "radio.h"
#include "stockcp.h"
#include "gfx.h"
#include "hal.h"
#include "modem_core.h"
#include "dsp_backend.h"
#include "voice.h"
#include "ui_int.h"
#include "rset.h"
#include "battdisp.h"
#include "stockopt.h"
#include "stockrun.h"
#include "stockfx.h"
#include "spk.h"
#include "mandown.h"
#include "emerg.h"
#include "stockdlg.h"
#include "scan.h"
#include "calllog.h"
#include "gps.h"
#include "sig.h"

#ifndef VERSION
#define VERSION "?"                     /* Makefile: CFW_VER */
#endif
#ifndef CFW_BUILD
#define CFW_BUILD "?"                   /* Makefile: tree, build kind, date */
#endif

/* board texts (a board_cfg.h, included by hal.h, may override them) */
#ifndef CFW_BOARD_NAME
#define CFW_BOARD_NAME   "D890UV"
#endif
#ifndef CFW_RF_CHIPS
#define CFW_RF_CHIPS     2
#endif
#ifndef CFW_TXT_MODEKEY
#define CFW_TXT_MODEKEY  "PF3/P1: change mode"
#endif
#ifndef CFW_TXT_SOFT_MEM
#define CFW_TXT_SOFT_MEM "PF3:MODE P2:MEM"
#define CFW_TXT_SOFT_VFO "PF3:MODE P2:VFO"
#endif
#ifndef CFW_TXT_DAC
#define CFW_TXT_DAC      "PWM PB9 "
#endif
#ifndef CFW_ADC_TEMP_SLOT
#define CFW_ADC_TEMP_SLOT 4
#endif

enum { SCR_BOOT, SCR_MAIN, SCR_MENU, SCR_MODE, SCR_EDIT, SCR_INFO, SCR_LOCK };   /* SCR_MENU = ui_menu.c */
static int s_screen = SCR_BOOT;
static uint32_t s_boot_until;
static int s_mode_sel;
static bool s_locked;
static char s_entry[10];      /* digits typed: VFO frequency or memory channel number */
static int s_entry_n;
static uint32_t s_entry_t;
static char s_popup[2][28];
static uint32_t s_popup_until;
static uint16_t s_popup_col;

ui_boot_t g_boot;

static uint32_t s_last_key;            /* hal_millis() of the last key (backlight, auto key lock) */
static bool s_bl_off, s_tot_shown;
static int s_menu_sel;

/* ------------------------------------------------------------------ helpers */
#define fmt_freq ui_fmt_freq
char *ui_fmt_freq(char *p, uint32_t hz)
{
    p = fmt_u(p, hz / 1000000u, 3);
    *p++ = '.';
    return fmt_u(p, (hz % 1000000u) / 100u, 4);   /* 146.5200 */
}

/* 12500 -> "12.5", 6250 -> "6.25", 25000 -> "25" */
static char *fmt_khz(char *p, uint32_t hz)
{
    p = fmt_u(p, hz / 1000u, 1);
    uint32_t f = hz % 1000u;
    if(f) {
        *p++ = '.';
        while(f) { *p++ = (char)('0' + f / 100u); f = (f % 100u) * 10u; }
        *p = 0;
    }
    return p;
}

/* what a mode can do in this build, for the picker / popup / main screen */
typedef enum { AV_FM, AV_RUN, AV_NO_DEMOD, AV_NO_PROTO } avail_t;

static avail_t mode_avail(cfw_mode_t m)
{
    if(m == CFW_MODE_FM) { return AV_FM; }
    if(!modem_find(m)) { return AV_NO_PROTO; }
    if(radio_current()->mode == m) {
        return modem_availability() == MODEM_NO_DEMOD ? AV_NO_DEMOD : AV_RUN;
    }
    return modem_backend_for(m) ? AV_RUN : AV_NO_DEMOD;
}

static bool mcu_dsp(cfw_mode_t m)
{
    const char *b = modem_backend_for(m);
    return b && b[0] == 'M';                         /* "MCU-DSP" */
}

static const char *avail_text(avail_t a, cfw_mode_t m)
{
    switch(a) {
    case AV_FM:       return "analog FM";
    case AV_RUN:      return mcu_dsp(m) ? "demod: unverified on HW" : "decoder running";
    case AV_NO_DEMOD: return "decoder ok, no demod";
    default:          return "no decoder linked";
    }
}

static uint16_t avail_col(avail_t a)
{
    return a == AV_FM || a == AV_RUN ? C_GREEN : a == AV_NO_DEMOD ? C_ORANGE : C_RED;
}

void ui_popup(const char *l1, const char *l2, uint16_t col);
#define popup ui_popup
void ui_popup(const char *l1, const char *l2, uint16_t col)
{
    strncpy(s_popup[0], l1, sizeof s_popup[0] - 1);
    strncpy(s_popup[1], l2, sizeof s_popup[1] - 1);
    s_popup_col = col;
    s_popup_until = hal_millis() + 1600;
}

static void mode_changed_popup(void)
{
    cfw_mode_t m = radio_current()->mode;
    char t[24];
    fmt_s(fmt_s(t, "MODE  "), modem_mode_name(m));
    avail_t a = mode_avail(m);
    popup(t, avail_text(a, m), avail_col(a));
}

static void set_mode(cfw_mode_t m)
{
    radio_set_mode(m);
    mode_changed_popup();
}

/* ------------------------------------------------------------------ field editor */
typedef enum { F_NUM, F_HEX, F_CHOICE, F_CALL, F_CTCSS, F_MODE } ftype_t;
typedef struct {
    const char *label;
    ftype_t t;
    uint32_t min, max;
    const char *const *choices;
    void *ptr;
    uint8_t size;            /* bytes of *ptr for NUM/HEX/CHOICE */
    bool global;             /* radio-wide (my id / my call), not a channel field */
} field_t;

#define MAX_FIELDS 9
static field_t s_f[MAX_FIELDS];
static int s_nf, s_fsel, s_cur;           /* field count, selected field, cursor in a CALL field */
static bool s_fresh = true;               /* next digit replaces the number (just selected) */
static const char *const k_onoff[] = { "Off", "On" };
static const char *const k_bw[] = { "12.5k", "25k" };
static const char k_callset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-";

static void add_field(const char *label, ftype_t t, uint32_t min, uint32_t max, const char *const *ch, void *ptr,
                      uint8_t size, bool global)
{
    if(s_nf < MAX_FIELDS) { s_f[s_nf++] = (field_t){ label, t, min, max, ch, ptr, size, global }; }
}

static void build_fields(void)
{
    channel_t *c = radio_edit();
    s_nf = 0;
    add_field("Mode", F_MODE, 0, CFW_MODE_COUNT - 1, NULL, NULL, 0, false);
    if(c->tx_hz && c->tx_hz != c->rx_hz) { add_field("Talkaround", F_CHOICE, 0, 1, k_onoff, &c->talkaround, 1, false); }
    switch(c->mode) {
    case CFW_MODE_FM:
        add_field("Bandwidth", F_CHOICE, 0, 1, k_bw, &c->wide, 1, false);
        add_field("CTCSS", F_CTCSS, 0, 0, NULL, &c->ctcss_dhz, 2, false);
        break;
    case CFW_MODE_DMR:
        add_field("Color code", F_NUM, 0, 15, NULL, &c->color_code, 1, false);
        add_field("Time slot", F_NUM, 1, 2, NULL, &c->slot, 1, false);
        add_field("Talkgroup", F_NUM, 1, 16776415, NULL, &c->talkgroup, 4, false);
        add_field("Tier III", F_CHOICE, 0, 1, k_onoff, &c->trunk, 1, false);
        add_field("TDMA direct", F_CHOICE, 0, 1, k_onoff, &c->dmr_tdma, 1, false);
        add_field("My DMR ID", F_NUM, 1, 16776415, NULL, &g_radio.my_id, 4, true);
        break;
    case CFW_MODE_P25P1:
        add_field("NAC", F_HEX, 0, 0xFFF, NULL, &c->nac, 2, false);
        add_field("Talkgroup", F_NUM, 0, 65535, NULL, &c->talkgroup, 4, false);
        add_field("Trunk CC", F_CHOICE, 0, 1, k_onoff, &c->trunk, 1, false);
        add_field("My unit ID", F_NUM, 1, 16777215, NULL, &g_radio.my_id, 4, true);
        break;
    case CFW_MODE_P25P2:
        add_field("NAC", F_HEX, 0, 0xFFF, NULL, &c->nac, 2, false);
        add_field("WACN", F_HEX, 0, 0xFFFFF, NULL, &c->wacn, 4, false);
        add_field("System ID", F_HEX, 0, 0xFFF, NULL, &c->sysid, 2, false);
        add_field("Slot", F_NUM, 1, 2, NULL, &c->slot, 1, false);
        add_field("Talkgroup", F_NUM, 0, 65535, NULL, &c->talkgroup, 4, false);
        add_field("My unit ID", F_NUM, 1, 16777215, NULL, &g_radio.my_id, 4, true);
        break;
    case CFW_MODE_NXDN:
        add_field("RAN (0=any)", F_NUM, 0, 63, NULL, &c->ran, 1, false);
        add_field("Group", F_NUM, 0, 65535, NULL, &c->talkgroup, 4, false);
        add_field("My unit ID", F_NUM, 1, 65535, NULL, &g_radio.my_id, 4, true);
        break;
    case CFW_MODE_DSTAR:
        add_field("UR", F_CALL, 0, 0, NULL, c->ur, 8, false);
        add_field("RPT1", F_CALL, 0, 0, NULL, c->rpt1, 8, false);
        add_field("RPT2", F_CALL, 0, 0, NULL, c->rpt2, 8, false);
        add_field("MY", F_CALL, 0, 0, NULL, g_radio.my_call, 8, true);
        break;
    case CFW_MODE_YSF:
        add_field("DG-ID (0=all)", F_NUM, 0, 99, NULL, &c->dgid, 1, false);
        add_field("MY", F_CALL, 0, 0, NULL, g_radio.my_call, 8, true);
        break;
    default:
        break;
    }
    if(s_fsel >= s_nf) { s_fsel = 0; }
}

static uint32_t fget(const field_t *f)
{
    switch(f->size) {
    case 1: return *(uint8_t *)f->ptr;
    case 2: return *(uint16_t *)f->ptr;
    default: return *(uint32_t *)f->ptr;
    }
}

static void fset(const field_t *f, uint32_t v)
{
    switch(f->size) {
    case 1: *(uint8_t *)f->ptr = (uint8_t)v; break;
    case 2: *(uint16_t *)f->ptr = (uint16_t)v; break;
    default: *(uint32_t *)f->ptr = v; break;
    }
}

static int ctcss_idx(uint16_t dhz)
{
    for(int i = 0; i < ctcss_count(); i++) { if(ctcss_at(i) == dhz) { return i + 1; } }
    return 0;
}

static void call_fix(char *s)       /* keep 8 chars, space padded */
{
    size_t n = strlen(s);
    while(n < 8) { s[n++] = ' '; }
    s[8] = 0;
}

static void field_text(const field_t *f, char *t)
{
    t[0] = 0;
    switch(f->t) {
    case F_MODE: fmt_s(t, modem_mode_name(radio_current()->mode)); break;
    case F_NUM: fmt_u(t, fget(f), 1); break;
    case F_HEX: fmt_hex(t, fget(f), f->max > 0xFFF ? 5 : 3); break;
    case F_CHOICE: fmt_s(t, f->choices[fget(f) ? 1 : 0]); break;
    case F_CTCSS: {
        uint16_t v = *(uint16_t *)f->ptr;
        if(!v) { fmt_s(t, "Off"); break; }
        char *p = fmt_u(t, v / 10u, 1); *p++ = '.'; fmt_u(p, v % 10u, 1);
        break;
    }
    case F_CALL: memcpy(t, f->ptr, 8); t[8] = 0; break;
    }
}

static void field_changed(const field_t *f)
{
    if(f->global) { radio_commit(false); return; }
    radio_commit(true);
}

static void field_step(field_t *f, int d)
{
    switch(f->t) {
    case F_MODE:
        radio_cycle_mode(d);
        build_fields();
        mode_changed_popup();
        return;
    case F_NUM: case F_HEX: {
        int64_t v = (int64_t)fget(f) + d;
        if(v < (int64_t)f->min) { v = f->max; }
        if(v > (int64_t)f->max) { v = f->min; }
        fset(f, (uint32_t)v);
        break;
    }
    case F_CHOICE: fset(f, fget(f) ? 0 : 1); break;
    case F_CTCSS: {
        int n = ctcss_count() + 1;
        int i = (ctcss_idx(*(uint16_t *)f->ptr) + d + n) % n;
        *(uint16_t *)f->ptr = i ? ctcss_at(i - 1) : 0;
        break;
    }
    case F_CALL: {
        char *s = f->ptr;
        call_fix(s);
        const char *pos = strchr(k_callset, s[s_cur]);
        int i = pos ? (int)(pos - k_callset) : 0, n = (int)sizeof k_callset - 1;
        s[s_cur] = k_callset[(i + d + n) % n];
        break;
    }
    }
    field_changed(f);
}

static void field_digit(field_t *f, int dgt)
{
    switch(f->t) {
    case F_NUM: case F_HEX: {
        uint32_t base = f->t == F_HEX ? 16u : 10u;
        uint64_t v = s_fresh ? (uint32_t)dgt : (uint64_t)fget(f) * base + (uint32_t)dgt;
        if(v > f->max) { v = (uint32_t)dgt; }
        if(v < f->min) { v = f->min; }
        s_fresh = false;
        fset(f, (uint32_t)v);
        break;
    }
    case F_CALL: {
        char *s = f->ptr;
        call_fix(s);
        s[s_cur] = (char)('0' + dgt);
        if(s_cur < 7) { s_cur++; }
        break;
    }
    default: return;
    }
    field_changed(f);
}

static void field_back(field_t *f)
{
    if(f->t == F_NUM || f->t == F_HEX) {
        uint32_t v = fget(f) / (f->t == F_HEX ? 16u : 10u);
        fset(f, v < f->min ? f->min : v);
        field_changed(f);
    } else if(f->t == F_CALL && s_cur > 0) {
        s_cur--;
    }
}

/* ------------------------------------------------------------------ drawing */
/* left_end = end of the status bar text left of the battery: with Percent the volts give way to it */
static void draw_battery(int x, int y, int left_end)
{
    uint16_t cv = battery_centivolts();
    bool low = cv < BATTERY_LOW_CV;
    int lvl = cv <= 600 ? 0 : cv >= 820 ? 4 : (cv - 600) * 4 / 220 + 1;   /* 7.4 V pack: 6.0 .. 8.2 V */
    if(lvl > 4) { lvl = 4; }
    uint16_t col = low ? C_RED : (lvl <= 1 ? C_ORANGE : C_GREEN);
    char t[8], *p = t;
    if(rset_batt_percent()) {                  /* same colour steps as the icon, from the shown (steady) percentage */
        int pc = battdisp_percent();
        col = pc <= 0 ? C_RED : pc * 10 < (int)battdisp_curve_x10(655u * BATT_Q) ? C_ORANGE : C_GREEN;
        if(pc < 0) { p = fmt_s(p, "--"); col = C_GREY; } else { p = fmt_u(p, (uint32_t)pc, 1); }
        fmt_s(p, "%");
        gfx_text(LCD_W - 1 - gfx_text_width(t, 1), y, t, col, C_BLUE, 1);
        cv = battdisp_centivolts();
        low = pc == 0;
        if(left_end + 2 > x - 6 * 4 - 2) { return; }        /* 2 px gap */
    } else {
        gfx_frame(x, y, 15, 8, C_WHITE);
        gfx_rect(x + 15, y + 2, 2, 4, C_WHITE);
        for(int i = 0; i < 4; i++) { gfx_rect(x + 2 + i * 3, y + 2, 2, 4, i < lvl ? col : C_DGREY); }
    }
    p = t;
    p = fmt_u(p, cv / 100u, 1); *p++ = '.'; fmt_u(p, (cv % 100u) / 10u, 1);
    gfx_text(x - 6 * 4 - 2, y, t, low ? C_RED : C_WHITE, C_BLUE, 1);
}

#define header ui_header
#define softkeys ui_softkeys
void ui_header(const char *title)
{
    gfx_rect(0, 0, LCD_W, 11, C_BLUE);
    gfx_text(2, 2, title, C_YELLOW, C_BLUE, 1);
}

void ui_softkeys(const char *left, const char *right)
{
    gfx_rect(0, LCD_H - 12, LCD_W, 12, C_DGREY);
    if(left) { gfx_text(3, LCD_H - 10, left, C_WHITE, C_DGREY, 1); }
    if(right) { gfx_text(LCD_W - 3 - gfx_text_width(right, 1), LCD_H - 10, right, C_WHITE, C_DGREY, 1); }
}

static void draw_status_bar(void)
{
    const channel_t *c = radio_current();
    gfx_rect(0, 0, LCD_W, 11, C_BLUE);
    /* mode badge: coloured by what the mode can do right now */
    const char *mn = modem_mode_name(c->mode);
    int w = gfx_text_width(mn, 1) + 4;
    uint16_t bc = avail_col(mode_avail(c->mode));
    gfx_rect(0, 0, w, 11, bc);
    gfx_text(2, 2, mn, C_BLACK, bc, 1);
    int x = w + 3;
    if(s_locked) { x = gfx_text(x, 2, "LOCK", C_ORANGE, C_BLUE, 1) + 3; }
    if(scan_active()) { x = gfx_text(x, 2, scan_holding() ? "SCN" : "SCAN", C_YELLOW, C_BLUE, 1) + 3; }
    else if(g_radio.monitor) { x = gfx_text(x, 2, "MON", C_GREEN, C_BLUE, 1) + 3; }
    if(sms_unread()) { gfx_rect(x, 3, 9, 6, C_WHITE); gfx_frame(x, 3, 9, 6, C_BLUE); x += 12; }
    if(!s_locked && !scan_active() && !g_radio.monitor && !bb_ready() && x < LCD_W - 70) { x = gfx_text(x, 2, "BB", C_GREY, C_BLUE, 1); }
    if(rset_get(RS_TIMEDISP) && rtc_now() && LCD_W >= 160) {
        char t[8];
        ui_fmt_time(t, rtc_now(), false);
        gfx_text(LCD_W - 19 - 26 - 33, 2, t, C_WHITE, C_BLUE, 1);
    }
    draw_battery(LCD_W - 19, 2, x - 3);
}

static void draw_digital(const channel_t *c)
{
    char t[32], *p;
    if(g_radio.tx) {
        gfx_text(4, 81, "TX TEST: DUMMY LOAD ONLY", C_RED, C_BLACK, 1);
        {
            cfw_vocoder_t vv = c->mode == CFW_MODE_P25P1 ? CFW_VOC_IMBE_88 : c->mode == CFW_MODE_DSTAR ? CFW_VOC_AMBE_DSTAR : CFW_VOC_AMBE2_49;
            p = fmt_s(fmt_s(t, "voc "), voice_codec_name_for(vv));
            fmt_s(p, voice_codec_interoperable_for(vv) ? " (unverif)" : ": not interop");
            gfx_text(4, 90, t, C_ORANGE, C_BLACK, 1);
        }
        p = fmt_s(t, "audio: ");
        if(voice_test_tone()) { fmt_s(fmt_u(p, voice_test_tone(), 1), " Hz tone"); } else { fmt_s(p, voice_tx_source()); }
        gfx_text(4, 99, t, C_WHITE, C_BLACK, 1);
        p = fmt_s(t, CFW_TXT_DAC); fmt_s(p, dspb_tx_dac_moving() ? "DMA" : "timed (no DAC)");
        gfx_text(4, 108, t, C_GREY, C_BLACK, 1);
        return;
    }
    avail_t a = mode_avail(c->mode);
    if(a == AV_NO_PROTO) {
        gfx_text(4, 81, "no decoder linked", C_RED, C_BLACK, 1);
        gfx_text(4, 90, "RF stays in FM receive", C_GREY, C_BLACK, 1);
        return;
    }
    if(a == AV_NO_DEMOD) {
        gfx_text(4, 81, "decoder ready, no demod", C_ORANGE, C_BLACK, 1);
        gfx_text(4, 90, "no 4FSK/GMSK RX path yet", C_GREY, C_BLACK, 1);
        gfx_text(4, 99, "emu: make EMU=1 + syms", C_GREY, C_BLACK, 1);
        return;
    }
    char s[96];
    modem_status(s, sizeof s);
    const cfw_call_info_t *ci = modem_call_info();
    /* stock Talk / AIR Alias display priority (app/stockdlg.c): off, contact first, alias first */
    uint8_t ap = stockdlg_alias_prio();
    char cnm[28];
    bool contact = (ci->src_id || ci->dst_id) && (ap == TA_PRIO_CONTACT || !ci->alias[0] || ap == TA_PRIO_OFF) &&
                   stockcp_call_line((uint8_t)c->mode, ci->src_id, ci->dst_id, ci->private_call, cnm, sizeof cnm);
    if(ci->alias[0] && ap != TA_PRIO_OFF && !contact) {  /* DMR talker alias: replaces the third status line */
        char *nl = strchr(s, '\n');
        if(nl) { nl = strchr(nl + 1, '\n'); }
        if(!nl) { nl = s + strlen(s); }
        char a[40];
        fmt_s(fmt_s(a, "\nTA "), ci->alias);
        a[30] = 0;
        if((size_t)(nl - s) + strlen(a) < sizeof s) { strcpy(nl, a); }
    } else if(contact) {                                 /* name from the user database / contacts (stockcp.c) */
        char a[40];
        {
            char *nl = strchr(s, '\n');
            if(nl) { nl = strchr(nl + 1, '\n'); }
            if(!nl) { nl = s + strlen(s); }
            fmt_s(fmt_s(a, "\n"), cnm);
            a[30] = 0;
            if((size_t)(nl - s) + strlen(a) < sizeof s) { strcpy(nl, a); }
        }
    }
    int y = 81;
    char *line = s;
    for(int i = 0; i < 3 && line && *line; i++) {
        char *nl = strchr(line, '\n');
        if(nl) { *nl = 0; }
        gfx_text(4, y, line, i == 0 ? (modem_call_info()->voice ? C_YELLOW : C_GREEN) : C_WHITE, C_BLACK, 1);
        y += 9;
        line = nl ? nl + 1 : NULL;
    }
    const voice_meter_t *vm = voice_meter();
    if(vm->valid) {                                  /* recovered audio: tone meter */
        p = fmt_s(t, "AF ");
        p = fmt_u(p, (uint32_t)vm->freq, 1); *p++ = '.'; p = fmt_u(p, (uint32_t)(vm->freq * 10.0f) % 10u, 1);
        p = fmt_s(p, "Hz ");
        p = fmt_u(p, vm->sinad_db > 0 ? (uint32_t)vm->sinad_db : 0u, 1); fmt_s(p, "dB SINAD");
        gfx_text(4, 108, t, C_YELLOW, C_BLACK, 1);
        return;
    }
    p = fmt_s(t, modem_backend_name());
    if(dspb_active()) {
        p = fmt_s(p, dspb_locked() ? " LOCK q" : " q"); p = fmt_u(p, (uint32_t)dspb_quality(), 1);
        p = fmt_s(p, " "); p = fmt_u(p, dspb_load_x10() / 10u, 1); *p++ = '.'; p = fmt_u(p, dspb_load_x10() % 10u, 1); p = fmt_s(p, "%");
    } else {
        p = fmt_s(p, " "); p = fmt_u(p, modem_rx_symbol_count(), 1);
    }
    if(modem_rx_hz() && modem_rx_hz() != c->rx_hz) { p = fmt_s(p, " @"); fmt_freq(p, modem_rx_hz()); }
    gfx_text(4, 108, t, C_GREY, C_BLACK, 1);
}

static void draw_main(void)
{
    const channel_t *c = radio_current();
    char t[32], *p;
    gfx_clear(C_BLACK);
    draw_status_bar();

    /* channel line */
    if(g_radio.vfo) {
        gfx_text(4, 15, "VFO", C_CYAN, C_BLACK, 1);
        p = fmt_s(t, "step "); p = fmt_khz(p, g_radio.step_hz); fmt_s(p, "k");
        gfx_text(LCD_W - 4 - gfx_text_width(t, 1), 15, t, C_GREY, C_BLACK, 1);
    } else if(s_entry_n) {
        p = fmt_s(t, "CH ");
        for(int i = 0; i < s_entry_n; i++) { *p++ = s_entry[i]; }
        fmt_s(p, "_  #=go");
        gfx_text(4, 15, t, C_YELLOW, C_BLACK, 1);
    } else {
        p = fmt_u(t, (uint32_t)g_radio.channel + 1, 3); *p++ = ' '; fmt_s(p, c->name);
        gfx_text(4, 15, t, stockfx_color(0), C_BLACK, 1);
        if(c->src == CH_SRC_CODEPLUG) { gfx_text(LCD_W - 10, 15, "C", C_GREY, C_BLACK, 1); }
    }

    /* frequency, big */
    if(s_entry_n && g_radio.vfo) {
        memset(t, 0, sizeof t);
        int j = 0;
        for(int i = 0; i < 8; i++) {
            if(i == 3) { t[j++] = '.'; }
            t[j++] = i < s_entry_n ? s_entry[i] : '-';
        }
        gfx_text((LCD_W - gfx_text_width(t, 2)) / 2, 28, t, C_YELLOW, C_BLACK, 2);
    } else {
        fmt_freq(t, g_radio.tx ? radio_tx_hz(c) : c->rx_hz);
        gfx_text((LCD_W - gfx_text_width(t, 2)) / 2, 28, t, g_radio.tx ? C_RED : stockfx_color(1), C_BLACK, 2);
    }

    /* RX/TX + channel details */
    if(g_radio.tx) {
        gfx_rect(4, 50, 18, 10, C_RED);
        gfx_text(7, 51, "TX", C_WHITE, C_RED, 1);
    } else {
        uint16_t bg = g_radio.carrier ? C_GREEN : C_GREY;      /* green = carrier (reg 0x0E bit 14) */
        gfx_rect(4, 50, 18, 10, bg);
        gfx_text(7, 51, "RX", C_BLACK, bg, 1);
        int dbm = rf_rssi_dbm(g_radio.rssi);
        char r[12], *q = r;
        if(dbm < 0) { *q++ = '-'; dbm = -dbm; }
        q = fmt_u(q, (uint32_t)dbm, 1);
        fmt_s(q, "dBm");
#if LCD_W < 160
        gfx_text(LCD_W - gfx_text_width(r, 1) - 4, LCD_H - 24, r, g_radio.carrier ? C_GREEN : C_GREY, C_BLACK, 1);   /* portrait: own line */
#else
        gfx_text(LCD_W - gfx_text_width(r, 1) - 4, 62, r, g_radio.carrier ? C_GREEN : C_GREY, C_BLACK, 1);
#endif
    }
    p = t;
    switch(c->mode) {
    case CFW_MODE_FM:
        p = fmt_s(p, c->wide ? "FM 25k " : "FM 12.5k ");
        if(c->ctcss_dhz) { p = fmt_s(p, "T"); p = fmt_u(p, c->ctcss_dhz / 10u, 1); *p++ = '.'; p = fmt_u(p, c->ctcss_dhz % 10u, 1); }
        break;
    case CFW_MODE_DMR:
        p = fmt_s(p, "CC"); p = fmt_u(p, c->color_code, 1); p = fmt_s(p, " TS"); p = fmt_u(p, c->slot, 1);
        if(c->trunk) { p = fmt_s(p, " T3"); }
        else if(c->talkgroup) { p = fmt_s(p, c->private_call ? " P" : " TG"); p = fmt_u(p, c->talkgroup, 1); }
        break;
    case CFW_MODE_P25P1: case CFW_MODE_P25P2:
        p = fmt_s(p, "NAC"); p = fmt_hex(p, c->nac, 3);
        if(c->trunk) { p = fmt_s(p, " TRUNK"); }
        else if(c->talkgroup) { p = fmt_s(p, " TG"); p = fmt_u(p, c->talkgroup, 1); }
        if(c->mode == CFW_MODE_P25P2) { p = fmt_s(p, " S"); p = fmt_u(p, c->slot, 1); }
        break;
    case CFW_MODE_NXDN:
        p = fmt_s(p, "RAN"); p = fmt_u(p, c->ran, 1);
        if(c->talkgroup) { p = fmt_s(p, " G"); p = fmt_u(p, c->talkgroup, 1); }
        break;
    case CFW_MODE_DSTAR:
        p = fmt_s(p, "UR "); memcpy(p, c->ur[0] ? c->ur : "CQCQCQ  ", 8); p += 8; *p = 0;
        break;
    case CFW_MODE_YSF:
        p = fmt_s(p, "DG-ID "); p = fmt_u(p, c->dgid, 1);
        break;
    default: break;
    }
    if(c->tx_hz == 0) { fmt_s(p, " RX"); }
    gfx_text(26, 51, t, C_WHITE, C_BLACK, 1);

    /* offset line */
    if(c->tx_hz && c->tx_hz != c->rx_hz && c->talkaround) {
        gfx_text(26, 62, "TALKAROUND", C_ORANGE, C_BLACK, 1);
    } else if(c->tx_hz && c->tx_hz == c->rx_hz) {
        gfx_text(26, 62, "SIMPLEX", C_GREY, C_BLACK, 1);
    } else if(c->tx_hz && c->tx_hz != c->rx_hz) {
        int32_t d = (int32_t)(c->tx_hz - c->rx_hz);
        p = fmt_s(t, d < 0 ? "Shift -" : "Shift +");
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        p = fmt_u(p, ad / 1000000u, 1); *p++ = '.'; fmt_u(p, (ad % 1000000u) / 1000u, 3);
        gfx_text(26, 62, t, C_GREY, C_BLACK, 1);
    }

    /* mode status area */
    gfx_rect(0, 75, LCD_W, 1, C_DGREY);
    if(c->mode == CFW_MODE_FM) {
        gfx_text(4, 81, "Analog FM", C_GREEN, C_BLACK, 1);
        const voice_meter_t *vm = voice_meter();
        if(g_radio.tx && voice_test_tone()) {
            char t2[28];
            fmt_s(fmt_u(fmt_s(t2, "TX test tone "), voice_test_tone(), 1), " Hz");
            gfx_text(4, 90, t2, C_ORANGE, C_BLACK, 1);
            gfx_text(4, 99, "(DAC test path, dummy load)", C_GREY, C_BLACK, 1);
        } else if(vm->valid) {
            char t2[28], *q = fmt_s(t2, "AF ");
            q = fmt_u(q, (uint32_t)vm->freq, 1); *q++ = '.'; q = fmt_u(q, (uint32_t)(vm->freq * 10.0f) % 10u, 1);
            q = fmt_s(q, "Hz ");
            q = fmt_u(q, vm->sinad_db > 0 ? (uint32_t)vm->sinad_db : 0u, 1); fmt_s(q, "dB SINAD");
            gfx_text(4, 108, t2, C_YELLOW, C_BLACK, 1);
            gfx_text(4, 90, CFW_TXT_MODEKEY, C_GREY, C_BLACK, 1);
        } else {
            gfx_text(4, 90, CFW_TXT_MODEKEY, C_GREY, C_BLACK, 1);
        }
    } else {
        draw_digital(c);
    }
    softkeys("MENU", g_radio.vfo ? CFW_TXT_SOFT_MEM : CFW_TXT_SOFT_VFO);
}

static void draw_popup(void)
{
    if((int32_t)(hal_millis() - s_popup_until) >= 0) { return; }
    gfx_rect(8, 38, LCD_W - 16, 44, C_BLACK);
    gfx_frame(8, 38, LCD_W - 16, 44, s_popup_col);
    gfx_frame(9, 39, LCD_W - 18, 42, s_popup_col);
#if LCD_W < 160
    /* narrow (portrait) panel: shrink the title, split a long second line at a space */
    int sc = gfx_text_width(s_popup[0], 2) <= LCD_W - 20 ? 2 : 1;
    gfx_text_center(sc == 2 ? 44 : 48, s_popup[0], C_WHITE, C_BLACK, sc);
    if(gfx_text_width(s_popup[1], 1) > LCD_W - 20) {
        char a[28], *sp = NULL;
        strcpy(a, s_popup[1]);
        for(char *q = a; *q && gfx_text_width(a, 1) > 0; q++) { if(*q == ' ' && (q - a) * 6 <= LCD_W - 20) { sp = q; } }
        if(sp) { *sp = 0; gfx_text_center(62, a, s_popup_col, C_BLACK, 1); gfx_text_center(71, sp + 1, s_popup_col, C_BLACK, 1); return; }
    }
    gfx_text_center(66, s_popup[1], s_popup_col, C_BLACK, 1);
#else
    gfx_text_center(44, s_popup[0], C_WHITE, C_BLACK, 2);
    gfx_text_center(66, s_popup[1], s_popup_col, C_BLACK, 1);
#endif
}

static void draw_mode_picker(void)
{
    gfx_clear(C_BLACK);
    header("MODE");
    cfw_mode_t cur = radio_current()->mode;
    for(int i = 0; i < CFW_MODE_COUNT; i++) {
        bool sel = i == s_mode_sel;
        avail_t a = mode_avail((cfw_mode_t)i);
        uint16_t bg = sel ? C_CYAN : C_BLACK, fg = sel ? C_BLACK : C_WHITE;
        gfx_rect(0, 13 + i * 14, LCD_W, 14, bg);
        gfx_text(4, 16 + i * 14, (cfw_mode_t)i == cur ? ">" : " ", fg, bg, 1);
        gfx_text(12, 16 + i * 14, modem_mode_name((cfw_mode_t)i), fg, bg, 1);
        const char *st = a == AV_FM ? "analog" : a == AV_RUN ? (mcu_dsp((cfw_mode_t)i) ? "demod*" : "decode")
                       : a == AV_NO_DEMOD ? "no demod" : "no proto";
        gfx_text(LCD_W - 4 - gfx_text_width(st, 1), 16 + i * 14, st, sel ? C_BLACK : avail_col(a), bg, 1);
    }
    softkeys("SET", "BACK");
}

static void draw_edit(void)
{
    char t[28];
    build_fields();
    gfx_clear(C_BLACK);
    char h[24];
    fmt_s(fmt_s(h, modem_mode_name(radio_current()->mode)), g_radio.vfo ? " VFO" : " CHANNEL");
    header(h);
    for(int i = 0; i < s_nf; i++) {
        bool sel = i == s_fsel;
        uint16_t bg = sel ? C_CYAN : C_BLACK, fg = sel ? C_BLACK : C_WHITE;
        gfx_rect(0, 13 + i * 10, LCD_W, 10, bg);
        gfx_text(4, 14 + i * 10, s_f[i].label, fg, bg, 1);
        field_text(&s_f[i], t);
        int x = LCD_W - 4 - gfx_text_width(t, 1);
        gfx_text(x, 14 + i * 10, t, s_f[i].global && !sel ? C_GREY : fg, bg, 1);
        if(sel && s_f[i].t == F_CALL) { gfx_rect(x + s_cur * 6, 22 + i * 10, 5, 1, C_RED); }
    }
    const char *hint = s_f[s_fsel].t == F_CALL ? "knob=char #/*=move" : "knob/0-9 edit *=del";
    gfx_text(4, LCD_H - 22, hint, C_GREY, C_BLACK, 1);
    softkeys("NEXT", "BACK");
}

static void draw_info(void)
{
    char t[28], *p;
    gfx_clear(C_BLACK);
    header("INFO");
    int y = 14;
    gfx_text(4, y, CFW_BOARD_NAME " cfw " VERSION, C_WHITE, C_BLACK, 1); y += 9;
    p = fmt_s(t, "NAND "); p = fmt_hex(p, nand_id(), 4); p = fmt_s(p, " CP ch "); fmt_u(p, (uint32_t)channels_codeplug_count(), 1);
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    p = fmt_s(t, "RF0 "); p = fmt_s(p, g_radio.rf_ok[0] ? "ok" : "FAIL");
    if(CFW_RF_CHIPS > 1) { p = fmt_s(p, "  RF1 "); fmt_s(p, g_radio.rf_ok[1] ? "ok" : "FAIL"); }
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    p = fmt_s(t, "BB "); p = fmt_s(p, bb_status());
    if(bb_ready()) { p = fmt_s(p, " "); const uint8_t *v = bb_version(); for(int i = 3; i < 8; i++) { p = fmt_hex(p, v[i], 2); } }
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    uint16_t bv = battery_centivolts();
    p = fmt_s(t, "Bat "); p = fmt_u(p, bv / 100u, 1); *p++ = '.'; p = fmt_u(p, bv % 100u, 2); p = fmt_s(p, "V ");
    if(battdisp_percent() >= 0) { p = fmt_u(p, (uint32_t)battdisp_percent(), 1); p = fmt_s(p, "%"); }
    p = fmt_s(p, " T"); fmt_u(p, battery_raw(CFW_ADC_TEMP_SLOT), 1);
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    if(g_cal_info.copy) {                      /* RF calibration copy in use + CRC (docs/CALIBRATION.md section 3) */
        p = fmt_s(t, "Cal "); *p++ = g_cal_info.copy; *p++ = ' '; p = fmt_hex(p, g_cal_info.crc, 8);
        p = fmt_s(p, " U"); p = fmt_u(p, g_cal_info.uhf, 1); p = fmt_s(p, " V"); fmt_u(p, g_cal_info.vhf, 1);
        gfx_text(4, y, t, C_WHITE, C_BLACK, 1);
    } else {
        gfx_text(4, y, "Cal none: TX off", C_RED, C_BLACK, 1);
    }
    y += 9;
    t[0] = 0; p = t;
    for(int i = 0; i < modem_proto_count(); i++) {
        if(gfx_text_width(t, 1) > LCD_W - 50) { gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 9; p = t; *p = 0; }
        p = fmt_s(p, modem_proto_at(i)->name); *p++ = ' '; *p = 0;
    }
    gfx_text(4, y, t, C_GREY, C_BLACK, 1); y += 9;
    p = fmt_s(t, "Demod "); p = fmt_s(p, modem_backend_for(CFW_MODE_DMR) ? modem_backend_for(CFW_MODE_DMR) : "none");
    p = fmt_s(p, " CPU "); p = fmt_u(p, dspb_load_x10() / 10u, 1); *p++ = '.'; p = fmt_u(p, dspb_load_x10() % 10u, 1); fmt_s(p, "%");
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    gfx_text(4, y, "ch1 48k: unverified on HW", C_GREY, C_BLACK, 1); y += 9;
    p = fmt_s(t, "ID "); p = fmt_u(p, g_radio.my_id, 1); p = fmt_s(p, " "); fmt_s(p, g_radio.my_call);
    gfx_text(4, y, t, C_WHITE, C_BLACK, 1); y += 9;
    softkeys(NULL, "BACK");
}

static char s_fault[2][20];
static bool s_fault_shown;

static void draw_boot(void)
{
    char t[28], *p;
    gfx_clear(C_BLACK);
    gfx_text_center(8, CFW_BOARD_NAME, C_CYAN, C_BLACK, 2);
    gfx_text_center(28, "custom firmware " VERSION, C_WHITE, C_BLACK, 1);
    gfx_text_center(37, CFW_BUILD, C_GREY, C_BLACK, 1);
    int y = 46;
    p = fmt_s(t, "NAND "); p = fmt_hex(p, nand_id(), 4); fmt_s(p, g_boot.nand_ok ? " ok" : " ?");
    gfx_text(10, y, t, g_boot.nand_ok ? C_GREEN : C_ORANGE, C_BLACK, 1); y += 10;
    p = fmt_s(t, "Codeplug ch "); fmt_u(p, (uint32_t)channels_codeplug_count(), 1);
    gfx_text(10, y, t, channels_codeplug_count() ? C_GREEN : C_GREY, C_BLACK, 1); y += 10;
    p = fmt_s(t, "RF0 "); p = fmt_s(p, g_radio.rf_ok[0] ? "ok" : "fail");
    if(CFW_RF_CHIPS > 1) { p = fmt_s(p, "  RF1 "); fmt_s(p, g_radio.rf_ok[1] ? "ok" : "fail"); }
    gfx_text(10, y, t, g_radio.rf_ok[0] && (CFW_RF_CHIPS < 2 || g_radio.rf_ok[1]) ? C_GREEN : C_ORANGE, C_BLACK, 1); y += 10;
    p = fmt_s(t, "Baseband "); fmt_s(p, bb_status());
    gfx_text(10, y, t, bb_ready() ? C_GREEN : C_GREY, C_BLACK, 1); y += 10;
    p = fmt_s(t, "Decoders "); fmt_u(p, (uint32_t)modem_proto_count(), 1);
    gfx_text(10, y, t, C_WHITE, C_BLACK, 1); y += 10;
    if(g_clk_err) { gfx_text(10, y, "CLK ERR: internal clock, no TX", C_RED, C_BLACK, 1); y += 10; }
    if(s_fault[0][0]) { gfx_text(10, y, s_fault[0], C_RED, C_BLACK, 1); y += 10; }
    char l1[17], l2[17];                       /* stock Power-on Interface = Character: the CPS text lines */
    if(stockopt_boot_text(l1, l2)) {
        if(l1[0]) { gfx_text_center(LCD_H - 22, l1, C_YELLOW, C_BLACK, 1); }
        if(l2[0]) { gfx_text_center(LCD_H - 12, l2, C_YELLOW, C_BLACK, 1); }
    }
}

/* after a fault reset (hal/safety.c): shown on the boot screen and once as a popup on the main screen */
void ui_boot_fault(const char *l1, const char *l2)
{
    strncpy(s_fault[0], l1, sizeof s_fault[0] - 1);
    strncpy(s_fault[1], l2, sizeof s_fault[1] - 1);
}

/* ------------------------------------------------------------------ screen switches for ui_menu.c */
void ui_show_main(void) { s_screen = SCR_MAIN; }
void ui_show_mode_picker(void) { s_mode_sel = radio_current()->mode; s_screen = SCR_MODE; }
void ui_show_chan_edit(void) { s_fsel = 0; s_cur = 0; s_fresh = true; s_screen = SCR_EDIT; }
void ui_show_info(void) { s_screen = SCR_INFO; }

char *ui_fmt_time(char *p, uint32_t t, bool date)
{
    if(date) {
        int y, m, d;
        cal_date(t / 86400u, &y, &m, &d);
        p = fmt_u(p, (uint32_t)m, 2); *p++ = '-'; p = fmt_u(p, (uint32_t)d, 2); *p++ = ' ';
    }
    uint32_t sday = t % 86400u;
    p = fmt_u(p, sday / 3600u, 2); *p++ = ':';
    return fmt_u(p, sday / 60u % 60u, 2);
}

/* ------------------------------------------------------------------ keys */
void ui_init(void)
{
    s_screen = SCR_BOOT;
    s_last_key = hal_millis();
    s_boot_until = hal_millis() + 1500;
}

static void entry_commit(void)
{
    if(g_radio.vfo) {
        uint32_t hz = 0;
        for(int i = 0; i < 8; i++) { hz = hz * 10 + (uint32_t)(i < s_entry_n ? s_entry[i] - '0' : 0); }
        hz *= 10;   /* 8 digits = 100 Hz resolution: 14652000 -> 146.520 MHz */
        s_entry_n = 0;
        if(freq_in_band(hz)) { radio_set_vfo_freq(hz); }
        else { popup("OUT OF BAND", "136-174 / 400-480", C_RED); }
        return;
    }
    uint32_t ch = 0;
    for(int i = 0; i < s_entry_n; i++) { ch = ch * 10 + (uint32_t)(s_entry[i] - '0'); }
    s_entry_n = 0;
    if(ch >= 1 && ch <= (uint32_t)channels_count()) { radio_goto((int)ch - 1); }
    else { popup("NO CHANNEL", "", C_RED); }
}

/* ------------------------------------------------------------------ key functions (Settings > Key ...) */
void ui_keyfn(uint8_t fn)
{
    switch(fn) {
    case KF_MODE_NEXT: s_entry_n = 0; set_mode((cfw_mode_t)((radio_current()->mode + 1) % CFW_MODE_COUNT)); break;
    case KF_MODE_PICKER: ui_show_mode_picker(); break;
    case KF_VFO_MEM: s_entry_n = 0; scan_stop(true); radio_toggle_vfo(); popup(g_radio.vfo ? "VFO" : "MEMORY", "", C_CYAN); break;
    case KF_KEY_LOCK: s_locked = !s_locked; popup(s_locked ? "LOCKED" : "UNLOCKED", "", C_ORANGE); break;
    case KF_MONITOR: g_radio.monitor = !g_radio.monitor; popup("MONITOR", g_radio.monitor ? "squelch open" : "off", C_CYAN); break;
    case KF_SCAN:
        if(scan_active()) { scan_stop(true); popup("SCAN OFF", "", C_CYAN); break; }
        {
            uint8_t l = radio_current()->scanlist;
            int list = l < cfwcp_scanlist_count() ? l : -1;
            scan_start(list);
            popup(scan_active() ? "SCAN" : "NO CHANNELS", scan_name(list), C_CYAN);
        }
        break;
    case KF_TEST_TONE: voice_set_test_tone(voice_test_tone() ? 0 : 440); popup("TEST TONE", voice_test_tone() ? "440 Hz" : "off", C_CYAN); break;
    case KF_ZONE_NEXT: {
        int n = zone_count();
        if(!n) { popup("NO ZONES", "", C_ORANGE); break; }
        radio_set_zone(g_radio.zone + 1 >= n ? -1 : g_radio.zone + 1);
        popup("ZONE", zone_name(g_radio.zone), C_CYAN);
        break;
    }
    case KF_TALKAROUND: {
        const channel_t *c = radio_current();
        if(!c->tx_hz || c->tx_hz == c->rx_hz) { popup("SIMPLEX", "no repeater offset", C_GREY); break; }
        radio_toggle_talkaround();
        popup(radio_current()->talkaround ? "TALKAROUND" : "REPEATER", radio_current()->talkaround ? "TX on RX freq" : "TX offset on", C_ORANGE);
        break;
    }
    case KF_TX_POWER: {
        static const char *const pw[] = { "Low", "Mid", "High", "Turbo" };
        radio_set_power((uint8_t)((radio_tx_power() + 1) & 3));
        popup("TX POWER", pw[radio_tx_power()], C_CYAN);
        break;
    }
    case KF_CALL_LOG: um_open(UM_CALLLOG); s_screen = SCR_MENU; break;
    case KF_ALARM: emerg_start("ALARM KEY", true); break;
    case KF_GPS_INFO: um_open(UM_GPS); s_screen = SCR_MENU; break;
    default: break;
    }
}

static int key_slot(int code, bool lng)
{
    switch(code) {
    case KEY_P1:  return lng ? -1 : RK_P1;
    case KEY_P2:  return lng ? -1 : RK_P2;
    case KEY_PF1: return lng ? -1 : RK_PF1;
    case KEY_PF2: return lng ? -1 : RK_PF2;
    case KEY_PF3: return lng ? RK_PF3L : RK_PF3;
    default:      return -1;
    }
}

static void main_key(int k)
{
    int code = k & 0x3F;
    bool rel = k & KEY_RELEASED, lng = k & KEY_LONG;

    if(rel) { return; }
    if(code == KEY_STAR && lng) { s_locked = !s_locked; popup(s_locked ? "LOCKED" : "UNLOCKED", "long * toggles", C_ORANGE); return; }
    if(code == KEY_HASH && lng && !s_locked) { ui_keyfn(KF_TALKAROUND); return; }
    if(s_locked) { return; }
    int slot = key_slot(code, lng);
    if(slot >= 0) { ui_keyfn(rset_key(slot)); return; }
    if(lng && code >= KEY_0 && code <= KEY_9) {             /* stock Hot Key (app/stockrun.c) */
        const char *l1, *l2;
        int um = stockrun_hotkey(code - KEY_0, &l1, &l2);
        if(um != -2) {
            s_entry_n = 0;                                    /* the press already typed the digit */
            if(um >= 0) { s_screen = SCR_MENU; um_open(um); }
            else if(l1[0]) { popup(l1, l2, C_CYAN); }
            return;
        }
    }
    if(lng) {
        if(code == KEY_P1) { ui_show_mode_picker(); }      /* P1 (D168UV: PF1) long: mode list */
        return;
    }
    if(scan_active()) {                                    /* scanning: Exit stops, * deletes a nuisance channel */
        if(code == KEY_STAR) { scan_nuisance(); popup("DELETED", "from this scan", C_ORANGE); return; }
        if(code == KEY_MENU) { s_screen = SCR_MENU; um_open(UM_SCAN); return; }
        scan_stop(true);
        if(code == KEY_EXIT) { popup("SCAN OFF", "", C_CYAN); return; }
    }

    if(code >= KEY_0 && code <= KEY_9) {
        int max = g_radio.vfo ? 8 : 4;
        if(s_entry_n < max) { s_entry[s_entry_n++] = (char)('0' + code - KEY_0); s_entry_t = hal_millis(); }
        if(s_entry_n == 8 && g_radio.vfo) { entry_commit(); }
        return;
    }
    switch(code) {
    case KEY_UP: case KEY_KNOB_CW: s_entry_n = 0; radio_step(+1); break;
    case KEY_DOWN: case KEY_KNOB_CCW: s_entry_n = 0; radio_step(-1); break;
    case KEY_HASH: if(s_entry_n) { entry_commit(); } break;
    case KEY_EXIT: s_entry_n = 0; break;
    case KEY_MENU: s_screen = SCR_MENU; s_menu_sel = 0; um_open(UM_ROOT); break;
    default: break;
    }
}

static void mode_key(int k)
{
    switch(k) {
    case KEY_UP: case KEY_KNOB_CCW: s_mode_sel = (s_mode_sel + CFW_MODE_COUNT - 1) % CFW_MODE_COUNT; break;
    case KEY_DOWN: case KEY_KNOB_CW: s_mode_sel = (s_mode_sel + 1) % CFW_MODE_COUNT; break;
    case KEY_EXIT: ui_show_main(); break;
    case KEY_MENU: case KEY_PF3: case KEY_P1:
        set_mode((cfw_mode_t)s_mode_sel);
        s_screen = SCR_MAIN;
        break;
    default:
        if(k >= KEY_1 && k <= KEY_1 + CFW_MODE_COUNT - 1) { set_mode((cfw_mode_t)(k - KEY_1)); s_screen = SCR_MAIN; }
        break;
    }
}

static void edit_key(int k)
{
    build_fields();
    field_t *f = &s_f[s_fsel];
    switch(k) {
    case KEY_UP: s_fsel = (s_fsel + s_nf - 1) % s_nf; s_cur = 0; s_fresh = true; break;
    case KEY_DOWN: case KEY_MENU: s_fsel = (s_fsel + 1) % s_nf; s_cur = 0; s_fresh = true; break;
    case KEY_KNOB_CW: field_step(f, +1); break;
    case KEY_KNOB_CCW: field_step(f, -1); break;
    case KEY_STAR: field_back(f); break;
    case KEY_HASH: if(f->t == F_CALL && s_cur < 7) { s_cur++; } else if(f->t != F_CALL) { field_step(f, +1); } break;
    case KEY_EXIT: um_open(UM_ROOT); s_screen = SCR_MENU; break;
    case KEY_PF3: case KEY_P1: field_step(&s_f[0], +1); break;
    default:
        if(k >= KEY_0 && k <= KEY_9) { field_digit(f, k - KEY_0); }
        break;
    }
}

void ui_key(int k)
{
    if(prog_active()) { return; }                    /* CPS program mode: keys and PTT ignored */
    if(s_screen == SCR_BOOT) { return; }
    s_last_key = hal_millis();
    radio_activity();
    if(s_bl_off) {                                   /* backlight was off: the first key only wakes it (not PTT) */
        s_bl_off = false;
        backlight_set((uint8_t)(rset_get(RS_BKL) + 1u));
        if((k & 0x3F) != KEY_PTT) { return; }
    }
    if(s_screen == SCR_LOCK) {
        int c = k & 0x3F;
        if(k & (KEY_RELEASED | KEY_LONG)) { return; }
        stockfx_lock_key(c >= KEY_0 && c <= KEY_9 ? c - KEY_0 : -1, c == KEY_MENU || c == KEY_HASH, c == KEY_EXIT || c == KEY_STAR);
        if(!stockfx_locked()) { s_screen = SCR_MAIN; }
        return;
    }
    if((k & 0x3F) == KEY_PTT) {
        radio_ptt(!(k & KEY_RELEASED));
        const channel_t *c = radio_current();
        if(!(k & KEY_RELEASED)) {
            if(c->tx_hz == 0) { popup("RX ONLY", "", C_ORANGE); }
#ifndef CFW_DIGITAL_TX
            else if(c->mode != CFW_MODE_FM) { popup("DIGITAL TX OFF", "HW build: HW_DTX=1", C_ORANGE); }
#else
            else if(c->mode != CFW_MODE_FM && g_radio.tx) { popup("TX TEST", "dummy load only!", C_RED); }
#endif
            if(g_radio.tot_hit) { popup("TX TIMEOUT", "release PTT", C_RED); }
            else if(g_radio.tx && g_radio.enc_tx == 2) { popup("CLEAR TX", "ham band: no ENC", C_ORANGE); }
        }
        return;
    }
    if(mandown_alarm()) {                                     /* man down alarm: any key silences it */
        if(!(k & (KEY_RELEASED | KEY_LONG))) { mandown_cancel(); spk_stop(); popup("MAN DOWN", "silenced", C_ORANGE); }
        return;
    }
    if(!(k & (KEY_RELEASED | KEY_LONG))) {
        bool w = emerg_warning();
        emerg_key();
        if(emerg_active()) { emerg_cancel(); popup("EMERGENCY", "cancelled", C_GREEN); return; }
        if(w && !emerg_warning()) { spk_stop(); popup("WORK ALONE", "response ok", C_GREEN); return; }
    }
    if(g_radio.tx && !(k & (KEY_RELEASED | KEY_LONG)) && radio_current()->mode == CFW_MODE_FM) {
        int c = k & 0x3F;                                     /* FM TX: keypad sends DTMF (app/sig.c) */
        char d = c >= KEY_0 && c <= KEY_9 ? (char)('0' + c - KEY_0) : c == KEY_STAR ? '*' : c == KEY_HASH ? '#' : 0;
        if(d) { sig_dtmf_key(d); return; }
        if(c == KEY_UP && stockfx_tbst()) { popup("TBST", "repeater access tone", C_CYAN); return; }
    }
    if(!(k & (KEY_RELEASED | KEY_LONG))) { spk_key(); }     /* Key beep */
    if(s_screen == SCR_MAIN) { main_key(k); return; }
    if(k & (KEY_RELEASED | KEY_LONG)) { return; }
    switch(s_screen) {
    case SCR_MENU: um_key(k); break;
    case SCR_MODE: mode_key(k); break;
    case SCR_EDIT: edit_key(k); break;
    case SCR_INFO: um_open(UM_ROOT); s_screen = SCR_MENU; break;
    }
}

void ui_draw(void)
{
    if(prog_active()) { prog_draw(); return; }      /* CPS program mode screen */
    if(s_screen == SCR_BOOT && (int32_t)(hal_millis() - s_boot_until) >= 0 && power_armed()) {
        s_screen = stockfx_locked() ? SCR_LOCK : SCR_MAIN;
    }
    uint32_t now = hal_millis();
    if(s_screen != SCR_BOOT) {
        uint32_t bl = rset_backlight_s();
        if(bl && !s_bl_off && !g_radio.tx && now - s_last_key >= bl * 1000u) { s_bl_off = true; backlight_set(0); }
        if(rset_get(RS_KEYLOCK) && !s_locked && s_screen == SCR_MAIN && now - s_last_key >= 10000u) {
            s_locked = true;                         /* Key lock: Auto */
        }
        uint32_t mx = stockopt_menu_exit_s();     /* stock Menu Exit Time */
        if(mx && s_screen != SCR_MAIN && s_screen != SCR_LOCK && !g_radio.tx && now - s_last_key >= mx * 1000u) { s_screen = SCR_MAIN; }
        if(s_screen != SCR_LOCK && stockfx_locked()) { s_screen = SCR_LOCK; }
        if(s_screen == SCR_LOCK && !stockfx_locked()) { s_screen = SCR_MAIN; }   /* CPS wrote a codeplug without it */
        uint32_t apo = rset_apo_s();
        if(apo && !g_radio.tx && now - g_radio.last_activity >= apo * 1000u) { power_off(); }
        if(g_radio.tot_hit && !s_tot_shown) { popup("TX TIMEOUT", "release PTT", C_RED); }
        s_tot_shown = g_radio.tot_hit;
        static uint8_t s_note_seq;
        static uint32_t s_fault_pop_end;
        if(s_fault[0][0] && !s_fault_shown) {               /* fault before the last reset: first popup after boot */
            s_fault_shown = true;
            s_fault_pop_end = now + 1600u;
            popup(s_fault[0], s_fault[1], C_RED);
        } else if(g_radio.tx_note_seq != s_note_seq && (int32_t)(now - s_fault_pop_end) >= 0) {
            s_note_seq = g_radio.tx_note_seq;               /* TX refused / stopped (hal/safety.c) */
            popup(g_radio.tx_note[0], g_radio.tx_note[1], C_RED);
        } else {
            static uint8_t s_sig_seq;                       /* analog signalling decoded (app/sig.c) */
            const char *l1, *l2;
            uint8_t q = sig_note(&l1, &l2);
            if(q != s_sig_seq) { s_sig_seq = q; popup(l1, l2, C_CYAN); }
        }
    }
    if(s_entry_n && hal_millis() - s_entry_t > (g_radio.vfo ? 5000u : 2500u)) {
        if(!g_radio.vfo) { entry_commit(); } else { s_entry_n = 0; }
    }
    switch(s_screen) {
    case SCR_BOOT: draw_boot(); break;
    case SCR_MAIN: draw_main(); break;
    case SCR_MENU: um_draw(); break;
    case SCR_MODE: draw_mode_picker(); break;
    case SCR_EDIT: draw_edit(); break;
    case SCR_INFO: draw_info(); break;
    case SCR_LOCK: {                            /* stock Power-on Password */
        char t[16];
        int n = stockfx_lock_len();
        for(int i = 0; i < n; i++) { t[i] = '*'; }
        t[n] = '_'; t[n + 1] = 0;
        gfx_clear(C_BLACK);
        ui_header("PASSWORD");
        gfx_text_center(LCD_H / 2 - 18, "Power-on password", C_WHITE, C_BLACK, 1);
        gfx_text_center(LCD_H / 2, t, C_YELLOW, C_BLACK, 2);
        ui_softkeys("OK", "DEL");
        break; }
    }
    if(s_screen != SCR_BOOT) { draw_popup(); }
    gfx_flush();
}

bool ui_on_main(void) { return s_screen == SCR_MAIN; }
