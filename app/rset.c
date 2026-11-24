/* Radio menu settings <-> codeplug (see rset.h). */
#include <string.h>
#include "rset.h"
#include "hal.h"
#include "gfx.h"
#include "stockopt.h"
#include "stockdlg.h"

/* board key names for the six key slots (D168UV: PF1/PF2 post KEY_P1/KEY_P2, no PF3) */
#ifndef CFW_KEY_SLOT_NAMES
#define CFW_KEY_SLOT_NAMES "P1", "P2", "PF1", "PF2", "PF3", "PF3 long"
#endif

static const char *const k_sql[] = { "Off", "1", "2", "3", "4", "5" };
static const char *const k_tot[] = { "Off", "30s", "60s", "90s", "120s", "150s", "180s", "210s", "240s" };
static const char *const k_vox[] = { "Off", "1", "2", "3" };
static const char *const k_onoff[] = { "Off", "On" };
static const char *const k_vol[] = { "Indoor", "1", "2", "3", "4", "5", "6", "7", "8" };
static const char *const k_bkl[] = { "1", "2", "3", "4", "5" };
static const char *const k_bkt[] = { "Always", "5s", "10s", "15s", "20s", "25s", "30s", "1m", "2m", "3m", "4m", "5m",
                                     "15m", "30m", "45m", "1h" };
static const char *const k_ps[] = { "Off", "1:1", "2:1" };
static const char *const k_apo[] = { "Off", "10m", "30m", "60m", "120m" };
static const char *const k_lock[] = { "Manual", "Auto" };

static const rs_def_t k_defs[RS_COUNT] = {
    [RS_SQL]      = { "Squelch",       9, 6, k_sql, "FM: mute below level" },
    [RS_TOT]      = { "TX timeout",    4, 9, k_tot, "PTT released after" },
    [RS_VOX]      = { "VOX",          12, 4, k_vox, "hands-free TX (radio detector)" },
    [RS_BEEP]     = { "Key beep",      0, 2, k_onoff, "tone on each key press" },
    [RS_MAXVOL]   = { "Volume limit", 56, 9, k_vol, "caps the speaker (as stock)" },
    [RS_BKL]      = { "Backlight",    32, 5, k_bkl, "LCD backlight level" },
    [RS_BKT]      = { "Light time",   33, 16, k_bkt, "backlight off after" },
    [RS_PSAVE]    = { "Power save",   11, 3, k_ps, "RX duty cycle when idle" },
    [RS_APO]      = { "Auto pwr off",  3, 5, k_apo, "off after no activity" },
    [RS_KEYLOCK]  = { "Key lock",      2, 2, k_lock, "Auto: lock after 10s" },
    [RS_TIMEDISP] = { "Clock",        71, 2, k_onoff, "time in the status bar" },
};
static const uint32_t k_tot_s[] = { 0, 30, 60, 90, 120, 150, 180, 210, 240 };
static const uint32_t k_apo_s[] = { 0, 600, 1800, 3600, 7200 };
static const uint32_t k_bkt_s[] = { 0, 5, 10, 15, 20, 25, 30, 60, 120, 180, 240, 300, 900, 1800, 2700, 3600 };
static const char *const k_kf[KF_COUNT] = { "None", "Next mode", "Mode list", "VFO/MEM", "Key lock", "Monitor", "Scan",
                                            "Test tone", "Next zone", "Talkaround", "TX power", "Call log", "GPS info", "Alarm" };
static const char *const k_slot[RK_COUNT] = { CFW_KEY_SLOT_NAMES };
static const uint8_t k_key_default[RK_COUNT] = { KF_MODE_NEXT, KF_VFO_MEM, KF_NONE, KF_NONE, KF_MODE_NEXT, KF_MODE_PICKER };

#define MAX_EDITS 32
static uint8_t s_val[RS_COUNT];
static uint8_t s_key[RK_COUNT];
static cp_edit_t s_ed[MAX_EDITS];
static int s_ned;
static bool s_dirty, s_journal, s_rs_enc, s_rs_msms, s_rs_batt;

static uint32_t dec(const char *t)
{
    uint32_t v = 0;
    while(*t >= '0' && *t <= '9') { v = v * 10u + (uint32_t)(*t++ - '0'); }
    return v;
}

