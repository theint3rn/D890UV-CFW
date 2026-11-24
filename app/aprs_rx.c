/* APRS receive: Bell 202 AFSK 1200 Bd demodulator on the analog FM receive audio (dsp_backend fm_rx_poll, 8 kHz
 * samples of the MCU's own FM demodulator), NRZI + HDLC (flags, bit unstuffing, FCS), AX.25 UI frame -> heard list.
 *
 * Demodulator: mark (1200 Hz) and space (2200 Hz) correlators over one bit (sliding sum of I/Q products, 7 samples),
 * difference of the two energies low-passed; bit clock = DPLL nudged at each tone change; sample in the bit middle.
 * Runs whenever the FM receive path is on (also with the squelch closed: noise gives no valid FCS). */
#include <string.h>
#include "aprs.h"
#include "hal.h"
#include "radio.h"
#include "dsp_backend.h"

#define FS   8000
#define WIN  7                                  /* ~1 bit at 8 kHz / 1200 Bd */
#define MAXF 330                                /* AX.25 frame bytes incl. FCS */

static float s_c1[WIN], s_s1[WIN], s_c2[WIN], s_s2[WIN];
static float s_a1i, s_a1q, s_a2i, s_a2q, s_lp;
static uint32_t s_ph1, s_ph2, s_w, s_bitph;
static int s_last, s_prev_tone;
static uint8_t s_shift, s_ones, s_byte, s_nb;
static uint8_t s_buf[MAXF];
static uint16_t s_len;
static bool s_inframe;
static aprs_heard_t s_heard[APRS_HEARD_MAX];
static uint8_t s_nheard;
static uint32_t s_frames, s_bad;

static float tab(uint32_t ph)                   /* sine from the top 8 bits of a 32-bit phase */
{
    static float t[256];
    static bool init;
    if(!init) { for(int i = 0; i < 256; i++) { t[i] = dsp_sinf(6.2831853f * (float)i / 256.0f); } init = true; }
    return t[ph >> 24];
}

static uint16_t fcs(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while(n--) { c ^= *p++; for(int k = 0; k < 8; k++) { c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408u) : (uint16_t)(c >> 1); } }
    return (uint16_t)~c;
}

static void call_of(const uint8_t *a, char *o)  /* AX.25 address -> "CALL-SSID" */
{
    int n = 0;
    for(int i = 0; i < 6; i++) { char c = (char)(a[i] >> 1); if(c != ' ') { o[n++] = c; } }
    int ssid = (a[6] >> 1) & 15;
    if(ssid) { o[n++] = '-'; if(ssid >= 10) { o[n++] = '1'; } o[n++] = (char)('0' + ssid % 10); }
    o[n] = 0;
}

static void frame_done(void)
{
    if(s_len < 18) { return; }
    if(fcs(s_buf, s_len - 2u) != (uint16_t)(s_buf[s_len - 2] | s_buf[s_len - 1] << 8)) { s_bad++; return; }
    /* addresses: dest, src, up to 8 digis; the last one has bit 0 of its SSID byte set */
    size_t p = 0;
    while(p + 7 <= (size_t)s_len - 2u && !(s_buf[p + 6] & 1)) { p += 7; }
    p += 7;
    if(p < 14 || p + 2 > (size_t)s_len - 2u || s_buf[p] != 0x03 || s_buf[p + 1] != 0xF0) { s_bad++; return; }   /* UI, no L3 */
    s_frames++;
    aprs_heard_t h;
    memset(&h, 0, sizeof h);
    call_of(s_buf + 7, h.call);
    size_t n = (size_t)s_len - 2u - (p + 2u);
    if(n > sizeof h.info - 1) { n = sizeof h.info - 1; }
    for(size_t i = 0; i < n; i++) { uint8_t c = s_buf[p + 2 + i]; h.info[i] = (char)(c >= 0x20 && c < 0x7F ? c : '.'); }
    h.t = rtc_now();
    h.count = 1;
    for(int i = 0; i < s_nheard; i++) {          /* same station: update it and move it to the top */
        if(strcmp(s_heard[i].call, h.call) == 0) {
            h.count = (uint16_t)(s_heard[i].count + 1u);
            memmove(&s_heard[1], &s_heard[0], sizeof h * (size_t)i);
            s_heard[0] = h;
            radio_activity();
            return;
        }
    }
    memmove(&s_heard[1], &s_heard[0], sizeof h * (size_t)(s_nheard < APRS_HEARD_MAX ? s_nheard : APRS_HEARD_MAX - 1));
    s_heard[0] = h;
    if(s_nheard < APRS_HEARD_MAX) { s_nheard++; }
    radio_activity();
}

static void hdlc_bit(int b)
{
    s_shift = (uint8_t)((s_shift >> 1) | (b ? 0x80u : 0));
    if(s_shift == 0x7E) {                       /* flag */
        if(s_inframe && s_nb == 7) { frame_done(); }   /* flag after a whole byte (the 7 bits seen = flag's start) */
        s_inframe = true; s_len = 0; s_nb = 0; s_ones = 0; s_byte = 0;
        return;
    }
    if(!s_inframe) { return; }
    if(b) {
        if(++s_ones > 6) { s_inframe = false; return; }   /* abort */
    } else {
        if(s_ones == 5) { s_ones = 0; return; }     /* stuffed zero */
        s_ones = 0;
    }
    s_byte = (uint8_t)((s_byte >> 1) | (b ? 0x80u : 0));
    if(++s_nb == 8) {
        if(s_len < MAXF) { s_buf[s_len++] = s_byte; } else { s_inframe = false; }
        s_nb = 0;
    }
}

void aprs_rx_sample(int16_t x)
{
    static int k;
    if(!s_w) { s_w = (uint32_t)(4294967296.0 * 1200.0 / FS); }
    float v = (float)x;
    s_ph1 += (uint32_t)(4294967296.0 * 1200.0 / FS);
    s_ph2 += (uint32_t)(4294967296.0 * 2200.0 / FS);
    float c1 = v * tab(s_ph1 + 0x40000000u), q1 = v * tab(s_ph1), c2 = v * tab(s_ph2 + 0x40000000u), q2 = v * tab(s_ph2);
    s_a1i += c1 - s_c1[k]; s_a1q += q1 - s_s1[k]; s_a2i += c2 - s_c2[k]; s_a2q += q2 - s_s2[k];
    s_c1[k] = c1; s_s1[k] = q1; s_c2[k] = c2; s_s2[k] = q2;
    k = (k + 1) % WIN;
    float d = (s_a1i * s_a1i + s_a1q * s_a1q) - (s_a2i * s_a2i + s_a2q * s_a2q);
    s_lp += 0.5f * (d - s_lp);
    int tone = s_lp > 0 ? 1 : 0;                /* 1 = mark */
    if(tone != s_last) {                        /* transition: pull the bit clock towards its edge */
        s_last = tone;
        if(s_bitph < 0x80000000u) { s_bitph -= s_bitph / 3u; } else { s_bitph += (0xFFFFFFFFu - s_bitph) / 3u; }
    }
    uint32_t prev = s_bitph;
    s_bitph += s_w;
    if(prev < 0x80000000u && s_bitph >= 0x80000000u) {   /* middle of the bit */
        hdlc_bit(tone == s_prev_tone);           /* NRZI: no change = 1 */
        s_prev_tone = tone;
    }
}

int aprs_heard_count(void) { return s_nheard; }
const aprs_heard_t *aprs_heard(int i) { return i >= 0 && i < s_nheard ? &s_heard[i] : NULL; }
uint32_t aprs_rx_frames(void) { return s_frames; }
