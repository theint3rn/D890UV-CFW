/* Analog signalling: codeplug lists, TX bursts, alert tones, heard list (see sig.h, docs/AUDIO_PATHS.md "Signalling").
 *
 * Codeplug: the stock CPS tables travel as stock CSV text in section 10 (cfwcp_stock_value*). Table ids = position
 * in the radio model's stock table order (cfw_cps/schema/stock_d890uv.json / stock_d168uv.json); the per-channel
 * stock columns are table 0xF0, row = codeplug channel index (absent = stock default).
 *
 *            D890UV CPS                      D168UV CPS
 *   4        Analog Address Book             Analog Address Book            No., Number, Name
 *   9        5Tone Encode                    5Tone Setting                  NO., Encode ID, Standard, Time[ms], Name
 *   10       2Tone Encode                    2Tone Setting                  NO., 1st Tone[Hz], 2nd Tone[Hz], Name
 *   11       DTMF Encode                     DTMF Setting                   DTMF ID, DTMF Encode
 *   22       Alert Tone                      Alert Tone                     Freq<a><s>, Time<a><s> (10 ms units)
 *   35 / 25  MDC1200 Address Book            QDC Address Book
 *   36 / 23  MDC1200 Encode                  QDC1200 Setting
 *   -  / 24  -                               QDC1200 Decode (record: Self IDs, ...)
 *   65 / 66 / 67 / 68   DTMF / 2Tone / 5Tone Settings, 5Tone Information Codes (.rdt-only dialogs; sig_cfg())
 * Values are parsed leniently (the stock CPS option texts are not all known): numbers are decimal (frequencies,
 * entry numbers, ms) or hex (MDC / QDC unit ids, 4 digits); option texts are matched by keyword. */
#include <string.h>
#include "sig.h"
#include "radio.h"
#include "cfwcp.h"
#include "stockdlg.h"
#include "channels.h"
#include "hal.h"
#include "board.h"
#include "voice.h"
#include "aprs.h"
#include "dsp_backend.h"
#include "modem_core.h"

#define T_ABOOK   4
#define T_5T      9
#define T_2T     10
#define T_DTMF   11
#define T_ALERT  22
#define T_CHAN   0xF0
#ifdef CFW_BOARD_D168UV
#define T_MENC   23
#define T_MDEC   24
#define T_MBOOK  25
/* Channel columns (STOCK_CPS_MAP_D168.md) */
enum { C_OPTSIG = 14, C_DTMFID = 15, C_2TID = 16, C_5TID = 17, C_PTTID = 18, C_2TDEC = 33, C_R5BOT = 46, C_R5EOT = 47,
       C_MDC = 58 };
/* QDC1200 Setting / QDC Address Book: No., Call Type, Private ID, Group ID, Type, Ack, Name */
enum { M_CTYPE = 1, M_PRI = 2, M_GRP = 3, M_KIND = 4, M_ACK = 5, M_NAME = 6 };
#define MDC_LABEL "QDC1200"
#else
#define T_MENC   36
#define T_MDEC   -1
#define T_MBOOK  35
enum { C_OPTSIG = 15, C_DTMFID = 16, C_2TID = 17, C_5TID = 18, C_PTTID = 19, C_2TDEC = 34, C_R5BOT = 47, C_R5EOT = 48,
       C_MDC = 55 };
/* MDC1200 Encode: ID, Enc_Type, ENC_KIND, ENC_MDC_ACK, ENC_GROUP_ID, ENC_PRI_ID, ENC_MDC_ENC_NAME (address book:
 * ID, Type, Attr, ACK, GROUP_ID, PRI_ID, Name) */
enum { M_CTYPE = 1, M_KIND = 2, M_ACK = 3, M_GRP = 4, M_PRI = 5, M_NAME = 6 };
#define MDC_LABEL "MDC1200"
#endif

const char *sig_mdc_label(void) { return MDC_LABEL; }

/* ------------------------------------------------------------------ 5-tone standards */
static const char *const k_5t_names[SIG_5T_STD_N] = { "ZVEI1", "ZVEI2", "ZVEI3", "PZVEI", "DZVEI", "PDZVEI", "CCIR1",
    "CCIR2", "PCCIR", "EEA", "EURO SIGNAL", "NATEL", "MODAT", "CCITT" };
/* digits 0-9, A-F (E = repeat tone); common published tables. Only 0-9 and E are used by the cfw (Encode IDs are
 * digits). Not checked against a stock radio's output. */
static const uint16_t k_zvei1[16] = { 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 2800, 810, 970, 885, 2600, 680 };
static const uint16_t k_zvei3[16] = { 2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 885, 810, 740, 680, 2400, 2600 };
static const uint16_t k_ccir[16] = { 1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860, 2400, 930, 2247, 991, 2110, 0 };
static const uint16_t k_eea[16] = { 1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860, 1055, 930, 2247, 991, 2110, 0 };
static const uint16_t k_euro[16] = { 980, 903, 833, 767, 707, 652, 601, 554, 511, 471, 434, 400, 369, 340, 313, 1063 };
static const uint16_t k_natel[16] = { 1633, 631, 697, 770, 852, 941, 1040, 1209, 1336, 1477, 0, 0, 0, 0, 1995, 0 };
static const uint16_t k_modat[16] = { 600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869, 0, 0, 0, 0, 459, 0 };
static const uint16_t k_ccitt[16] = { 400, 697, 770, 852, 941, 1209, 1336, 1477, 1633, 1750, 0, 0, 0, 0, 2400, 0 };
static const uint16_t *const k_5t_tab[SIG_5T_STD_N] = { k_zvei1, k_zvei1, k_zvei3, k_zvei1, k_zvei3, k_zvei3, k_ccir,
    k_ccir, k_ccir, k_eea, k_euro, k_natel, k_modat, k_ccitt };
static const uint8_t k_5t_ms[SIG_5T_STD_N] = { 70, 70, 70, 70, 70, 70, 100, 70, 100, 40, 100, 70, 40, 100 };

const char *sig_5t_std_name(int std) { return (std >= 0 && std < SIG_5T_STD_N) ? k_5t_names[std] : "?"; }
const uint16_t *sig_5t_tones(int std) { return k_5t_tab[(std >= 0 && std < SIG_5T_STD_N) ? std : 0]; }
uint16_t sig_5t_default_ms(int std) { return k_5t_ms[(std >= 0 && std < SIG_5T_STD_N) ? std : 0]; }

