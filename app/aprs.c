#include <string.h>
#include "aprs.h"
#include "radio.h"
#include "hal.h"
#include "gfx.h"
#include "dsp_backend.h"
#include "stockopt.h"

#define FS        48000u
#define SPB       (FS / 1200u)              /* 40 samples per bit */
#define PRE_FLAGS 40                        /* ~270 ms TX delay */
#define POST_FLAGS 24                       /* > 80 ms DAC ring: the source runs ahead of the PWM output */

static uint8_t s_bits[400];                 /* stuffed bit stream incl. flags, LSB first */
static uint32_t s_nbits, s_bit, s_samp;
static float s_ph;
static bool s_tone_hi, s_busy;

static void put_bit(int b) { if(s_nbits < sizeof s_bits * 8u) { if(b) { s_bits[s_nbits / 8] |= (uint8_t)(1u << (s_nbits % 8)); } s_nbits++; } }

static void put_flag(void) { for(int i = 0; i < 8; i++) { put_bit((0x7E >> i) & 1); } }

static uint16_t fcs(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while(n--) { c ^= *p++; for(int k = 0; k < 8; k++) { c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408u) : (uint16_t)(c >> 1); } }
    return (uint16_t)~c;
}

static size_t put_addr(uint8_t *o, const char *call, bool last)
{
    char c[10];
    int ssid = 0, i = 0;
    strncpy(c, call, 9); c[9] = 0;
    char *dash = strchr(c, '-');
    if(dash) { *dash = 0; for(char *q = dash + 1; *q >= '0' && *q <= '9'; q++) { ssid = ssid * 10 + (*q - '0'); } }
    for(i = 0; i < 6; i++) {
        char ch = c[i] && c[i] != ' ' && i < (int)strlen(c) ? c[i] : ' ';
        if(ch >= 'a' && ch <= 'z') { ch = (char)(ch - 32); }
        o[i] = (uint8_t)(ch << 1);
    }
    o[6] = (uint8_t)(0x60 | ((ssid & 15) << 1) | (last ? 1 : 0));
    return 7;
}

/* call + "-ssid" (ssid 0: plain call) */
static void call_ssid(char *d, const char *call, uint8_t ssid)
{
    char *p = fmt_s(d, call);
    if(ssid) { *p++ = '-'; fmt_u(p, ssid, 1); }
}

/* destination (TOCALL) and digipeater path: stock APRS table when set, else APZCFW / WIDE1-1 */
size_t aprs_build_frame(uint8_t *o, size_t max, const char *src, const char *info)
{
    const so_aprs_t *a = stockopt_aprs();
    char dst[10], path[32], *digi[3];
    int nd = 0;
    size_t n = 0, il = strlen(info);
    if(a->tocall[0]) { call_ssid(dst, a->tocall, a->tocall_ssid); } else { strcpy(dst, "APZCFW"); }
    strcpy(path, a->path[0] ? a->path : "WIDE1-1");
    for(char *p = path; *p && nd < 3; ) {           /* "WIDE1-1,WIDE2-1"; the D168UV CPS writes "WIDE1-1WIDE2-1" */
        while(*p == ' ' || *p == ',') { p++; }
        if(!*p) { break; }
        digi[nd++] = p;
        while(*p && *p != ',' && *p != ' ') {
            if(*p == '-' && p[1] >= '0' && p[1] <= '9') {          /* SSID: split before a following call */
                char *q = p + 1;
                while(*q >= '0' && *q <= '9') { q++; }
                if(*q && *q != ',' && *q != ' ') { memmove(q + 1, q, strlen(q) + 1); *q = 0; p = q + 1; goto next; }
                p = q;
                continue;
            }
            p++;
        }
        if(*p) { *p++ = 0; }
    next:;
    }
    if(max < 7 * (2 + (size_t)nd) + 2 + il + 2) { return 0; }
    n += put_addr(o + n, dst, false);
    n += put_addr(o + n, src, nd == 0);
    for(int i = 0; i < nd; i++) { n += put_addr(o + n, digi[i], i == nd - 1); }
    o[n++] = 0x03; o[n++] = 0xF0;
    memcpy(o + n, info, il); n += il;
    uint16_t f = fcs(o, n);
    o[n++] = (uint8_t)f; o[n++] = (uint8_t)(f >> 8);
    return n;
}

