/* SCT3288 hardware vocoder backend (packet API from the baseband-protocol session, cfw/docs/SCT3288_VOCODER.md):
 *   control  B0 0x80: 30 PRODID, 31 VERSTRING, 33 RESET, 09 <33|34> RATET, 0A <12 bytes> RATEP; replies echo <id> 00
 *   encode   B0 0x82 ID 0x00 {0xA0, 160 x s16 big endian}  ->  B0 0x81 ID 0x01 {nbits, packed bits MSB first}
 *   decode   B0 0x81 ID 0x01 {nbits, bits}                 ->  B0 0x82 ID 0x00 {0xA0, 320 bytes PCM}
 * Rates: 34 = AMBE+2 3600x2450 without FEC (49 bits; the cfw proto glue adds the protocol FEC itself), D-STAR AMBE
 * 3600x2400 via RATEP (72 bits incl. its FEC). Synchronous: each call waits (bb_poll) for the reply, up to 60 ms.
 * UNVERIFIED on a real chip (mode switch / HPI mode may be needed first; doc section 6). In the emulator Baseband.cs
 * answers with a deterministic tone stand-in ('F', freq/8, amp/128): proves the link and framing, not audio quality. */
#include <string.h>
#include "voc_sct.h"
#include "hal.h"

static volatile bool s_got;
static volatile uint8_t s_want_b0, s_want_id;
static uint8_t s_reply[BB_MAX_DATA];
static volatile uint16_t s_reply_len;
static int s_rate;
static int8_t s_avail = -1;                       /* -1 unknown, 0 no, 1 yes */
static char s_prodid[24];

static sct_async_cb s_acb;
static volatile bool s_async_busy;
static uint32_t s_async_t0;

static void listener(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len)
{
    if(s_async_busy && b0 == s_want_b0 && id == s_want_id) {          /* asynchronous request finished */
        s_async_busy = false;
        if(s_acb) { s_acb(b0, d, len); }
        return;
    }
    if(s_got || b0 != s_want_b0 || id != s_want_id) { return; }
    s_reply_len = len > sizeof s_reply ? (uint16_t)sizeof s_reply : len;
    memcpy(s_reply, d, s_reply_len);
    s_got = true;
}

static bool request(uint8_t b0, uint8_t id, const uint8_t *data, uint16_t len, uint8_t rb0, uint8_t rid)
{
    bb_set_listener(listener);
    s_got = false;
    s_want_b0 = rb0;
    s_want_id = rid;
    if(!bb_send(b0, id, data, len)) { return false; }
    uint32_t t0 = hal_millis();
    while(!s_got && hal_millis() - t0 < 60u) { bb_poll(); }
    return s_got;
}

bool sct_voc_available(void)
{
    if(s_avail >= 0) { return s_avail == 1; }
    if(!bb_ready()) { return false; }               /* ask again once the baseband is up */
    s_avail = request(0x80, 0x30, NULL, 0, 0x80, 0x30) ? 1 : 0;
    if(s_avail) {
        uint16_t n = s_reply_len < sizeof s_prodid - 1 ? s_reply_len : (uint16_t)(sizeof s_prodid - 1);
        memcpy(s_prodid, s_reply, n);
        s_prodid[n] = 0;
    }
    return s_avail == 1;
}

const char *sct_voc_prodid(void) { return s_prodid; }

static bool set_rate(int rate)
{
    if(rate == s_rate) { return true; }
    bool ok;
    if(rate == SCT_RATE_DSTAR) {
        static const uint8_t ratep[12] = { 0x01, 0x30, 0x07, 0x63, 0x40, 0, 0, 0, 0, 0, 0, 0x48 };
        ok = request(0x80, 0x0A, ratep, sizeof ratep, 0x80, 0x0A);
    } else {
        uint8_t r = (uint8_t)rate;
        ok = request(0x80, 0x09, &r, 1, 0x80, 0x09);
    }
    if(ok && s_reply_len >= 1 && s_reply[0] == 0) { s_rate = rate; return true; }
    return false;
}

bool sct_voc_encode(int rate, const int16_t pcm[160], uint8_t *bits, int *nbits)
{
    if(!sct_voc_available() || !set_rate(rate)) { return false; }
    static uint8_t d[1 + 320];
    d[0] = 0xA0;
    for(int i = 0; i < 160; i++) { d[1 + 2 * i] = (uint8_t)((uint16_t)pcm[i] >> 8); d[2 + 2 * i] = (uint8_t)pcm[i]; }
    if(!request(0x82, 0x00, d, sizeof d, 0x81, 0x01) || s_reply_len < 2) { return false; }
    int nb = s_reply[0], nbytes = (nb + 7) / 8;
    if(nbytes > s_reply_len - 1 || nbytes > 9) { return false; }
    memcpy(bits, s_reply + 1, (size_t)nbytes);
    *nbits = nb;
    return true;
}

bool sct_voc_decode(int rate, const uint8_t *bits, int nbits, int16_t pcm[160])
{
    if(!sct_voc_available() || !set_rate(rate)) { return false; }
    uint8_t d[1 + 9];
    int nbytes = (nbits + 7) / 8;
    if(nbytes > 9) { return false; }
    d[0] = (uint8_t)nbits;
    memcpy(d + 1, bits, (size_t)nbytes);
    if(!request(0x81, 0x01, d, (uint16_t)(1 + nbytes), 0x82, 0x00) || s_reply_len < 321 || s_reply[0] != 0xA0) {
        return false;
    }
    for(int i = 0; i < 160; i++) { pcm[i] = (int16_t)((s_reply[1 + 2 * i] << 8) | s_reply[2 + 2 * i]); }
    return true;
}

/* ---- asynchronous requests (one in flight): the reply arrives through bb_poll() -> listener -> cb ---- */
bool sct_async_busy(void)
{
    if(s_async_busy && hal_millis() - s_async_t0 > 100u) { s_async_busy = false; }   /* lost reply */
    return s_async_busy;
}

static bool async_send(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len, uint8_t rb0, uint8_t rid, sct_async_cb cb)
{
    bb_set_listener(listener);
    s_acb = cb;
    s_want_b0 = rb0; s_want_id = rid;
    s_async_busy = true;
    s_async_t0 = hal_millis();
    if(!bb_send(b0, id, d, len)) { s_async_busy = false; return false; }
    return true;
}

bool sct_async_encode(int rate, const int16_t pcm[160], sct_async_cb cb)
{
    if(sct_async_busy() || !sct_voc_available() || !set_rate(rate)) { return false; }
    static uint8_t d[1 + 320];
    d[0] = 0xA0;
    for(int i = 0; i < 160; i++) { d[1 + 2 * i] = (uint8_t)((uint16_t)pcm[i] >> 8); d[2 + 2 * i] = (uint8_t)pcm[i]; }
    return async_send(0x82, 0x00, d, sizeof d, 0x81, 0x01, cb);
}

bool sct_async_decode(int rate, const uint8_t *bits, int nbits, sct_async_cb cb)
{
    if(sct_async_busy() || !sct_voc_available() || !set_rate(rate)) { return false; }
    uint8_t d[1 + 9];
    int nbytes = (nbits + 7) / 8;
    if(nbytes > 9) { return false; }
    d[0] = (uint8_t)nbits;
    memcpy(d + 1, bits, (size_t)nbytes);
    return async_send(0x81, 0x01, d, (uint16_t)(1 + nbytes), 0x82, 0x00, cb);
}