/* ------------------------------------------------------------------ text helpers */
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static bool has(const char *s, const char *kw)                 /* case-insensitive substring */
{
    for(; *s; s++) {
        int i = 0;
        while(kw[i] && s[i] && up(s[i]) == up(kw[i])) { i++; }
        if(!kw[i]) { return true; }
    }
    return false;
}
static uint32_t dec(const char *t)
{
    uint32_t v = 0;
    while(*t == ' ') { t++; }
    while(*t >= '0' && *t <= '9') { v = v * 10u + (uint32_t)(*t++ - '0'); }
    return v;
}
static int hexd(char c)
{
    c = up(c);
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}
static uint16_t hex16(const char *t)
{
    uint32_t v = 0;
    while(*t == ' ') { t++; }
    if(t[0] == '0' && up(t[1]) == 'X') { t += 2; }
    for(int d; (d = hexd(*t)) >= 0; t++) { v = v << 4 | (uint32_t)d; }
    return (uint16_t)v;
}
static void copyn(char *d, const char *s, int n) { int i = 0; for(; i < n - 1 && s[i]; i++) { d[i] = s[i]; } d[i] = 0; }

static bool cell(int table, int col, int row, char *b, int n)
{
    if(table < 0) { b[0] = 0; return false; }
    if(!cfwcp_stock_value_row(table, col, row, b, n)) { b[0] = 0; return false; }
    return true;
}
static int rows(int table)
{
    char b[8];
    return (table >= 0 && cfwcp_stock_value(table, 0xFF, b, sizeof b)) ? (int)dec(b) : 0;
}

/* ------------------------------------------------------------------ lists */
int sig_dtmf_count(void) { return rows(T_DTMF); }
bool sig_dtmf(int i, char *seq, int n)
{
    char b[40];
    if(!cell(T_DTMF, 1, i, b, sizeof b)) { seq[0] = 0; return false; }
    int k = 0;
    for(const char *p = b; *p && k < n - 1; p++) {
        char c = up(*p);
        if((c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D')) { seq[k++] = c; }
    }
    seq[k] = 0;
    return k > 0;
}

/* entry number column (1-based) -> row: the row whose number cell equals id, else row id - 1 */
static int row_of(int table, int ncol, int id)
{
    int n = rows(table);
    char b[8];
    for(int r = 0; r < n; r++) { if(cell(table, ncol, r, b, sizeof b) && (int)dec(b) == id) { return r; } }
    return (id >= 1 && id <= n) ? id - 1 : -1;
}

int sig_2t_count(void) { return rows(T_2T); }
bool sig_2t(int i, sig_2t_t *e)
{
    char b[24];
    memset(e, 0, sizeof *e);
    if(cell(T_2T, 1, i, b, sizeof b)) { e->f1 = (uint16_t)dec(b); }
    if(cell(T_2T, 2, i, b, sizeof b)) { e->f2 = (uint16_t)dec(b); }
    cell(T_2T, 3, i, e->name, sizeof e->name);
    return e->f1 >= 100 && e->f1 <= 3500;
}

static int std_of(const char *s)
{
    int best = -1, bl = 0;
    for(int k = 0; k < SIG_5T_STD_N; k++) {                     /* longest name contained ("PZVEI" before "ZVEI1") */
        int l = (int)strlen(k_5t_names[k]);
        if(has(s, k_5t_names[k]) && l > bl) { best = k; bl = l; }
    }
    if(best < 0 && has(s, "EURO")) { best = 10; }
    return best < 0 ? 0 : best;
}

int sig_5t_count(void) { return rows(T_5T); }
bool sig_5t(int i, sig_5t_t *e)
{
    char b[40];
    memset(e, 0, sizeof *e);
    if(cell(T_5T, 1, i, b, sizeof b)) {
        int k = 0;
        for(const char *p = b; *p && k < SIG_SEQ_MAX; p++) { int d = hexd(*p); if(d >= 0) { e->seq[k++] = up(*p); } }
        e->seq[k] = 0;
    }
    e->std = cell(T_5T, 2, i, b, sizeof b) ? (uint8_t)std_of(b) : 0;
    e->tone_ms = cell(T_5T, 3, i, b, sizeof b) ? (uint16_t)dec(b) : 0;
    if(e->tone_ms < 20 || e->tone_ms > 1000) { e->tone_ms = sig_5t_default_ms(e->std); }
    cell(T_5T, 4, i, e->name, sizeof e->name);
    return e->seq[0] != 0;
}

/* ------------------------------------------------------------------ stock setting dialogs (tables 65-68) */
#define T_SDTMF  SD_T_DTMF
#define T_S2T    SD_T_2TONE
#define T_S5T    SD_T_5TONE
#define T_S5INFO SD_T_5TINFO
static sig_cfg_t s_cfg;
static uint32_t s_cfg_gen;
static bool s_cfg_ok;

static uint16_t ms10(int table, int col, uint32_t dflt) { return (uint16_t)(stockdlg_num(table, col, dflt) * 10u); }
static char code_char(uint32_t v)                              /* 10 A .. 13 D, 14 *, 15 # */
{
    return v >= 10 && v <= 15 ? "ABCD*#"[v - 10] : 0;
}
static void digits(int table, int col, char *out, int n, bool hex)
{
    char b[40];
    int k = 0;
    if(stockdlg_text(table, col, b, sizeof b)) {
        for(const char *p = b; *p && k < n - 1; p++) {
            char ch = up(*p);
            if(hex ? hexd(ch) >= 0 : ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'D') || ch == '*' || ch == '#')) { out[k++] = ch; }
        }
    }
    out[k] = 0;
}
static void cfg_5t(sig_5t_t *e, int col)                       /* BOT / EOT: Encode ID, standard, tone time */
{
    memset(e, 0, sizeof *e);
    digits(T_S5T, col, e->seq, sizeof e->seq, true);
    e->std = (uint8_t)stockdlg_num(T_S5T, col + 1, 0);
    if(e->std >= SIG_5T_STD_N) { e->std = 0; }
    e->tone_ms = (uint16_t)stockdlg_num(T_S5T, col + 2, 70);
    if(e->tone_ms < 20 || e->tone_ms > 1000) { e->tone_ms = sig_5t_default_ms(e->std); }
}