size_t aprs_build_info(char *out, size_t max, const gps_fix_t *f)
{
    if(max < 40) { return 0; }
    uint32_t la = (uint32_t)(f->lat_e7 < 0 ? -f->lat_e7 : f->lat_e7), lo = (uint32_t)(f->lon_e7 < 0 ? -f->lon_e7 : f->lon_e7);
    uint32_t lam = (la % 10000000u) * 60u / 100000u, lom = (lo % 10000000u) * 60u / 100000u;   /* minutes * 100 */
    char *p = out;
    *p++ = '!';
    p = fmt_u(p, la / 10000000u, 2); p = fmt_u(p, lam / 100u, 2); *p++ = '.'; p = fmt_u(p, lam % 100u, 2);
    const so_aprs_t *a = stockopt_aprs();
    *p++ = f->lat_e7 < 0 ? 'S' : 'N'; *p++ = a->sym_table ? a->sym_table : '/';
    p = fmt_u(p, lo / 10000000u, 3); p = fmt_u(p, lom / 100u, 2); *p++ = '.'; p = fmt_u(p, lom % 100u, 2);
    *p++ = f->lon_e7 < 0 ? 'W' : 'E'; *p++ = a->sym ? a->sym : '>';
    if(a->text[0]) { size_t room = max - (size_t)(p - out) - 1, k = strlen(a->text); if(k > room) { k = room; } memcpy(p, a->text, k); p += k; }
    else { p = fmt_s(p, "cfw " ); p = fmt_s(p, g_radio.my_call); }
    while(p > out && p[-1] == ' ') { p--; }
    *p = 0;
    return (size_t)(p - out);
}

/* one 48 kHz sample of FM deviation (Hz); NRZI: 0 = change tone, 1 = keep */
static float afsk_sample(void)
{
    if(s_bit >= s_nbits) { return 0.0f; }
    if(s_samp == 0) {
        int b = (s_bits[s_bit / 8] >> (s_bit % 8)) & 1;
        if(!b) { s_tone_hi = !s_tone_hi; }
    }
    s_ph += 6.2831853f * (s_tone_hi ? 2200.0f : 1200.0f) / (float)FS;
    if(s_ph > 6.2831853f) { s_ph -= 6.2831853f; }
    if(++s_samp >= SPB) { s_samp = 0; s_bit++; }
    return 3000.0f * dsp_sinf(s_ph);
}

/* stock APRS Transmission Frequency: the beacon goes out on a temporary FM simplex VFO there, then back */
static bool s_qsy, s_qsy_vfo;
static channel_t s_qsy_ch;

bool aprs_busy(void) { return s_busy; }
bool aprs_done(void) { return s_busy && s_bit >= s_nbits; }
void aprs_end(void)
{
    s_busy = false;
    dspb_fm_set_source(NULL);
    if(s_qsy) { s_qsy = false; g_radio.vfo_ch = s_qsy_ch; g_radio.vfo = s_qsy_vfo; radio_apply(); }
}

const char *aprs_send_position(const gps_fix_t *f)
{
    if(g_radio.tx || s_busy) { return "busy"; }
    const so_aprs_t *a = stockopt_aprs();
    if(a->tx_hz) {
        if(!freq_in_band(a->tx_hz)) { return "APRS freq out of band"; }
        s_qsy_ch = g_radio.vfo_ch; s_qsy_vfo = g_radio.vfo; s_qsy = true;
        channel_t q = g_radio.vfo_ch;
        q.rx_hz = q.tx_hz = a->tx_hz; q.mode = CFW_MODE_FM; q.wide = true; q.ctcss_dhz = 0; q.talkaround = false;
        g_radio.vfo_ch = q; g_radio.vfo = true;
        radio_apply();
    }
    const channel_t *c = radio_current();
    if(c->mode != CFW_MODE_FM) { aprs_end(); return "FM channel only"; }
    if(!radio_tx_hz(c)) { aprs_end(); return "RX only channel"; }
    char info[64];
    uint8_t fr[128];
    aprs_build_info(info, sizeof info, f);
    char src[12];
    if(a->call[0]) { call_ssid(src, a->call, a->ssid); }            /* stock APRS Your Call Sign / SSID */
    else {
        memcpy(src, g_radio.my_call, 8); src[8] = 0;
        for(int i = 7; i >= 0 && src[i] == ' '; i--) { src[i] = 0; }
        if(a->ssid) { call_ssid(src, src, a->ssid); }
    }
    size_t n = aprs_build_frame(fr, sizeof fr, src, info);
    memset(s_bits, 0, sizeof s_bits);
    s_nbits = 0;
    for(int i = 0; i < PRE_FLAGS; i++) { put_flag(); }
    int ones = 0;
    for(size_t i = 0; i < n; i++) {
        for(int k = 0; k < 8; k++) {
            int b = (fr[i] >> k) & 1;
            put_bit(b);
            if(b) { if(++ones == 5) { put_bit(0); ones = 0; } } else { ones = 0; }
        }
    }
    for(int i = 0; i < POST_FLAGS; i++) { put_flag(); }
    s_bit = s_samp = 0; s_ph = 0; s_tone_hi = false;
    s_busy = true;
    dspb_fm_set_source(afsk_sample);
    radio_ptt(true);
    if(!g_radio.tx) { aprs_end(); return "TX refused"; }
    return NULL;
}
