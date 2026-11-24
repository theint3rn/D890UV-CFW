/* Stock CPS common sections -> cfw (see stockopt.h). Values are the stock CSV text kept in codeplug section 10. */
#include <string.h>
#include "stockopt.h"
#include "gfx.h"
#include "rset.h"

#if defined(CFW_BOARD_D168UV)
#define CH_COL(d890) ((d890) - 1)            /* D168UV Channel table: one column less before these */
#define HK_COLS_D168 1
#else
#define CH_COL(d890) (d890)
#endif

/* OptionalSetting columns (same on both radios) */
enum { OC_STEP = 8, OC_STARTDSP = 6, OC_MSGRING = 35, OC_CALLRING = 41, OC_TMZONE = 42, OC_TALKTIPS = 43,
       OC_STARTCHAR = 46, OC_BOOTSOUND = 54, OC_STARTCHAR2 = 62, OC_AUTORPT = 65, OC_UHFRPT = 76, OC_VHFRPT = 77,
       OC_ARF0 = 108, OC_ARF1 = 109, OC_ARF2 = 110, OC_ARF3 = 111, OC_STARTCHUSE = 115, OC_STARTZONE1 = 116,
       OC_STARTCH1 = 118, OC_MENUEXIT = 120, OC_TOTPRE = 161 };

static uint32_t dec(const char *t)
{
    uint32_t v = 0;
    while(*t == ' ') { t++; }
    while(*t >= '0' && *t <= '9') { v = v * 10u + (uint32_t)(*t++ - '0'); }
    return v;
}

static int32_t sdec(const char *t)
{
    while(*t == ' ') { t++; }
    if(*t == '-') { return -(int32_t)dec(t + 1); }
    if(*t == '+') { t++; }
    return (int32_t)dec(t);
}

/* "145.390" / "88.1" / "440.00000" -> Hz */
uint32_t stockopt_mhz(const char *t)
{
    while(*t == ' ') { t++; }
    uint32_t mhz = dec(t), frac = 0, scale = 1000000u;
    while(*t >= '0' && *t <= '9') { t++; }
    if(*t == '.') {
        t++;
        for(int k = 0; k < 6 && *t >= '0' && *t <= '9'; k++) { scale /= 10u; frac += (uint32_t)(*t++ - '0') * scale; }
    }
    return mhz * 1000000u + frac;
}

/* "12.34" minutes -> minutes * 1e5 */
static uint32_t minutes_e5(const char *t)
{
    uint32_t m = dec(t), f = 0, s = 100000u;
    while(*t == ' ') { t++; }
    while(*t >= '0' && *t <= '9') { t++; }
    if(*t == '.') { t++; for(int k = 0; k < 5 && *t >= '0' && *t <= '9'; k++) { s /= 10u; f += (uint32_t)(*t++ - '0') * s; } }
    return m * 100000u + f;
}

static bool cell(int t, int c, int r, char *b, int n) { b[0] = 0; return cfwcp_stock_value_row(t, c, r, b, n) && b[0]; }

/* rows of a list table: the CPS stores a row-count entry when the table differs from the stock default */
static int rows(int t, int dflt)
{
    char b[8];
    return cfwcp_stock_value(t, 0xFF, b, sizeof b) ? (int)dec(b) : dflt;
}
static bool stored(int t) { char b[8]; return cfwcp_stock_value(t, 0xFF, b, sizeof b); }