static void upsert(uint8_t table, uint8_t col, uint16_t row, uint32_t v)
{
    int i;
    for(i = 0; i < s_ned; i++) { if(s_ed[i].table == table && s_ed[i].col == col && s_ed[i].row == row) { break; } }
    if(i == s_ned) {
        if(s_ned == MAX_EDITS) { memmove(&s_ed[0], &s_ed[1], sizeof s_ed[0] * (MAX_EDITS - 1)); s_ned--; i = s_ned; }
        s_ned++;
    }
    s_ed[i].table = table; s_ed[i].col = col; s_ed[i].row = row;
    fmt_u(s_ed[i].text, v, 1);
    s_dirty = true;
}

static void apply_edits(void)
{
    for(int i = 0; i < s_ned; i++) {
        const cp_edit_t *e = &s_ed[i];
        uint32_t v = dec(e->text);
        if(e->table == CP_JT_OPTIONAL) {
            for(int k = 0; k < RS_COUNT; k++) { if(k_defs[k].col == e->col && v < k_defs[k].n) { s_val[k] = (uint8_t)v; } }
        } else if(e->table == CP_JT_SETTINGS && e->col < RK_COUNT && v < KF_COUNT) {
            s_key[e->col] = (uint8_t)v;
        } else if(e->table == CP_JT_SETTINGS && e->col == CP_SET_ENCRYPTION) {
            s_rs_enc = v == 1;
        } else if(e->table == CP_JT_SETTINGS && e->col == CP_SET_SMS_FORMAT) {
            s_rs_msms = v == 1;
        } else if(e->table == CP_JT_SETTINGS && e->col == CP_SET_BATT_PCT) {
            s_rs_batt = v == 1;
        }
    }
}

void rset_init(void)
{
    s_journal = cfwcp_journal_spare() > 0;
    for(int k = 0; k < RS_COUNT; k++) {
        char b[8];
        uint32_t v = cfwcp_stock_value(CP_JT_OPTIONAL, k_defs[k].col, b, sizeof b) ? dec(b)      /* journal included */
                   : k == RS_MAXVOL ? 8u : 0u;                                     /* no codeplug: full volume */
        s_val[k] = v < k_defs[k].n ? (uint8_t)v : 0;
    }
    const cp_settings_t *st = cfwcp_settings();
    for(int k = 0; k < RK_COUNT; k++) {
        s_key[k] = st ? (st->keys[k] < KF_COUNT ? st->keys[k] : KF_NONE) : k_key_default[k];
    }
    s_rs_enc = st && (st->flags & 16u);
    s_rs_msms = st && (st->flags & 32u);
    s_rs_batt = st && (st->flags & 64u);
    s_ned = 0;
    if(s_journal) {
        s_ned = cfwcp_journal_read(s_ed, MAX_EDITS);
        apply_edits();
    }
    s_dirty = false;
    stockopt_init();                         /* stock common sections (app/stockopt.c) */
    stockdlg_init();                         /* stock .rdt-only dialogs (app/stockdlg.c) */
}

/* settings blob edits (no journal): applied on top of the codeplug / defaults */
void rset_set_edits(const cp_edit_t *list, int n)
{
    if(s_journal) { return; }
    s_ned = 0;
    for(int i = 0; i < n && i < MAX_EDITS; i++) { s_ed[s_ned] = list[i]; s_ed[s_ned].text[7] = 0; s_ned++; }
    apply_edits();
    s_dirty = false;
}

int rset_edits(const cp_edit_t **list) { *list = s_ed; return s_ned; }
bool rset_in_codeplug(void) { return s_journal; }
bool rset_dirty(void) { return s_dirty; }

bool rset_save(void)
{
    if(!s_dirty) { return true; }
    if(!s_journal) { return false; }
    if(cfwcp_journal_write(s_ed, s_ned)) { s_dirty = false; return true; }
    return false;
}

const rs_def_t *rset_def(rs_id_t id) { return &k_defs[id]; }
uint8_t rset_get(rs_id_t id) { return s_val[id]; }
const char *rset_text(rs_id_t id) { return k_defs[id].opt[s_val[id] % k_defs[id].n]; }

void rset_set(rs_id_t id, uint8_t v)
{
    if(id >= RS_COUNT || v >= k_defs[id].n || v == s_val[id]) { return; }
    s_val[id] = v;
    upsert(CP_JT_OPTIONAL, k_defs[id].col, 0, v);
}

/* any stock Optional Settings column edited in the radio (Menu > Radio extras > Options, app/ui_stock.c) */
void rset_set_opt(uint8_t col, uint32_t v) { upsert(CP_JT_OPTIONAL, col, 0, v); }
bool rset_opt_pending(uint8_t col, uint32_t *v)
{
    bool f = false;
    for(int i = 0; i < s_ned; i++) {
        if(s_ed[i].table == CP_JT_OPTIONAL && s_ed[i].col == col) { *v = dec(s_ed[i].text); f = true; }
    }
    return f;
}

