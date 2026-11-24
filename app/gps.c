#include <string.h>
#include "gps.h"
#include "hal.h"
#include "gfx.h"
#include "aprs.h"

static volatile uint8_t s_rx[512];
static volatile uint16_t s_wr, s_rd;
static char s_line[96];
static int s_n;
static gps_fix_t s_fix;
static uint32_t s_last_ms;
static uint32_t s_beacons;
static bool s_seen;

__attribute__((weak)) void gps_uart_hw_init(void) { }

void gps_init(void) { gps_uart_hw_init(); }

void gps_uart_byte(uint8_t b)
{
    uint16_t n = (uint16_t)((s_wr + 1u) % sizeof s_rx);
    if(n != s_rd) { s_rx[s_wr] = b; s_wr = n; }
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* "ddmm.mmmm" / "dddmm.mmmm" -> degrees * 1e7 */
static int32_t nmea_deg(const char *f, int degdigits)
{
    int32_t d = 0;
    for(int i = 0; i < degdigits; i++) { if(f[i] < '0' || f[i] > '9') { return 0; } d = d * 10 + (f[i] - '0'); }
    f += degdigits;
    int64_t mm = 0, scale = 1;                                /* minutes * scale */
    bool frac = false;
    for(; *f && *f != ','; f++) {
        if(*f == '.') { frac = true; continue; }
        if(*f < '0' || *f > '9') { break; }
        mm = mm * 10 + (*f - '0');
        if(frac) { scale *= 10; }
    }
    return d * 10000000 + (int32_t)(mm * 10000000 / 60 / scale);
}

static uint32_t num(const char *f, int n)
{
    uint32_t v = 0;
    for(int i = 0; i < n && f[i] >= '0' && f[i] <= '9'; i++) { v = v * 10u + (uint32_t)(f[i] - '0'); }
    return v;
}

static const char *field(const char *s, int k)
{
    while(k-- > 0) { s = strchr(s, ','); if(!s) { return ""; } s++; }
    return s;
}

bool gps_nmea_feed(const char *l)
{
    if(l[0] != '$') { return false; }
    const char *star = strchr(l, '*');
    if(star) {                                               /* checksum */
        uint8_t c = 0;
        for(const char *p = l + 1; p < star; p++) { c ^= (uint8_t)*p; }
        if(hexv(star[1]) < 0 || hexv(star[2]) < 0 || c != (uint8_t)(hexv(star[1]) << 4 | hexv(star[2]))) { return false; }
    }
    s_last_ms = hal_millis();
    s_seen = true;
    s_fix.sentences++;
    if(strncmp(l + 3, "RMC", 3) == 0) {
        const char *t = field(l, 1), *st = field(l, 2), *la = field(l, 3), *ns = field(l, 4), *lo = field(l, 5),
                   *ew = field(l, 6), *sp = field(l, 7), *co = field(l, 8), *dt = field(l, 9);
        s_fix.valid = *st == 'A';
        if(s_fix.valid) {
            s_fix.lat_e7 = nmea_deg(la, 2) * (*ns == 'S' ? -1 : 1);
            s_fix.lon_e7 = nmea_deg(lo, 3) * (*ew == 'W' ? -1 : 1);
            s_fix.speed_kn10 = (uint16_t)(num(sp, 4) * 10u);
            s_fix.course = (uint16_t)num(co, 3);
        }
        if(t[0] >= '0' && dt[0] >= '0' && dt[5] >= '0') {
            uint32_t days = cal_days(2000 + (int)num(dt + 4, 2), (int)num(dt + 2, 2), (int)num(dt, 2));
            s_fix.utc = days * 86400u + num(t, 2) * 3600u + num(t + 2, 2) * 60u + num(t + 4, 2);
        }
        return true;
    }
    if(strncmp(l + 3, "GGA", 3) == 0) {
        s_fix.sats = (uint8_t)num(field(l, 7), 2);
        const char *a = field(l, 9);
        int32_t v = 0, sg = 1;
        if(*a == '-') { sg = -1; a++; }
        while(*a >= '0' && *a <= '9') { v = v * 10 + (*a++ - '0'); }
        s_fix.alt_m = v * sg;
        return true;
    }
    return true;
}

void gps_poll(void)
{
    while(s_rd != s_wr) {
        char c = (char)s_rx[s_rd];
        s_rd = (uint16_t)((s_rd + 1u) % sizeof s_rx);
        if(c == '$') { s_n = 0; }
        if(c == '\r' || c == '\n') {
            if(s_n) { s_line[s_n] = 0; gps_nmea_feed(s_line); s_n = 0; }
            continue;
        }
        if(s_n < (int)sizeof s_line - 1) { s_line[s_n++] = c; }
    }
}

bool gps_present(void) { return s_seen && hal_millis() - s_last_ms < 5000u; }
const gps_fix_t *gps_fix(void) { if(!gps_present()) { s_fix.valid = false; } return &s_fix; }

static void fmt_deg(char *t, int32_t e7, char pos, char neg)
{
    uint32_t a = (uint32_t)(e7 < 0 ? -e7 : e7);
    char *p = fmt_u(t, a / 10000000u, 1); *p++ = '.';
    p = fmt_u(p, (a % 10000000u) / 100u, 5); *p++ = ' '; *p++ = e7 < 0 ? neg : pos; *p = 0;
}
void gps_fmt_lat(char *t, int32_t e7) { fmt_deg(t, e7, 'N', 'S'); }
void gps_fmt_lon(char *t, int32_t e7) { fmt_deg(t, e7, 'E', 'W'); }

void gps_fmt_loc(char *t, int32_t lat, int32_t lon)
{
    int64_t lo = (int64_t)lon + 1800000000LL, la = (int64_t)lat + 900000000LL;   /* 1e7 deg */
    t[0] = (char)('A' + lo / 200000000LL); t[1] = (char)('A' + la / 100000000LL);
    lo %= 200000000LL; la %= 100000000LL;
    t[2] = (char)('0' + lo / 20000000LL); t[3] = (char)('0' + la / 10000000LL);
    lo %= 20000000LL; la %= 10000000LL;
    t[4] = (char)('a' + lo * 24 / 20000000LL); t[5] = (char)('a' + la * 24 / 10000000LL);
    t[6] = 0;
}

uint32_t gps_beacons(void) { return s_beacons; }

const char *gps_beacon_now(void)
{
    if(!gps_fix()->valid) { return "no GPS fix"; }
    const char *e = aprs_send_position(&s_fix);
    if(!e) { s_beacons++; }
    return e;
}