const sig_cfg_t *sig_cfg(void)
{
    uint32_t gen = cfwcp_generation();
    if(s_cfg_ok && gen == s_cfg_gen) { return &s_cfg; }
    static const uint16_t tt[5] = { 50, 100, 200, 300, 500 };
    sig_cfg_t *g = &s_cfg;
    memset(g, 0, sizeof *g);
    s_cfg_ok = true; s_cfg_gen = gen;
    uint32_t v = stockdlg_num(T_SDTMF, 0, 1);
    g->dt_ms = tt[v < 5 ? v : 1];
    digits(T_SDTMF, 1, g->dt_self, sizeof g->dt_self, false);
    if(!g->dt_self[0]) { copyn(g->dt_self, "000", sizeof g->dt_self); }
    g->dt_sep = code_char(stockdlg_num(T_SDTMF, 2, 14));
    g->dt_group = code_char(stockdlg_num(T_SDTMF, 3, 0));
    g->dt_resp = (uint8_t)stockdlg_num(T_SDTMF, 4, 0);
    g->dt_first = ms10(T_SDTMF, 5, 20);
    g->dt_pre = ms10(T_SDTMF, 6, 20);
    g->dt_lapse = ms10(T_SDTMF, 8, 20);
    v = stockdlg_num(T_SDTMF, 10, 10);
    g->dt_dpause = (uint16_t)((v > 30 ? 30 : v) * 1000u);
    digits(T_SDTMF, 13, g->dt_bot, sizeof g->dt_bot, false);
    digits(T_SDTMF, 14, g->dt_eot, sizeof g->dt_eot, false);
    g->t2_1 = (uint16_t)(stockdlg_num(T_S2T, 0, 5) * 100u);
    g->t2_2 = (uint16_t)(stockdlg_num(T_S2T, 1, 5) * 100u);
    g->t2_long = (uint16_t)(stockdlg_num(T_S2T, 2, 10) * 100u);
    g->t2_gap = (uint16_t)(stockdlg_num(T_S2T, 3, 0) * 100u);
    digits(T_S5T, 0, g->t5_self, sizeof g->t5_self, true);
    g->t5_std = (uint8_t)stockdlg_num(T_S5T, 1, 0);
    if(g->t5_std >= SIG_5T_STD_N) { g->t5_std = 0; }
    g->t5_resp = (uint8_t)stockdlg_num(T_S5T, 2, 0);
    v = stockdlg_num(T_S5T, 4, 2);
    g->t5_dec = (uint8_t)(v < 2 ? 2 : v > 10 ? 10 : v);        /* 10 ms blocks; one block cannot tell a tone from noise */
    g->t5_any = stockdlg_num(T_S5T, 6, 0) != 0;
    g->t5_pre = ms10(T_S5T, 7, 20);
    g->t5_lapse = ms10(T_S5T, 8, 20);
    g->t5_pause = (uint16_t)stockdlg_num(T_S5T, 9, 10);
    cfg_5t(&g->t5_bot, 14);
    cfg_5t(&g->t5_eot, 17);
    return g;
}

/* 5Tone information code: sequence = Information ID, or Self ID + Information ID (assumption). Only its number and
 * decoding response are used: the function option may be a remote command and is never acted on. */
static int info_code(const char *seq, uint8_t *resp)
{
    const sig_cfg_t *g = sig_cfg();
    int sl = (int)strlen(g->t5_self);
    char b[40], id[SIG_SEQ_MAX + 1];
    for(int r = 0, n = stockdlg_rows(T_S5INFO); r < n; r++) {
        if(!stockdlg_row_text(T_S5INFO, r, 3, b, sizeof b)) { continue; }
        int k = 0;
        for(const char *p = b; *p && k < SIG_SEQ_MAX; p++) { if(hexd(*p) >= 0) { id[k++] = up(*p); } }
        id[k] = 0;
        if(!k) { continue; }
        if(!strcmp(seq, id) || (sl && !strncmp(seq, g->t5_self, (size_t)sl) && !strcmp(seq + sl, id))) {
            *resp = stockdlg_row_text(T_S5INFO, r, 2, b, sizeof b) ? (uint8_t)dec(b) : 0;
            return stockdlg_row_text(T_S5INFO, r, 0, b, sizeof b) && dec(b) ? (int)dec(b) : r + 1;
        }
    }
    return 0;
}

static uint8_t mdc_type(const char *s)
{
    if(has(s, "EMERG")) { return MDC_T_EMERG; }
    if(has(s, "SEL")) { return MDC_T_SELCALL; }
    if(has(s, "CHECK")) { return MDC_T_CHECK; }
    if(has(s, "STATUS")) { return MDC_T_STATUS; }
    if(has(s, "PTT") || has(s, "ANI") || (has(s, "ID") && !has(s, "CALL"))) { return MDC_T_PTTID; }
    return MDC_T_CALL;                                           /* "Call", "Call Alert", "Page", "Selective Call", ... */
}

static bool mdc_row(int table, int i, sig_mdc_t *e)
{
    char b[24];
    memset(e, 0, sizeof *e);
    if(table < 0 || i < 0 || i >= rows(table)) { return false; }
    bool grp = cell(table, M_CTYPE, i, b, sizeof b) && has(b, "GROUP");
    e->type = cell(table, M_KIND, i, b, sizeof b) ? mdc_type(b) : MDC_T_CALL;
    char pri[12], gid[12];
    bool hp = cell(table, M_PRI, i, pri, sizeof pri) && pri[0];
    bool hg = cell(table, M_GRP, i, gid, sizeof gid) && gid[0];
    e->group = grp || (!hp && hg);
    e->unit = e->group ? hex16(gid) : hex16(pri);
    e->ack = cell(table, M_ACK, i, b, sizeof b) && (has(b, "ON") || has(b, "YES") || dec(b) > 0 || has(b, "ACK"));
    cell(table, M_NAME, i, e->name, sizeof e->name);
    /* op / arg as the stock V1.05 sends them (FUN_0810c51c, message kind DAT_20014e44): call alert and selective
     * call are double packets 35 89 <target> + <msg op> <msg arg> <own id>; radio check 63 85 <target>; emergency
     * 00 80 <own id>; PTT ID 01 00 (start) / 01 80 (end) <own id>. Status: 46 <status>, not seen in the stock. */
    switch(e->type) {
    case MDC_T_PTTID:   e->op = 0x01; e->arg = 0x80; break;
    case MDC_T_EMERG:   e->op = 0x00; e->arg = 0x80; break;
    case MDC_T_CHECK:   e->op = 0x63; e->arg = 0x85; break;
    case MDC_T_STATUS:  e->op = 0x46; e->arg = 0x00; break;
    case MDC_T_SELCALL: e->op = 0x81; e->arg = 0x0D; break;      /* message op / arg of the double packet */
    default:            e->op = 0x80; e->arg = 0x15; break;      /* call alert */
    }
    return true;
}
int sig_mdc_count(void) { return rows(T_MENC); }
bool sig_mdc(int i, sig_mdc_t *e) { return mdc_row(T_MENC, i, e); }
int sig_mbook_count(void) { return rows(T_MBOOK); }
bool sig_mbook(int i, sig_mdc_t *e) { return mdc_row(T_MBOOK, i, e); }