const char *rset_key_name(uint8_t fn) { return fn < KF_COUNT ? k_kf[fn] : "?"; }
const char *rset_key_slot_name(int slot) { return slot >= 0 && slot < RK_COUNT ? k_slot[slot] : NULL; }
uint8_t rset_key(int slot) { return slot >= 0 && slot < RK_COUNT ? s_key[slot] : KF_NONE; }

void rset_set_key(int slot, uint8_t fn)
{
    if(slot < 0 || slot >= RK_COUNT || fn >= KF_COUNT || s_key[slot] == fn) { return; }
    s_key[slot] = fn;
    upsert(CP_JT_SETTINGS, (uint8_t)(CP_SET_KEY_P1 + slot), 0, fn);
}

bool rset_channel_power(int ch, uint8_t *power)
{
    for(int i = 0; i < s_ned; i++) {
        if(s_ed[i].table == CP_JT_CHANNEL && s_ed[i].col == CP_CH_POWER && s_ed[i].row == ch) {
            *power = (uint8_t)(dec(s_ed[i].text) & 3u);
            return true;
        }
    }
    return false;
}

void rset_set_channel_power(int ch, uint8_t power)
{
    uint8_t cur;
    if(rset_channel_power(ch, &cur) && cur == power) { return; }
    upsert(CP_JT_CHANNEL, CP_CH_POWER, (uint16_t)ch, power & 3u);
}

/* encryption (privacy keys): cfw settings flag b4, radio edit = journal 0xE0 col 6 */
bool rset_encryption(void) { return s_rs_enc; }
void rset_set_encryption(bool on)
{
    if(on == s_rs_enc) { return; }
    s_rs_enc = on;
    upsert(CP_JT_SETTINGS, CP_SET_ENCRYPTION, 0, on ? 1u : 0u);
}

/* SMS TX format: cfw (default) or M-SMS (Motorola-style TMS over IP), flag b5, journal 0xE0 col 7 */
bool rset_sms_mformat(void) { return s_rs_msms; }
void rset_set_sms_mformat(bool on)
{
    if(on == s_rs_msms) { return; }
    s_rs_msms = on;
    upsert(CP_JT_SETTINGS, CP_SET_SMS_FORMAT, 0, on ? 1u : 0u);
}

/* battery in the status bar: icon (default) or percent (app/battdisp.c), flag b6, journal 0xE0 col 10 */
bool rset_batt_percent(void) { return s_rs_batt; }
void rset_set_batt_percent(bool on)
{
    if(on == s_rs_batt) { return; }
    s_rs_batt = on;
    upsert(CP_JT_SETTINGS, CP_SET_BATT_PCT, 0, on ? 1u : 0u);
}

/* Volume limit -> gain for the audio the MCU makes (decoded voice PCM, beeps), Q8: Indoor 1/16, level n = n/8 */
uint16_t rset_volume_q8(void) { uint8_t v = s_val[RS_MAXVOL]; return v == 0 ? 16u : (uint16_t)(v * 32u); }
/* VOX level 1..3 -> mic level threshold (mean |deviation| in 12-bit ADC counts; 1 = most sensitive) */

/* stock Optional Settings read straight from the codeplug (no radio menu item; stock CSV column = setting) */
static uint32_t stock_raw(int col, uint32_t dflt)
{
    char b[8];
    return cfwcp_stock_value(CP_JT_OPTIONAL, col, b, sizeof b) ? dec(b) : dflt;
}
bool rset_gps_on(void) { return stock_raw(34, 0) != 0; }                  /* Gps (settings +0x22) */
bool rset_mandown(void) { return stock_raw(30, 0) != 0; }                 /* FailAlarm (settings +0x24) */
uint8_t rset_mandown_wait(void) { uint32_t v = stock_raw(69, 0); return v > 255u ? 255u : (uint8_t)v; }   /* ManDownWait +0x4F, s */

uint32_t rset_tot_s(void) { return k_tot_s[s_val[RS_TOT]]; }
uint32_t rset_apo_s(void) { return k_apo_s[s_val[RS_APO]]; }
uint32_t rset_backlight_s(void) { return k_bkt_s[s_val[RS_BKT]]; }
uint8_t rset_sql_level(void) { return s_val[RS_SQL]; }
