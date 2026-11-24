#include <string.h>
#include "scan.h"
#include "radio.h"
#include "rset.h"
#include "cfwcp.h"
#include "stockopt.h"
#include "channels.h"
#include "hal.h"

#define HOP_FM_MS      150u          /* RF settle + carrier check */
#define HOP_DIG_MS     250u
#define HOLD_DEFAULT   3000u
#define MAX_DEL        16
enum { ST_HOP, ST_LISTEN, ST_HOLD };

static struct {
    bool on;
    int list, pos, home, prio, since_prio;
    bool vfo; uint8_t type; uint32_t lo, hi, start;     /* VFO scan: stock VFO Scan Type / start, end frequencies */
    uint8_t st;
    uint32_t t;
    uint16_t del[MAX_DEL];
    int ndel;
} s;

static int members(void)
{
    if(s.list >= 0) { const cp_scanlist_t *l = cfwcp_scanlist(s.list); return l ? l->count : 0; }
    return zone_size(g_radio.zone);
}

static int member(int k)
{
    if(s.list >= 0) {
        const cp_scanlist_t *l = cfwcp_scanlist(s.list);
        if(!l) { return -1; }
        uint16_t ch = cfwcp_member(l->first, k);
        return ch < channels_count() ? ch : -1;
    }
    return zone_channel(g_radio.zone, k);
}

static bool deleted(int ch)
{
    for(int i = 0; i < s.ndel; i++) { if(s.del[i] == ch) { return true; } }
    return false;
}

static bool usable(int ch)
{
    if(ch < 0 || deleted(ch)) { return false; }
    channel_t c;
    if(!channel_load(ch, &c)) { return false; }
    return !c.scan_skip;
}

static uint32_t hop_ms(void) { return radio_current()->mode == CFW_MODE_FM ? HOP_FM_MS : HOP_DIG_MS; }

static uint32_t hold_ms(void)
{
    const cp_scanlist_t *l = s.list >= 0 ? cfwcp_scanlist(s.list) : NULL;
    return (l && l->dwell) ? l->dwell * 100u : HOLD_DEFAULT;
}

static void go(int ch)
{
    radio_goto(ch);
    s.t = hal_millis();
    s.st = ST_HOP;
}

static void next(void)
{
    int n = members();
    if(n <= 0) { return; }
    if(s.prio >= 0 && s.since_prio >= 4 && g_radio.channel != s.prio && !deleted(s.prio)) {
        s.since_prio = 0;
        go(s.prio);
        return;
    }
    for(int tries = 0; tries < n; tries++) {
        s.pos = (s.pos + 1) % n;
        int ch = member(s.pos);
        if(usable(ch)) { s.since_prio++; go(ch); return; }
    }
}

const char *scan_name(int list)
{
    static char n[17];
    const cp_scanlist_t *l = cfwcp_scanlist(list);
    if(!l) { return "Zone"; }
    int i = 0;
    for(; i < 16 && l->name[i]; i++) { n[i] = (l->name[i] >= 0x20 && l->name[i] < 0x7F) ? l->name[i] : '?'; }
    n[i] = 0;
    return n;
}

/* VFO scan: range of the band the VFO is in (VFO Scan Start / End Freq, MHz; unset = the whole band) */
static void vfo_start(void)
{
    char b[16];
    uint32_t f = g_radio.vfo_ch.rx_hz;
    bool uhf = f >= 300000000u;
    memset(&s, 0, sizeof s);
    s.lo = uhf ? 400000000u : 136000000u; s.hi = uhf ? 480000000u : 174000000u;
    if(stockopt_opt_text(uhf ? 72 : 74, b, sizeof b)) { uint32_t v = stockopt_mhz(b); if(v >= s.lo && v <= s.hi) { s.lo = v; } }
    if(stockopt_opt_text(uhf ? 73 : 75, b, sizeof b)) { uint32_t v = stockopt_mhz(b); if(v > s.lo && v <= s.hi) { s.hi = v; } }
    s.vfo = s.on = true;
    s.list = -1; s.prio = -1;
    s.home = g_radio.channel;
    s.type = (uint8_t)stockopt_opt(14, 0);            /* 0 TO, 1 CO, 2 SE */
    s.start = f;
    s.t = hal_millis();
    s.st = ST_HOP;
}

static void vfo_next(void)
{
    uint32_t f = g_radio.vfo_ch.rx_hz + g_radio.step_hz;
    if(f > s.hi || f < s.lo) { f = s.lo; }
    radio_set_vfo_freq(f);
    s.t = hal_millis();
    s.st = ST_HOP;
}

void scan_start(int list)
{
    if(g_radio.vfo) { vfo_start(); return; }
    if(list >= cfwcp_scanlist_count()) { list = -1; }
    memset(&s, 0, sizeof s);
    s.list = list;
    s.home = g_radio.channel;
    s.prio = -1;
    const cp_scanlist_t *l = list >= 0 ? cfwcp_scanlist(list) : NULL;
    if(l && l->pri1 < channels_count()) { s.prio = l->pri1; }
    if(members() <= 0) { return; }
    g_radio.vfo = false;
    s.on = true;
    s.pos = -1;
    next();
}

void scan_stop(bool stay)
{
    if(!s.on) { return; }
    s.on = false;
    if(s.vfo) { if(!stay) { radio_set_vfo_freq(s.start); } return; }
    if(!stay) { radio_goto(s.home); }
}

bool scan_active(void) { return s.on; }
bool scan_holding(void) { return s.on && s.st != ST_HOP; }
int  scan_list(void) { return s.list; }
int  scan_deleted(void) { return s.ndel; }

void scan_nuisance(void)
{
    if(!s.on) { return; }
    if(s.vfo) { vfo_next(); return; }
    if(s.ndel < MAX_DEL && !deleted(g_radio.channel)) { s.del[s.ndel++] = (uint16_t)g_radio.channel; }
    next();
}

void scan_tick(void)
{
    if(!s.on || g_radio.tx) { return; }
    uint32_t now = hal_millis();
    bool busy = rf_carrier(0) && (!rset_sql_level() || rf_rssi(0) >= rf_sql_thr());   /* stock squelch threshold */
    if(s.vfo) {                                       /* SCM TO: 5 s per signal; CO: until it is gone; SE: stop */
        if(s.st == ST_HOP) {
            if(now - s.t < hop_ms()) { return; }
            if(!busy) { vfo_next(); return; }
            if(s.type == 2) { scan_stop(true); return; }
            s.st = ST_LISTEN; s.t = now;
        } else if(s.st == ST_LISTEN) {
            if(s.type == 0 && now - s.t >= 5000u) { vfo_next(); }
            else if(!busy) { s.st = ST_HOLD; s.t = now; }
        } else {
            if(busy) { s.st = ST_LISTEN; if(s.type != 0) { s.t = now; } }
            else if(now - s.t >= 2000u) { vfo_next(); }
        }
        return;
    }
    switch(s.st) {
    case ST_HOP:
        if(now - s.t < hop_ms()) { return; }
        if(busy) { s.st = ST_LISTEN; s.t = now; return; }
        next();
        break;
    case ST_LISTEN:
        if(!busy) { s.st = ST_HOLD; s.t = now; }
        break;
    case ST_HOLD:
        if(busy) { s.st = ST_LISTEN; s.t = now; return; }
        if(now - s.t >= hold_ms()) { next(); }
        break;
    }
}