static void copy_trim(char *d, const char *s, int n)
{
    int i = 0;
    for(; i < n - 1 && s[i]; i++) { d[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?'; }
    while(i > 0 && d[i - 1] == ' ') { i--; }
    d[i] = 0;
}

static bool streq_i(const char *a, const char *b)
{
    for(; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if(x != y) { return false; }
    }
    return *a == *b;
}

#define OPT_N 200
static uint16_t s_optv[OPT_N];                                       /* OptionalSetting numbers (last entry wins) */
static uint8_t s_optp[OPT_N / 8];                                    /* stored? */

/* ------------------------------------------------------------------ Optional Settings */
uint32_t stockopt_opt(int col, uint32_t dflt)
{
    uint32_t pv;
    if(rset_opt_pending((uint8_t)col, &pv)) { return pv; }
    return col >= 0 && col < OPT_N && (s_optp[col >> 3] >> (col & 7) & 1u) ? s_optv[col] : dflt;
}
bool stockopt_opt_text(int col, char *buf, int n) { buf[0] = 0; return cfwcp_stock_value(SO_T_OPT, col, buf, n) && buf[0]; }

/* stock step table (10 Hz): V1.05 0x0814A520, indexed by the setting byte (+8 of the settings block, factory default
 * 5 = 12.5K; the D168UV radio menu agrees: value 5 shows 12.5K). */
static const uint16_t k_step_10hz[] = { 250, 500, 625, 833, 1000, 1250, 2000, 2500, 3000, 5000 };
uint32_t stockopt_step_hz(void)
{
    uint32_t v = stockopt_opt(OC_STEP, 0xFFu);
    return v < sizeof k_step_10hz / sizeof k_step_10hz[0] ? k_step_10hz[v] * 10u : 0;
}

/* TmZone: stock string tables (D890UV V1.05 0x0818E2F8: 34 entries; D168UV radio menu: 32, no +09:30 / +10:30) */
static const int16_t k_tz_min[] = { -720, -660, -600, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0,
                                    60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 420, 480, 510, 540,
#if defined(CFW_BOARD_D168UV)
                                    600, 660, 720, 780 };
#else
                                    570, 600, 630, 660, 720, 780 };
#endif
int stockopt_tz_count(void) { return (int)(sizeof k_tz_min / sizeof k_tz_min[0]); }
int stockopt_tz_at(int i) { return i >= 0 && i < stockopt_tz_count() ? k_tz_min[i] : 0; }
int32_t stockopt_tz_min(void)
{
    return cfwcp_valid() ? stockopt_tz_at((int)stockopt_opt(OC_TMZONE, 0)) : 0;   /* absent = stock default 0 = UTC-12 */
}

bool stockopt_boot_text(char *l1, char *l2)
{
    char b[24];
    l1[0] = l2[0] = 0;
    if(stockopt_opt(OC_STARTDSP, 0) != 1u) { return false; }    /* 0 Picture, 1 Character, 2 Customer's picture */
    if(stockopt_opt_text(OC_STARTCHAR, b, sizeof b)) { copy_trim(l1, b, 17); }
    if(stockopt_opt_text(OC_STARTCHAR2, b, sizeof b)) { copy_trim(l2, b, 17); }
    return l1[0] || l2[0];
}
bool stockopt_boot_sound(void) { return stockopt_opt(OC_BOOTSOUND, 0) != 0; }
bool stockopt_sms_ring(void) { return stockopt_opt(OC_MSGRING, 0) != 0; }
bool stockopt_call_ring(void) { return stockopt_opt(OC_CALLRING, 0) != 0; }
uint8_t stockopt_talk_permit(void) { return (uint8_t)(stockopt_opt(OC_TALKTIPS, 0) & 3u); }
bool stockopt_tot_predict(void) { return stockopt_opt(OC_TOTPRE, 0) != 0; }

/* Menu Exit Time: 5S, 10S .. 60S (radio menu order); stock CSV 0 = 5 s? the CPS default 0 means "off" here */
uint32_t stockopt_menu_exit_s(void)
{
    if(!cfwcp_valid()) { return 0; }
    uint32_t v = stockopt_opt(OC_MENUEXIT, 0);                 /* absent = stock default 0 = 5 s */
    return v < 12u ? (v + 1u) * 5u : 0;
}

bool stockopt_start_channel(int *zone, int *ch)
{
    if(!stockopt_opt(OC_STARTCHUSE, 0)) { return false; }
    *zone = (int)stockopt_opt(OC_STARTZONE1, 0);
    *ch = (int)stockopt_opt(OC_STARTCH1, 0);
    return true;
}

/* ------------------------------------------------------------------ auto repeater */
int stockopt_rpt_offset_count(void) { return rows(SO_T_ARPT, 0); }
uint32_t stockopt_rpt_offset(int i)
{
    char b[16];
    return cell(SO_T_ARPT, 1, i, b, sizeof b) ? stockopt_mhz(b) : 0;
}

/* Auto Repeater A: 0 off, 1 positive, 2 negative (TX above / below RX). Ranges: AutoRepFreq0..3 (MHz, VHF 136-174 /
 * UHF 400-480 by default). Offset: Auto Repeater1(VHF / UHF) = entry of the offset list (1-based, 0 = none: the
 * usual amateur offset, 600 kHz VHF / 5 MHz UHF). */
static uint32_t rpt_off(uint32_t rx, int lo_col, uint32_t lo_d, int hi_col, uint32_t hi_d, int idx_col, uint32_t usual)
{
    uint32_t lo = stockopt_opt(lo_col, lo_d) * 1000000u, hi = stockopt_opt(hi_col, hi_d) * 1000000u;
    if(!lo || rx < lo || rx > hi) { return 0; }
    uint32_t k = stockopt_opt(idx_col, 0);
    return k ? stockopt_rpt_offset((int)k - 1) : usual;
}

/* second range (Auto Repeater2) first when its list entry is set: it is the narrower, user-made one */
uint32_t stockopt_auto_rpt_tx(uint32_t rx)
{
    uint32_t dir = stockopt_opt(OC_AUTORPT, 0), off = 0;
    if(dir != 1u && dir != 2u) { return rx; }
    off = rpt_off(rx, 151, 0, 152, 0, 144, 0);                        /* VHF 2: AutoRepMin/MaxFreqV2, VhfAutoRep2 */
    if(!off) { off = rpt_off(rx, 153, 0, 154, 0, 143, 0); }            /* UHF 2 */
    if(!off) { off = rpt_off(rx, OC_ARF0, 136, OC_ARF1, 174, OC_VHFRPT, 600000u); }
    if(!off) { off = rpt_off(rx, OC_ARF2, 400, OC_ARF3, 480, OC_UHFRPT, 5000000u); }
    if(!off) { return rx; }
    return dir == 1u ? rx + off : rx - off;
}

/* ------------------------------------------------------------------ channel columns */
void stockopt_channel(int ch, so_chan_t *o)
{
    char b[16];
    memset(o, 0, sizeof *o);
    if(ch < 0) { return; }
    if(cell(SO_T_CHANNEL, CH_COL(13), ch, b, sizeof b) && !streq_i(b, "Always") && !streq_i(b, "Off")) {
        o->busy_lock = SO_BUSY_FREE;
    }
    /* stock default of an analog channel is CTCSS/DCS (cps/stock.py CH_DEFAULT_A): absent = tone squelch (FM only) */
    o->sql_tone = !cell(SO_T_CHANNEL, CH_COL(14), ch, b, sizeof b) || !streq_i(b, "Carrier");
    o->alias = cell(SO_T_CHANNEL, CH_COL(51), ch, b, sizeof b) && (b[0] == '1' || streq_i(b, "On"));
    o->work_alone = cell(SO_T_CHANNEL, CH_COL(32), ch, b, sizeof b) && (b[0] == '1' || streq_i(b, "On"));
    o->auto_scan = cell(SO_T_CHANNEL, CH_COL(49), ch, b, sizeof b) && (b[0] == '1' || streq_i(b, "On"));
    if(cell(SO_T_CHANNEL, CH_COL(45), ch, b, sizeof b)) { o->dmr_mode = (uint8_t)(dec(b) & 3u); }
    if(cell(SO_T_CHANNEL, CH_COL(40), ch, b, sizeof b) && streq_i(b, "Analog") &&
       cell(SO_T_CHANNEL, CH_COL(38), ch, b, sizeof b) && (strstr(b, "End") || strstr(b, "end"))) { o->aprs_end = true; }
    if(cell(SO_T_CHANNEL, CH_COL(24), ch, b, sizeof b) && streq_i(b, "On")) { o->ptt_prohibit = true; }
    if(cell(SO_T_CHANNEL, CH_COL(25), ch, b, sizeof b) && streq_i(b, "On")) { o->reverse = true; }
    if(cell(SO_T_CHANNEL, CH_COL(42), ch, b, sizeof b)) { o->correct_hz = sdec(b); }
    if(cell(SO_T_CHANNEL, CH_COL(44), ch, b, sizeof b) && (b[0] == '1' || streq_i(b, "On"))) { o->no_roam = true; }
}

void stockopt_channel_apply(int ch, channel_t *c)
{
    so_chan_t o;
    stockopt_channel(ch, &o);
    if(o.dmr_mode >= 2 && c->mode == CFW_MODE_DMR) { c->dmr_tdma = true; }
    if(o.reverse && c->tx_hz) { uint32_t t = c->rx_hz; c->rx_hz = c->tx_hz; c->tx_hz = t; }
    if(o.correct_hz) {
        c->rx_hz = (uint32_t)((int32_t)c->rx_hz + o.correct_hz);
        if(c->tx_hz) { c->tx_hz = (uint32_t)((int32_t)c->tx_hz + o.correct_hz); }
    }
}

/* ------------------------------------------------------------------ hot keys */
#ifdef HK_COLS_D168
enum { HK_MODE = 1, HK_MENU = 2, HK_CTYPE = 3, HK_OBJ = 4, HK_OBJTYPE = 5, HK_DIGI = 6, HK_CONTENT = 7 };
#else
enum { HK_MODE = 1, HK_MENU = 2, HK_CTYPE = 3, HK_OBJ = 4, HK_OBJTYPE = 5, HK_DIGI = 6, HK_CONTENT = 8 };
#endif
static const char *const k_hk_menu_default[18] = { "SMS", "New SMS", "Hot Text", "Received SMS", "Send SMS",
    "Contact List", "Manual Dial", "Call Log", "Dialed Call", "Received Call", "Missed Call", "Zone", "Radio Set",
    "SMS", "New SMS", "Hot Text", "Received SMS", "Send SMS" };

bool stockopt_hotkey(int n, so_hotkey_t *o)
{
    char b[24];
    memset(o, 0, sizeof *o);
    if(n < 1 || n > 18) { return false; }
    int r = n - 1;
    if(!stored(SO_T_HOTKEY)) {                       /* stock default: every key opens a menu */
        o->mode = SO_HK_MENU;
        copy_trim(o->menu, k_hk_menu_default[r], sizeof o->menu);
        return true;
    }
    if(r >= rows(SO_T_HOTKEY, 18)) { return false; }
    if(!cell(SO_T_HOTKEY, HK_MODE, r, b, sizeof b)) { return false; }
    if(streq_i(b, "Menu")) {
        o->mode = SO_HK_MENU;
        if(cell(SO_T_HOTKEY, HK_MENU, r, b, sizeof b)) { copy_trim(o->menu, b, sizeof o->menu); }
    } else if(streq_i(b, "Call")) {
        o->mode = SO_HK_CALL;
        if(cell(SO_T_HOTKEY, HK_CTYPE, r, b, sizeof b)) { o->digital = streq_i(b, "Digital"); }
        if(cell(SO_T_HOTKEY, HK_OBJ, r, b, sizeof b) && !streq_i(b, "Off")) { copy_trim(o->object, b, sizeof o->object); }
        if(cell(SO_T_HOTKEY, HK_DIGI, r, b, sizeof b)) { o->priv = streq_i(b, "Private Call"); }
        if(cell(SO_T_HOTKEY, HK_OBJTYPE, r, b, sizeof b) && streq_i(b, "Private Call")) { o->priv = true; }
        if(cell(SO_T_HOTKEY, HK_CONTENT, r, b, sizeof b) && !streq_i(b, "Off")) { copy_trim(o->content, b, sizeof o->content); }
    } else {
        o->mode = SO_HK_OFF;
    }
    return true;
}

/* Hot Key Quick Call: Operation Type Off / DTMF / 2Tone / 5Tone (/ MDC1200), Call ID = encode list entry number */
bool stockopt_quickcall(int n, so_qcall_t *o)
{
    char b[24];
    memset(o, 0, sizeof *o);
    if(n < 1 || n > 4 || !stored(SO_T_QCALL) || n > rows(SO_T_QCALL, 4)) { return false; }
    if(!cell(SO_T_QCALL, 1, n - 1, b, sizeof b)) { return false; }
    o->kind = streq_i(b, "DTMF") ? 1 : streq_i(b, "2Tone") ? 2 : streq_i(b, "5Tone") ? 3 :
              (streq_i(b, "MDC1200") || streq_i(b, "QDC1200")) ? 4 : 0;
    if(cell(SO_T_QCALL, 2, n - 1, b, sizeof b) && !streq_i(b, "Off")) { o->entry = (uint8_t)dec(b); }
    return o->kind != 0;
}

int stockopt_hstate_count(void) { return rows(SO_T_HSTATE, 0); }
bool stockopt_hstate(int i, char *buf, int n)
{
    char b[48];
    if(!cell(SO_T_HSTATE, 1, i, b, sizeof b)) { buf[0] = 0; return false; }
    copy_trim(buf, b, n);
    return true;
}

/* ------------------------------------------------------------------ list tables: one walk of section 10 at init */
#if defined(CFW_BOARD_D168UV)
#define RCH_MAX 32
#define RZ_MAX  16
#define BM_MAX  101                                                  /* 100 FM memories + VFO; no AM */
#define AM_MAX  1
#define AMZ_MAX 1
#else
#define RCH_MAX 64
#define RZ_MAX  32
#define BM_MAX  128
#define AM_MAX  257
#define AMZ_MAX 32
#endif
typedef struct { const uint8_t *p; uint16_t n; } txt_t;              /* text in the (memory mapped) codeplug */
typedef struct { uint32_t hz; int16_t no; bool del; char name[17]; } bm_t;
static so_roamch_t s_rch[RCH_MAX];
static int s_nrch;
static char s_rzname[RZ_MAX][17];
static txt_t s_rzmem[RZ_MAX];
static int s_nrz;
static so_gpsroam_t s_gr[32];
static bool s_gr_stored;
static txt_t s_grc[32][11];
static bm_t s_fm[BM_MAX], s_am[AM_MAX];
static int s_nfm, s_nam;                                             /* rows (VFO row included) */
static bool s_fm_stored, s_am_stored;
static char s_amzname[AMZ_MAX][17];
static txt_t s_amzmem[AMZ_MAX];
static int s_namz;

static void tcopy(char *d, const uint8_t *p, uint16_t n, int max)
{
    char b[48];
    int k = n < (int)sizeof b - 1 ? n : (int)sizeof b - 1;
    memcpy(b, p, (size_t)k); b[k] = 0;
    copy_trim(d, b, max);
}
static void ttext(char *b, int bn, const uint8_t *p, uint16_t n) { int k = n < bn - 1 ? n : bn - 1; memcpy(b, p, (size_t)k); b[k] = 0; }

static bool walk_fn(const uint8_t *e, uint32_t pos, void *u)
{
    (void)pos; (void)u;
    uint8_t t = e[0], c = e[1];
    uint16_t r = (uint16_t)(e[2] | e[3] << 8), n = (uint16_t)(e[4] | e[5] << 8);
    const uint8_t *p = e + 6;
    char b[24];
    ttext(b, sizeof b, p, n);
    if(t == SO_T_OPT && c < OPT_N && r == 0) {
        if(n && p[0] >= '0' && p[0] <= '9') {
            uint32_t v = dec(b);
            s_optv[c] = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
            s_optp[c >> 3] |= (uint8_t)(1u << (c & 7));
        } else { s_optp[c >> 3] &= (uint8_t)~(1u << (c & 7)); }
        return true;
    }
    if(c == 0xFF) {                                                  /* row count of a list table */
        int k = (int)dec(b);
        if(t == SO_T_ROAMCH) { s_nrch = k < RCH_MAX ? k : RCH_MAX; }
        else if(t == SO_T_ROAMZ) { s_nrz = k < RZ_MAX ? k : RZ_MAX; }
        else if(t == SO_T_GPSROAM) { s_gr_stored = true; }
        else if(t == SO_T_FM) { s_fm_stored = true; s_nfm = k < BM_MAX ? k : BM_MAX; }
        else if(t == SO_T_AMAIR) { s_am_stored = true; s_nam = k < AM_MAX ? k : AM_MAX; }
        else if(t == SO_T_AMZONE) { s_namz = k < AMZ_MAX ? k : AMZ_MAX; }
        return true;
    }
    if(t == SO_T_ROAMCH && r < RCH_MAX) {
        so_roamch_t *o = &s_rch[r];
        if(c == 1) { o->rx_hz = stockopt_mhz(b); }
        else if(c == 2) { o->tx_hz = stockopt_mhz(b); }
        else if(c == 3) { o->cc = (uint8_t)(dec(b) & 15u); }
        else if(c == 4) { o->slot = strchr(b, '2') ? 2 : 1; }
        else if(c == 5) { tcopy(o->name, p, n, sizeof o->name); }
    } else if(t == SO_T_ROAMZ && r < RZ_MAX) {
        if(c == 1) { tcopy(s_rzname[r], p, n, 17); }
        else if(c == 2) { s_rzmem[r].p = p; s_rzmem[r].n = n; }
    } else if(t == SO_T_GPSROAM && r < 32 && c < 11) {
        s_grc[r][c].p = p; s_grc[r][c].n = n;
    } else if((t == SO_T_FM && r < BM_MAX) || (t == SO_T_AMAIR && r < AM_MAX)) {
        bm_t *m = t == SO_T_FM ? &s_fm[r] : &s_am[r];
        int namecol = t == SO_T_FM ? 3 : 2;
#if defined(CFW_BOARD_D168UV)
        if(t == SO_T_FM) { namecol = -1; }
#endif
        if(c == 0) { m->no = (int16_t)dec(b); }
        else if(c == 1) { m->hz = stockopt_mhz(b); }
        else if(c == 2 && t == SO_T_FM) { m->del = streq_i(b, "Del"); }
        else if(c == namecol) { tcopy(m->name, p, n, sizeof m->name); }
    } else if(t == SO_T_AMZONE && r < AMZ_MAX) {
        if(c == 1) { tcopy(s_amzname[r], p, n, 17); }
        else if(c == 2) { s_amzmem[r].p = p; s_amzmem[r].n = n; }
    }
    return true;
}

static void lists_load(void)
{
    memset(s_optp, 0, sizeof s_optp);
    memset(s_rch, 0, sizeof s_rch); memset(s_rzname, 0, sizeof s_rzname); memset(s_rzmem, 0, sizeof s_rzmem);
    memset(s_grc, 0, sizeof s_grc); memset(s_fm, 0, sizeof s_fm); memset(s_am, 0, sizeof s_am);
    memset(s_amzname, 0, sizeof s_amzname); memset(s_amzmem, 0, sizeof s_amzmem);
    s_nrch = s_nrz = s_nfm = s_nam = s_namz = 0;
    s_gr_stored = s_fm_stored = s_am_stored = false;
    for(int i = 0; i < RCH_MAX; i++) { s_rch[i].slot = 1; }
    cfwcp_stock_walk(walk_fn, NULL);
    /* stock default rows when the CPS stored none: FM-001 88.000 MHz, AM-001 108.000 MHz */
    if(!s_fm_stored) { s_nfm = 1; s_fm[0].no = 1; s_fm[0].hz = 88000000u; copy_trim(s_fm[0].name, "FM-001", 17); }
    if(!s_am_stored) { s_nam = 1; s_am[0].no = 1; s_am[0].hz = 108000000u; copy_trim(s_am[0].name, "AM-001", 17); }
    /* GPS roaming rows */
    memset(s_gr, 0, sizeof s_gr);
    for(int i = 0; i < 32 && s_gr_stored; i++) {
        char b[16];
        uint32_t v[11];
        for(int c = 0; c < 11; c++) { ttext(b, sizeof b, s_grc[i][c].p ? s_grc[i][c].p : (const uint8_t *)"", s_grc[i][c].n); v[c] = c == 6 || c == 8 ? minutes_e5(b) : dec(b); }
        so_gpsroam_t *o = &s_gr[i];
        o->on = v[0] != 0;
        o->zone = v[1] >= 255u ? -1 : (int16_t)v[1];
        o->lat_e7 = (int32_t)(v[2] * 10000000u + v[6] * 100u / 60u) * (v[3] ? -1 : 1);
        o->lon_e7 = (int32_t)(v[4] * 10000000u + v[8] * 100u / 60u) * (v[5] ? -1 : 1);
        o->radius_m = v[10];
    }
}

/* name list "a|b|c" -> indices of the rows whose name matches */
static int resolve(const txt_t *t, bool am, uint16_t *idx, int max)
{
    static char b[1024];
    if(!t->p) { return 0; }
    ttext(b, sizeof b, t->p, t->n);
    int n = 0;
    char *p = b;
    while(*p && n < max) {
        char *e = strchr(p, '|');
        if(e) { *e = 0; }
        char nm[17];
        copy_trim(nm, p, sizeof nm);
        if(am) { for(int i = 0; i < stockopt_am_count(); i++) { so_bmem_t m; if(stockopt_am(i, &m) && streq_i(m.name, nm)) { idx[n++] = (uint16_t)i; break; } } }
        else { for(int i = 0; i < s_nrch; i++) { if(streq_i(s_rch[i].name, nm)) { idx[n++] = (uint16_t)i; break; } } }
        if(!e) { break; }
        p = e + 1;
    }
    return n;
}

/* ------------------------------------------------------------------ roaming */
int stockopt_roamch_count(void) { return s_nrch; }
bool stockopt_roamch(int i, so_roamch_t *o)
{
    if(i < 0 || i >= s_nrch) { memset(o, 0, sizeof *o); return false; }
    *o = s_rch[i];
    return o->rx_hz != 0;
}
int stockopt_roamzone_count(void) { return s_nrz; }
bool stockopt_roamzone(int z, char *name, int n)
{
    name[0] = 0;
    if(z < 0 || z >= s_nrz) { return false; }
    copy_trim(name, s_rzname[z], n);
    return true;
}
int stockopt_roamzone_members(int z, uint8_t *idx, int max)
{
    uint16_t t[RCH_MAX];
    if(z < 0 || z >= s_nrz) { return 0; }
    int n = resolve(&s_rzmem[z], false, t, max < RCH_MAX ? max : RCH_MAX);
    for(int i = 0; i < n; i++) { idx[i] = (uint8_t)t[i]; }
    return n;
}

int stockopt_gpsroam_count(void) { return 32; }
bool stockopt_gpsroam(int i, so_gpsroam_t *o)
{
    if(i < 0 || i >= 32 || !s_gr_stored) { memset(o, 0, sizeof *o); o->zone = -1; return false; }
    *o = s_gr[i];
    return o->on && o->zone >= 0 && o->radius_m > 0;
}

/* ------------------------------------------------------------------ broadcast memories */
/* FM: No. (101 = VFO), Frequency[MHz], Scan (Add / Del), Name (D890UV); AM Air: No. (257 = VFO), Frequency, Name */
static bool bm_get(const bm_t *t, int rows, int vfo_no, int i, so_bmem_t *o, const char *pre)
{
    memset(o, 0, sizeof *o);
    for(int r = 0, k = 0; r < rows; r++) {
        if(t[r].no == vfo_no || !t[r].hz) { continue; }
        if(k++ != i) { continue; }
        o->hz = t[r].hz; o->scan = !t[r].del;
        copy_trim(o->name, t[r].name, sizeof o->name);
        if(!o->name[0]) { fmt_u(fmt_s(o->name, pre), (uint32_t)i + 1u, 3); }
        return true;
    }
    return false;
}
static int bm_count(const bm_t *t, int rows, int vfo_no)
{
    int n = 0;
    for(int r = 0; r < rows; r++) { if(t[r].no != vfo_no && t[r].hz) { n++; } }
    return n;
}
static bool bm_vfo(const bm_t *t, int rows, int vfo_no, uint32_t *hz)
{
    for(int r = 0; r < rows; r++) { if(t[r].no == vfo_no && t[r].hz) { *hz = t[r].hz; return true; } }
    return false;
}

int stockopt_fm_count(void) { return bm_count(s_fm, s_nfm, 101); }
bool stockopt_fm(int i, so_bmem_t *o) { return bm_get(s_fm, s_nfm, 101, i, o, "FM-"); }
bool stockopt_fm_vfo(uint32_t *hz) { return bm_vfo(s_fm, s_nfm, 101, hz); }
#if defined(CFW_BOARD_D168UV)
int stockopt_am_count(void) { return 0; }
bool stockopt_am(int i, so_bmem_t *o) { (void)i; memset(o, 0, sizeof *o); return false; }
bool stockopt_am_vfo(uint32_t *hz) { (void)hz; return false; }
int stockopt_amzone_count(void) { return 0; }
bool stockopt_amzone(int z, char *name, int n) { (void)z; (void)n; name[0] = 0; return false; }
int stockopt_amzone_members(int z, uint16_t *idx, int max) { (void)z; (void)idx; (void)max; return 0; }
#else
int stockopt_am_count(void) { return bm_count(s_am, s_nam, 257); }
bool stockopt_am(int i, so_bmem_t *o) { return bm_get(s_am, s_nam, 257, i, o, "AM-"); }
bool stockopt_am_vfo(uint32_t *hz) { return bm_vfo(s_am, s_nam, 257, hz); }
int stockopt_amzone_count(void) { return s_namz; }
bool stockopt_amzone(int z, char *name, int n)
{
    name[0] = 0;
    if(z < 0 || z >= s_namz) { return false; }
    copy_trim(name, s_amzname[z], n);
    return true;
}
int stockopt_amzone_members(int z, uint16_t *idx, int max)
{
    if(z < 0 || z >= s_namz) { return 0; }
    return resolve(&s_amzmem[z], true, idx, max);
}
#endif

/* ------------------------------------------------------------------ APRS */
static so_aprs_t s_aprs;

static void aprs_load(void)
{
    char b[48];
    so_aprs_t *a = &s_aprs;
    memset(a, 0, sizeof *a);
    a->manual_s = (uint16_t)(cfwcp_stock_value(SO_T_APRS, 0, b, sizeof b) ? dec(b) : 0);
    a->auto_s = (uint16_t)(cfwcp_stock_value(SO_T_APRS, 1, b, sizeof b) ? dec(b) : 0);
#if defined(CFW_BOARD_D168UV)
    const uint32_t fixed_dflt = 1, tossid_dflt = 1;                  /* D168UV CPS defaults */
#else
    const uint32_t fixed_dflt = 0, tossid_dflt = 0;
#endif
    a->fixed = (cfwcp_stock_value(SO_T_APRS, 3, b, sizeof b) ? dec(b) : fixed_dflt) != 0;
    bool pos_set = false;                                             /* any position column differs from 23 N 113 E */
    for(int c = 4; c <= 11; c++) { pos_set = pos_set || cfwcp_stock_value(SO_T_APRS, c, b, sizeof b); }
    if(!pos_set) { a->fixed = false; }      /* the stock placeholder position is never beaconed (D168UV default: on) */
    uint32_t latd = cfwcp_stock_value(SO_T_APRS, 4, b, sizeof b) ? dec(b) : 23u;     /* stock defaults 23 N 113 E */
    uint32_t latm = cfwcp_stock_value(SO_T_APRS, 5, b, sizeof b) ? dec(b) : 0;
    uint32_t latf = cfwcp_stock_value(SO_T_APRS, 6, b, sizeof b) ? dec(b) : 0;
    bool south = cfwcp_stock_value(SO_T_APRS, 7, b, sizeof b) && dec(b) != 0;
    uint32_t lond = cfwcp_stock_value(SO_T_APRS, 8, b, sizeof b) ? dec(b) : 113u;
    uint32_t lonm = cfwcp_stock_value(SO_T_APRS, 9, b, sizeof b) ? dec(b) : 0;
    uint32_t lonf = cfwcp_stock_value(SO_T_APRS, 10, b, sizeof b) ? dec(b) : 0;
    bool west = cfwcp_stock_value(SO_T_APRS, 11, b, sizeof b) && dec(b) != 0;
    /* minutes = MinInt + MinMark / 100 */
    a->lat_e7 = (int32_t)(latd * 10000000u + (latm * 100u + latf % 100u) * 10000u / 6u) * (south ? -1 : 1);
    a->lon_e7 = (int32_t)(lond * 10000000u + (lonm * 100u + lonf % 100u) * 10000u / 6u) * (west ? -1 : 1);
    /* text fields: "" / 0 = not set in the codeplug (the stock defaults APAT81 / BG6LKT / "/[" / WIDE1-1,WIDE2-1 /
     * APRSCN are not stored by the CPS): the cfw keeps its own (APZCFW, radio callsign, "/>", WIDE1-1, "cfw CALL") */
    if(cfwcp_stock_value(SO_T_APRS, 48, b, sizeof b) && b[0]) { copy_trim(a->tocall, b, sizeof a->tocall); }
    a->tocall_ssid = (uint8_t)((cfwcp_stock_value(SO_T_APRS, 49, b, sizeof b) ? dec(b) : tossid_dflt) & 15u);
    if(cfwcp_stock_value(SO_T_APRS, 50, b, sizeof b) && b[0]) { copy_trim(a->call, b, sizeof a->call); }
    a->ssid = (uint8_t)(cfwcp_stock_value(SO_T_APRS, 51, b, sizeof b) ? dec(b) & 15u : 0);
    if(cfwcp_stock_value(SO_T_APRS, 52, b, sizeof b) && b[0]) { a->sym_table = b[0]; }
    if(cfwcp_stock_value(SO_T_APRS, 53, b, sizeof b) && b[0]) { a->sym = b[0]; }
    if(cfwcp_stock_value(SO_T_APRS, 54, b, sizeof b) && b[0]) { copy_trim(a->path, b, sizeof a->path); }
    if(cfwcp_stock_value(SO_T_APRS, 55, b, sizeof b)) { copy_trim(a->text, b, sizeof a->text); }
    a->tx_hz = cfwcp_stock_value(SO_T_APRS, 56, b, sizeof b) && b[0] ? stockopt_mhz(b) : 0;
}

const so_aprs_t *stockopt_aprs(void) { return &s_aprs; }

void stockopt_init(void)
{
    lists_load();
    aprs_load();
}