int sig_abook_count(void) { return rows(T_ABOOK); }
bool sig_abook(int i, char *num, int nn, char *name, int nm)
{
    char b[40];
    num[0] = name[0] = 0;
    if(!cell(T_ABOOK, 1, i, b, sizeof b)) { return false; }
    int k = 0;
    for(const char *p = b; *p && k < nn - 1; p++) {
        char c = up(*p);
        if((c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D')) { num[k++] = c; }
    }
    num[k] = 0;
    cell(T_ABOOK, 2, i, name, nm);
    return k > 0;
}

/* own MDC / QDC unit id: D168UV QDC1200 Decode "Self ID - Private Call" (hex); otherwise the PTT ID entry of the
 * encode list (its Private ID is the id this radio sends) */
static uint16_t own_unit(bool group)
{
#ifdef CFW_BOARD_D168UV
    char b[12];
    if(cfwcp_stock_value(T_MDEC, group ? 2 : 1, b, sizeof b) && b[0]) { uint16_t u = hex16(b); if(u) { return u; } }
#endif
    sig_mdc_t e;
    for(int i = 0, n = sig_mdc_count(); i < n; i++) { if(sig_mdc(i, &e) && e.type == MDC_T_PTTID && e.group == group) { return e.unit; } }
    return 0;
}

/* ------------------------------------------------------------------ per-channel settings */
static sig_chan_t s_ch;
static int s_ch_key = -2;
static uint32_t s_ch_gen;

static uint8_t kind_of(const char *s)
{
    if(has(s, "DTMF")) { return SIG_DTMF; }
    if(has(s, "2TONE") || has(s, "2 TONE")) { return SIG_2TONE; }
    if(has(s, "5TONE") || has(s, "5 TONE")) { return SIG_5TONE; }
    if(has(s, "MDC") || has(s, "QDC")) { return SIG_MDC; }
    return SIG_OFF;
}
static uint8_t pttid_of(const char *s)
{
    bool b = has(s, "START") || has(s, "BOT") || has(s, "BEGIN") || has(s, "PRE");
    bool e = has(s, "END") || has(s, "EOT") || has(s, "POST");
    if(has(s, "BOTH")) { b = e = true; }
    return (uint8_t)((b ? SIG_PTTID_BOT : 0) | (e ? SIG_PTTID_EOT : 0));
}

static void load_chan(int ch)
{
    memset(&s_ch, 0, sizeof s_ch);
    s_ch.dtmf_id = s_ch.tone2_id = s_ch.tone5_id = 1;           /* stock defaults: DTMF ID / 2Tone ID / 5Tone ID = 1 */
    if(ch < 0) { return; }
    char b[24];
    if(cell(T_CHAN, C_OPTSIG, ch, b, sizeof b)) { s_ch.kind = kind_of(b); }
    if(cell(T_CHAN, C_DTMFID, ch, b, sizeof b)) { s_ch.dtmf_id = (uint8_t)dec(b); }
    if(cell(T_CHAN, C_2TID, ch, b, sizeof b)) { s_ch.tone2_id = (uint8_t)dec(b); }
    if(cell(T_CHAN, C_5TID, ch, b, sizeof b)) { s_ch.tone5_id = (uint8_t)dec(b); }
    if(cell(T_CHAN, C_PTTID, ch, b, sizeof b)) { s_ch.pttid = pttid_of(b); }
    if(cell(T_CHAN, C_2TDEC, ch, b, sizeof b)) { s_ch.tone2_dec = (uint8_t)dec(b); }
    if(cell(T_CHAN, C_R5BOT, ch, b, sizeof b)) { s_ch.r5_bot = dec(b) > 0 || has(b, "ON"); }
    if(cell(T_CHAN, C_R5EOT, ch, b, sizeof b)) { s_ch.r5_eot = dec(b) > 0 || has(b, "ON"); }
    if(cell(T_CHAN, C_MDC, ch, b, sizeof b)) { s_ch.mdc_id = (uint8_t)dec(b); }
    if(s_ch.mdc_id && s_ch.kind == SIG_OFF) { s_ch.kind = SIG_MDC; }
    if(s_ch.kind == SIG_MDC && !s_ch.mdc_id) { s_ch.mdc_id = 1; }
}

static int cur_cp_channel(void)
{
    if(g_radio.vfo || !cfwcp_valid() || g_radio.channel >= channels_codeplug_count()) { return -1; }
    return radio_current()->mode == CFW_MODE_FM ? g_radio.channel : -1;
}

void sig_reload(void) { s_ch_key = -2; }

const sig_chan_t *sig_chan(void)
{
    int ch = cur_cp_channel();
    uint32_t gen = cfwcp_generation();
    if(ch != s_ch_key || gen != s_ch_gen) { s_ch_key = ch; s_ch_gen = gen; load_chan(ch); sig_rx_reset(); }
    return &s_ch;
}

/* ------------------------------------------------------------------ burst generator (48 kHz FM deviation) */
#define FS       48000u
#define NSEG     72
#define AMP_TONE 2500.0f
#define AMP_DTMF 1500.0f
#define AMP_MDC  2500.0f
typedef struct { uint16_t f1, f2, ms; } seg_t;
static seg_t s_seg[NSEG];
static int s_nseg, s_iseg;
static uint32_t s_left;
static float s_p1, s_p2, s_w1, s_w2, s_a;
static uint8_t s_mdc[MDC_PACKET2_BYTES];
static int s_mdc_bit, s_mdc_nbits, s_mdc_samp;
static uint8_t s_mdc_prev;
static volatile bool s_gen_done;
static uint32_t s_tail;
static bool s_burst;              /* a burst is loaded / playing */
static uint32_t s_done_ms;

static void seg_add(uint16_t f1, uint16_t f2, uint16_t ms)
{
    if(s_nseg < NSEG && ms) { s_seg[s_nseg].f1 = f1; s_seg[s_nseg].f2 = f2; s_seg[s_nseg].ms = ms; s_nseg++; }
}
static void seg_start(void)
{
    const seg_t *g = &s_seg[s_iseg];
    s_left = (uint32_t)g->ms * (FS / 1000u);
    s_w1 = 6.2831853f * (float)g->f1 / (float)FS;
    s_w2 = 6.2831853f * (float)g->f2 / (float)FS;
    s_a = g->f2 ? AMP_DTMF : AMP_TONE;
}

static float burst_src(void)
{
    if(s_iseg < s_nseg) {
        if(!s_left) { s_iseg++; if(s_iseg >= s_nseg) { goto mdc; } seg_start(); }
        s_left--;
        float v = 0;
        if(s_w1 > 0) { s_p1 += s_w1; if(s_p1 > 6.2831853f) { s_p1 -= 6.2831853f; } v += s_a * dsp_sinf(s_p1); }
        if(s_w2 > 0) { s_p2 += s_w2; if(s_p2 > 6.2831853f) { s_p2 -= 6.2831853f; } v += s_a * dsp_sinf(s_p2); }
        return v;
    }
mdc:
    if(s_mdc_bit < s_mdc_nbits) {                               /* 40 samples per bit at 48 kHz = 1200 bd */
        uint8_t b = (uint8_t)((s_mdc[s_mdc_bit / 8] >> (7 - s_mdc_bit % 8)) & 1u);
        float f = b != s_mdc_prev ? 1800.0f : 1200.0f;
        s_p1 += 6.2831853f * f / (float)FS;
        if(s_p1 > 6.2831853f) { s_p1 -= 6.2831853f; }
        if(++s_mdc_samp >= 40) { s_mdc_samp = 0; s_mdc_prev = b; s_mdc_bit++; }
        return AMP_MDC * dsp_sinf(s_p1);
    }
    if(s_tail) { s_tail--; return 0.0f; }                       /* silence: a late stop only cuts this */
    s_gen_done = true;
    return 0.0f;
}

static void burst_clear(void) { s_nseg = s_iseg = 0; s_mdc_nbits = s_mdc_bit = s_mdc_samp = 0; s_mdc_prev = 0; s_p1 = s_p2 = 0; }
static void burst_arm(void)
{
    s_iseg = 0;
    if(s_nseg) { seg_start(); }
    s_gen_done = false;
    s_tail = 150u * (FS / 1000u);
    s_burst = true;
    s_done_ms = 0;
}

static void add_dtmf(const char *d, bool first)                /* first = the first digit has its own length */
{
    const sig_cfg_t *g = sig_cfg();
    static const uint16_t row[4] = { 697, 770, 852, 941 }, col[4] = { 1209, 1336, 1477, 1633 };
    static const char keys[] = "123A456B789C*0#D";
    for(; *d; d++) {
        const char *p = strchr(keys, up(*d));
        if(!p || !*p) { continue; }
        if(*p == 'D' && g->dt_dpause) { seg_add(0, 0, g->dt_dpause); continue; }   /* D Code Pause */
        int k = (int)(p - keys);
        seg_add(row[k / 4], col[k % 4], first && g->dt_first > g->dt_ms ? g->dt_first : g->dt_ms);
        seg_add(0, 0, g->dt_ms);
        first = false;
    }
}
static void add_5t(const sig_5t_t *e)
{
    const uint16_t *t = sig_5t_tones(e->std);
    char prev = 0;
    for(const char *p = e->seq; *p; p++) {
        int d = hexd(*p);
        if(d < 0) { continue; }
        if(*p == prev && t[14]) { d = 14; prev = 0; } else { prev = *p; }   /* repeated digit -> repeat tone E */
        if(t[d]) { seg_add(t[d], 0, e->tone_ms); }
    }
    seg_add(0, 0, 40);
}
static void add_2t(const sig_2t_t *e)
{
    const sig_cfg_t *g = sig_cfg();
    if(!e->f2 || e->f2 == e->f1) { seg_add(e->f1, 0, g->t2_long); seg_add(0, 0, 40); return; }   /* one tone = long tone */
    seg_add(e->f1, 0, g->t2_1);
    seg_add(0, 0, g->t2_gap);
    seg_add(e->f2, 0, g->t2_2);
    seg_add(0, 0, 40);
}
static void add_mdc(uint8_t op, uint8_t arg, uint16_t unit)
{
    mdc_build(op, arg, unit, s_mdc);
    s_mdc_nbits = MDC_PACKET_BYTES * 8;
    seg_add(0, 0, 30);                                           /* tones first, the MDC packet plays after them */
}
static void add_mdc_call(uint8_t mop, uint8_t marg, uint16_t target, uint16_t own)   /* double packet */
{
    uint8_t ext[4] = { mop, marg, (uint8_t)(own >> 8), (uint8_t)own };
    mdc_build2(0x35, 0x89, target, ext, s_mdc);
    s_mdc_nbits = MDC_PACKET2_BYTES * 8;
    seg_add(0, 0, 30);
}

/* ------------------------------------------------------------------ TX control */
enum { ST_IDLE, ST_BOT, ST_MANUAL, ST_EOT, ST_LIVE };
static uint8_t s_state;
static bool s_own_stream;         /* this module started the PWM stream (no test tone running) */
static bool s_keyed;              /* this module keyed the transmitter (manual send) */
static bool s_eot_done;           /* EOT burst sent: the next sig_tx_end lets the transmitter go */
volatile uint32_t g_sig_dbg[8];   /* bursts started, bursts done, decodes, alerts, MDC syncs, DTMF digits, tones, errors */

bool sig_busy(void) { return s_state != ST_IDLE; }

static bool stream_on(const channel_t *c)
{
    dspb_fm_set_source(burst_src);
    if(voice_test_tone() && g_radio.tx) { s_own_stream = false; return true; }   /* emulator test tone stream runs */
    s_own_stream = dspb_fm_tx(true, 0, c ? c->ctcss_dhz : 0);
    if(!s_own_stream) { dspb_fm_set_source(NULL); }
    return s_own_stream;
}

/* silence before a code: the transmitter's rise time and the dialog's pretime */
static void seg_pre(uint16_t ms) { seg_add(0, 0, ms > 60 ? ms : 60); }

static uint32_t s_t5_bot_ms;      /* when the last 5Tone BOT was sent */
static bool s_t5_bot_sent;

/* a channel's PTT ID. As the manual says: DTMF / 5Tone send the BOT / EOT codes of their setting pages, 5Tone BOT / EOT
 * (R5tone columns) the channel's 5Tone encode ID. MDC1200 / QDC1200 PTT ID is the cfw's own (encode list entry). */
static bool build_pttid(const sig_chan_t *s, bool eot)
{
    const sig_cfg_t *g = sig_cfg();
    bool on = (s->pttid & (eot ? SIG_PTTID_EOT : SIG_PTTID_BOT)) != 0;
    bool r5 = eot ? s->r5_eot : s->r5_bot;
    burst_clear();
    int n0;
    switch(on ? s->kind : SIG_OFF) {
    case SIG_DTMF: seg_pre(g->dt_pre); n0 = s_nseg; add_dtmf(eot ? g->dt_eot : g->dt_bot, true);
        if(s_nseg > n0) { seg_add(0, 0, g->dt_lapse); } break;
    case SIG_5TONE: {
        const sig_5t_t *e = eot ? &g->t5_eot : &g->t5_bot;
        /* PTT ID Pause Time: no new BOT within that time of the last one (assumption) */
        if(!e->seq[0] || (!eot && g->t5_pause && s_t5_bot_sent && hal_millis() - s_t5_bot_ms < g->t5_pause * 1000u)) { break; }
        seg_pre(g->t5_pre); add_5t(e); seg_add(0, 0, g->t5_lapse);
        if(!eot) { s_t5_bot_ms = hal_millis(); s_t5_bot_sent = true; }
        break; }
    case SIG_MDC: { sig_mdc_t e; int r = row_of(T_MENC, 0, s->mdc_id);
        seg_pre(0);
        if(r >= 0 && sig_mdc(r, &e)) {
            uint16_t u = e.type == MDC_T_PTTID ? e.unit : own_unit(false);
            if(u) { add_mdc(0x01, eot ? 0x80 : 0x00, u); }
        }
        break; }
    default: break;                                              /* 2Tone: no PTT ID in the stock */
    }
    if(r5 && !(on && s->kind == SIG_5TONE && s_nseg)) {
        sig_5t_t e; int r = row_of(T_5T, 0, s->tone5_id);
        if(r >= 0 && sig_5t(r, &e)) { seg_pre(g->t5_pre); add_5t(&e); seg_add(0, 0, g->t5_lapse); }
    }
    return s_nseg > 1 || s_mdc_nbits;
}

void sig_tx_begin(const channel_t *c)
{
    s_eot_done = false;
    if(s_state == ST_MANUAL) { dspb_fm_set_source(burst_src); return; }   /* manual send (spk_stop cleared the source) */
    if(s_state != ST_IDLE) { return; }
    if(c->mode != CFW_MODE_FM) { return; }
    const sig_chan_t *s = sig_chan();
    if(!((s->pttid & SIG_PTTID_BOT) && s->kind != SIG_OFF) && !s->r5_bot) { return; }
    if(!build_pttid(s, false)) { return; }
    burst_arm();
    s_state = ST_BOT;
    dspb_fm_set_source(burst_src);                               /* radio.c starts the stream right after this */
    s_own_stream = !voice_test_tone();
    g_sig_dbg[0]++;
}

bool sig_tx_end(const channel_t *c)
{
    if(s_eot_done || c->mode != CFW_MODE_FM) { s_eot_done = false; return false; }
    if(s_state == ST_MANUAL || s_state == ST_EOT) { return true; }   /* PTT released during a send: finish it first */
    if(s_state != ST_IDLE) { s_state = ST_IDLE; dspb_fm_set_source(NULL); }
    const sig_chan_t *s = sig_chan();
    if(!((s->pttid & SIG_PTTID_EOT) && s->kind != SIG_OFF) && !s->r5_eot) { return false; }
    if(!build_pttid(s, true)) { return false; }
    burst_arm();
    if(!stream_on(c)) { s_burst = false; return false; }
    s_state = ST_EOT;
    g_sig_dbg[0]++;
    return true;
}

static const char *start_manual(void)
{
    const channel_t *c = radio_current();
    if(c->mode != CFW_MODE_FM) { return "FM channel only"; }
    if(!radio_tx_hz(c)) { return "RX only channel"; }
    if(s_state != ST_IDLE || aprs_busy()) { return "busy"; }
    if(!s_nseg && !s_mdc_nbits) { return "empty entry"; }
    burst_arm();
    g_sig_dbg[0]++;
    if(g_radio.tx) {                                             /* PTT held: send it inside this transmission */
        if(!stream_on(c)) { s_burst = false; return "no TX audio"; }
        s_state = ST_LIVE;
        s_keyed = false;
        return NULL;
    }
    s_state = ST_MANUAL;
    s_keyed = true;
    dspb_fm_set_source(burst_src);
    s_own_stream = !voice_test_tone();
    radio_ptt(true);
    if(!g_radio.tx) { s_state = ST_IDLE; s_burst = false; s_keyed = false; dspb_fm_set_source(NULL); return "TX refused"; }
    return NULL;
}

const char *sig_send(sig_kind_t kind, int entry)
{
    if(s_state != ST_IDLE) { return "busy"; }
    burst_clear();
    const sig_cfg_t *g = sig_cfg();
    seg_pre(kind == SIG_DTMF ? g->dt_pre : kind == SIG_5TONE ? g->t5_pre : 0);
    switch(kind) {
    case SIG_DTMF: { char d[SIG_SEQ_MAX + 1]; if(!sig_dtmf(entry, d, sizeof d)) { return "empty entry"; } add_dtmf(d, true);
        seg_add(0, 0, g->dt_lapse); break; }
    case SIG_2TONE: { sig_2t_t e; if(!sig_2t(entry, &e)) { return "empty entry"; } add_2t(&e); break; }
    case SIG_5TONE: { sig_5t_t e; if(!sig_5t(entry, &e)) { return "empty entry"; } add_5t(&e); seg_add(0, 0, g->t5_lapse); break; }
    case SIG_MDC: { sig_mdc_t e; if(!sig_mdc(entry, &e)) { return "empty entry"; }
        uint16_t own = own_unit(false);
        if(e.type == MDC_T_CALL || e.type == MDC_T_SELCALL) {
            if(!e.unit || !own) { return "no ID"; }
            add_mdc_call(e.op, e.arg, e.unit, own);
        } else {
            uint16_t u = (e.type == MDC_T_EMERG) ? own : e.unit;
            if(!u) { return "no ID"; }
            add_mdc(e.op, e.arg, u);
        }
        break; }
    default: return "?";
    }
    return start_manual();
}

const char *sig_send_dtmf(const char *digits)
{
    if(s_state != ST_IDLE) { return "busy"; }
    burst_clear();
    const sig_cfg_t *g = sig_cfg();
    seg_pre(g->dt_pre);
    add_dtmf(digits, true);
    seg_add(0, 0, g->dt_lapse);
    return start_manual();
}

const char *sig_send_mdc(uint8_t op, uint8_t arg, uint16_t unit)
{
    if(s_state != ST_IDLE) { return "busy"; }
    burst_clear();
    seg_add(0, 0, 60);
    add_mdc(op, arg, unit);
    return start_manual();
}

const char *sig_call_mdc(uint16_t target)                       /* call alert to an address book entry */
{
    uint16_t own = own_unit(false);
    if(s_state != ST_IDLE) { return "busy"; }
    if(!own || !target) { return "no ID"; }
    burst_clear();
    seg_add(0, 0, 60);
    add_mdc_call(0x80, 0x15, target, own);
    return start_manual();
}

void sig_dtmf_key(char d)
{
    const channel_t *c = radio_current();
    if(!g_radio.tx || c->mode != CFW_MODE_FM || s_state != ST_IDLE || aprs_busy()) { return; }
    char t[2] = { d, 0 };
    burst_clear();
    add_dtmf(t, false);
    burst_arm();
    if(!stream_on(c)) { s_burst = false; return; }
    s_state = ST_LIVE;
    g_sig_dbg[0]++;
}

/* ------------------------------------------------------------------ alert tone (speaker) */
static const uint16_t *s_al_f, *s_al_t;
static uint16_t s_alf[5], s_alt[5];
static int s_al_i, s_al_pending;
static uint32_t s_al_left, s_al_wait;
static bool s_al_on;
static float s_al_ph, s_al_w;

static float alert_src(void)
{
    while(!s_al_left) {
        if(++s_al_i >= 5) { return 0.0f; }
        s_al_left = (uint32_t)s_al_t[s_al_i] * 10u * (FS / 1000u);
        s_al_w = 6.2831853f * (float)s_al_f[s_al_i] / (float)FS;
    }
    s_al_left--;
    if(s_al_w <= 0) { return 0.0f; }
    s_al_ph += s_al_w;
    if(s_al_ph > 6.2831853f) { s_al_ph -= 6.2831853f; }
    return 1500.0f * dsp_sinf(s_al_ph);
}

static void alert_load(int n)
{
    for(int s = 0; s < 5; s++) {
        int col = ((n - 1) * 5 + s) * 2;
        char b[12];
        /* stock defaults: alerts 1-3 = n kHz / silence / n kHz / silence / n kHz, 100 x 10 ms each; 4-5 none */
        s_alf[s] = (uint16_t)(n <= 3 && !(s & 1) ? n * 1000 : 0);
        s_alt[s] = (uint16_t)(n <= 3 ? 100 : 0);
        if(cfwcp_stock_value(T_ALERT, col, b, sizeof b)) { s_alf[s] = (uint16_t)dec(b); }
        if(cfwcp_stock_value(T_ALERT, col + 1, b, sizeof b)) { s_alt[s] = (uint16_t)dec(b); }
        if(s_alt[s] > 300) { s_alt[s] = 300; }                  /* at most 3 s per step */
    }
}

void sig_alert(int n)
{
    if(n < 1 || n > 5) { return; }
    s_al_pending = n;
    s_al_wait = hal_millis();
}

static void alert_stop(void)
{
    if(!s_al_on) { return; }
    s_al_on = false;
    dspb_fm_tx(false, 0, 0);
    dspb_fm_local(false);
    dspb_fm_set_source(NULL);
    audio_tone(false);
}

static void alert_tick(uint32_t now)
{
    if(s_al_on) {
        if(s_al_i >= 5 || g_radio.tx) { alert_stop(); }
        return;
    }
    if(!s_al_pending) { return; }
    if(g_radio.tx || s_state != ST_IDLE || aprs_busy() || (g_radio.sq_open && g_radio.carrier)) {
        if(now - s_al_wait > 5000u) { s_al_pending = 0; }       /* channel never cleared: drop it */
        return;
    }
    alert_load(s_al_pending);
    s_al_pending = 0;
    uint32_t total = 0;
    for(int s = 0; s < 5; s++) { total += s_alt[s]; }
    if(!total || audio_tone_muted()) { return; }
    s_al_f = s_alf; s_al_t = s_alt; s_al_i = -1; s_al_left = 0; s_al_ph = 0;
    dspb_fm_set_source(alert_src);
    dspb_fm_local(true);
    if(!dspb_fm_tx(true, 0, 0)) { dspb_fm_set_source(NULL); dspb_fm_local(false); return; }
    audio_tone(true);
    s_al_on = true;
    g_sig_dbg[3]++;
}

/* ------------------------------------------------------------------ heard list / popup */
#ifdef CFW_BOARD_D168UV
#define NHEARD 8                  /* 224 KB RAM */
#else
#define NHEARD 16
#endif
static sig_heard_t s_sig_heard[NHEARD];
static int s_nheard;
static uint8_t s_note_seq;
static char s_note1[26], s_note2[26];

int sig_heard_count(void) { return s_nheard; }
const sig_heard_t *sig_heard(int i) { return (i >= 0 && i < s_nheard) ? &s_sig_heard[i] : NULL; }
uint8_t sig_note(const char **l1, const char **l2) { *l1 = s_note1; *l2 = s_note2; return s_note_seq; }

static const char *const k_kind[SIG_KIND_N] = { "", "DTMF", "2TONE", "5TONE", MDC_LABEL };

static void name_lookup(sig_kind_t kind, const char *text, uint16_t unit, char *name)
{
    name[0] = 0;
    if(kind == SIG_MDC) {
        sig_mdc_t e;
        for(int i = 0, n = sig_mbook_count(); i < n; i++) { if(sig_mbook(i, &e) && e.unit == unit) { copyn(name, e.name, 17); return; } }
        return;
    }
    char num[SIG_SEQ_MAX + 1], nm[17];
    for(int i = 0, n = sig_abook_count(); i < n; i++) {
        if(sig_abook(i, num, sizeof num, nm, sizeof nm) && !strcmp(num, text)) { copyn(name, nm, 17); return; }
    }
}

static const char *mdc_op_name(uint8_t op, uint8_t arg, const uint8_t *ext)
{
    if((op == 0x35 || op == 0x55) && ext) { return ext[0] == 0x81 || ext[0] == 0x83 ? "SEL CALL" : "CALL ALERT"; }
    switch(op) {
    case 0x01: return arg & 0x80 ? "PTT ID (end)" : "PTT ID";
    case 0x00: return "EMERGENCY";
    case 0x35: return "CALL";
    case 0x63: return "RADIO CHECK";
    case 0x46: return "STATUS";
    default: return "packet";
    }
}

/* DTMF call to this radio: Self ID, alone or followed by the interval character and the caller; with a group code, that
 * character stands for any digit of the Self ID (assumption: the usual format of these radios) */
static bool dtmf_to_me(const char *t, const sig_cfg_t *g)
{
    int i = 0;
    for(; g->dt_self[i]; i++) { if(t[i] != g->dt_self[i] && !(g->dt_group && t[i] == g->dt_group)) { return false; } }
    return i && (!t[i] || (g->dt_sep && t[i] == g->dt_sep));
}

void sig_decoded(sig_kind_t kind, const char *text, uint8_t op, uint8_t arg, uint16_t unit, uint16_t target, const uint8_t *ext)
{
    sig_heard_t h;
    memset(&h, 0, sizeof h);
    h.kind = (uint8_t)kind; h.op = op; h.arg = arg; h.unit = unit; h.t = hal_millis(); h.target = target;
    if(ext) { h.msg_op = ext[0]; h.msg_arg = ext[1]; }
    copyn(h.text, text, sizeof h.text);
    name_lookup(kind, text, unit, h.name);
    const sig_chan_t *s = sig_chan();
    int alert = 0;
    bool quiet = false;
    if(kind == SIG_MDC) {
        uint16_t me = own_unit(false), meg = own_unit(true);
        uint16_t tg = target ? target : op == 0x63 ? unit : 0;  /* radio check: single packet to the target */
        if((op == 0x35 || op == 0x55 || op == 0x63) && ((me && tg == me) || (meg && tg == meg))) {
            h.to_me = true; alert = tg == me ? 1 : 2;
        }
        if(op == 0x00) { alert = 1; }
    } else if(kind == SIG_2TONE) {
        h.to_me = true; alert = 3;                               /* decode of this channel's own pair = call */
    } else if(kind == SIG_5TONE) {
        const sig_cfg_t *g = sig_cfg();
        uint8_t resp = g->t5_resp, ir = 0;
        int ic = info_code(text, &ir);
        sig_5t_t e; int r = row_of(T_5T, 0, s->tone5_id);
        if(g->t5_self[0] ? !strcmp(g->t5_self, text) : (r >= 0 && sig_5t(r, &e) && !strcmp(e.seq, text))) { h.to_me = true; }
        else if(ic) { h.to_me = true; resp = ir; h.msg_arg = (uint8_t)ic; }
        if(h.to_me && resp) { alert = 3; }
        if(!h.to_me && !g->t5_any) { quiet = true; }            /* Disp AnyID off: heard list only */
    } else if(kind == SIG_DTMF) {
        const sig_cfg_t *g = sig_cfg();
        char d[SIG_SEQ_MAX + 1]; int r = row_of(T_DTMF, 0, s->dtmf_id);
        h.to_me = dtmf_to_me(text, g) || (r >= 0 && sig_dtmf(r, d, sizeof d) && !strcmp(d, text));
        if(h.to_me && g->dt_resp) { alert = 3; }
    }
    /* same packet / sequence again within 1.5 s (repeats, several bit phases): no new entry */
    if(s_nheard && s_sig_heard[0].kind == h.kind && s_sig_heard[0].op == h.op && s_sig_heard[0].arg == h.arg &&
       !strcmp(s_sig_heard[0].text, h.text) && h.t - s_sig_heard[0].t < 1500u) {
        s_sig_heard[0].t = h.t;
        return;
    }
    memmove(&s_sig_heard[1], &s_sig_heard[0], sizeof s_sig_heard[0] * (NHEARD - 1));
    s_sig_heard[0] = h;
    if(s_nheard < NHEARD) { s_nheard++; }
    g_sig_dbg[2]++;
    if(quiet) { return; }
    char *p = s_note1;
    const char *k = k_kind[kind < SIG_KIND_N ? kind : 0];
    while(*k) { *p++ = *k++; }
    if(kind == SIG_MDC) { *p++ = ' '; const char *o = mdc_op_name(op, arg, ext); while(*o && p < s_note1 + 24) { *p++ = *o++; } }
    else if(h.to_me) { const char *o = " CALL"; while(*o) { *p++ = *o++; } }
    *p = 0;
    copyn(s_note2, h.name[0] ? h.name : h.text, sizeof s_note2);
    s_note_seq++;
    radio_activity();
    if(alert) { sig_alert(alert); }
}

/* ------------------------------------------------------------------ tick */
/* emulator / test hooks: g_dbg_sig_send = (kind << 8 | entry) + 1 sends an encode list entry, g_dbg_sig_dtmf a
 * DTMF string, g_dbg_sig_alert = alert number */
volatile uint16_t g_dbg_sig_send;
volatile char g_dbg_sig_dtmf[20];
volatile uint8_t g_dbg_sig_alert;

void sig_tick(void)
{
    uint32_t now = hal_millis();
    (void)sig_chan();
    if(g_dbg_sig_send) {
        uint16_t v = (uint16_t)(g_dbg_sig_send - 1u);
        g_dbg_sig_send = 0;
        if(sig_send((sig_kind_t)(v >> 8), v & 0xFF)) { g_sig_dbg[7]++; }
    }
    if(g_dbg_sig_dtmf[0]) {
        char t[20];
        for(int i = 0; i < 19; i++) { t[i] = g_dbg_sig_dtmf[i]; }
        t[19] = 0;
        g_dbg_sig_dtmf[0] = 0;
        if(sig_send_dtmf(t)) { g_sig_dbg[7]++; }
    }
    if(g_dbg_sig_alert) { sig_alert(g_dbg_sig_alert); g_dbg_sig_alert = 0; }
    if(s_state != ST_IDLE) {
        if(!g_radio.tx && s_state != ST_EOT) {                  /* transmitter dropped (TOT, safety): abandon */
            s_state = ST_IDLE; s_burst = false; dspb_fm_set_source(NULL);
        } else if(s_gen_done) {
            if(!s_done_ms) { s_done_ms = now | 1u; }
            if(now - s_done_ms >= 150u) {                       /* <= 80 ms still in the PWM ring, then the tail */
                uint8_t st = s_state;
                s_state = ST_IDLE;
                s_burst = false;
                dspb_fm_set_source(NULL);
                if(s_own_stream && st != ST_EOT && st != ST_MANUAL) { dspb_fm_tx(false, 0, 0); }
                s_own_stream = false;
                g_sig_dbg[1]++;
                if(st == ST_EOT) { s_eot_done = true; radio_ptt(false); }
                else if(st == ST_MANUAL && s_keyed) {
                    s_keyed = false;
                    if(!key_ptt_down()) { s_eot_done = true; radio_ptt(false); }   /* no PTT ID around a manual send */
                    else if(!voice_test_tone()) { dspb_fm_tx(false, 0, 0); }       /* PTT held: voice goes on */
                }
            }
        }
    }
    alert_tick(now);
}
