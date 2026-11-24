/*
 * dstar_rx.c - D-STAR DV receiver state machine.
 *
 * SEARCH  -> bit sync tail + 15-bit frame sync  -> HEADER (660 bits)
 *         -> 24-bit data sync (late entry)      -> VOICE
 * VOICE   -> 96-bit frames (72 AMBE + 24 data), data sync every 21 frames,
 *            48-bit end pattern -> END, 3 missed syncs -> LOST.
 *
 * Slow data (JARL STD6.0 ch.6): two data frames form a 6-byte block whose
 * first byte is the mini header; the 3 bytes of every non-sync data frame are
 * XORed with 70 4F 93 (the x^7+x^4+1 scrambler restarted per frame).
 * DV fast data (ch.7): mini headers 0x8X/0x9X, payload also in the voice
 * frames (4 data + 0x02 + 4 data per voice frame), voice area scrambled.
 */
#include <string.h>

#include "../common/dv_common.h"
#include "dstar.h"

/* last 16 bits of "1010" bit sync + 15-bit frame sync, 31 bits */
#define HDR_PATTERN  ((0xAAAAull << 15) | DSTAR_FSYNC)
#define HDR_MASK     ((1ull << 31) - 1u)
#define DSYNC_MASK   0xFFFFFFull
#define END_MASK     ((1ull << 48) - 1u)

static void emit(dstar_rx_t *rx, const dstar_event_t *ev)
{
    if (rx->cb && rx->cb->event)
        rx->cb->event(rx->user, ev);
}

static void emit_voice(dstar_rx_t *rx, const uint8_t *ambe, uint8_t seq)
{
    if (rx->cb && rx->cb->voice)
        rx->cb->voice(rx->user, ambe, seq);
}

static void slow_reset(dstar_rx_t *rx)
{
    rx->text_mask = 0;
    rx->hdr_len = 0;
    rx->line_len = 0;
    rx->fast_active = 0;
    rx->held_valid = 0;
}

void dstar_rx_init(dstar_rx_t *rx, const dstar_rx_cb_t *cb, void *user)
{
    memset(rx, 0, sizeof(*rx));
    rx->cb = cb;
    rx->user = user;
    rx->max_sync_err = 2;
    rx->max_dsync_err = 4;
    rx->max_end_err = 4;
    dstar_rx_reset(rx);
}

void dstar_rx_reset(dstar_rx_t *rx)
{
    rx->state = DSTAR_RX_SEARCH;
    rx->nbits = 0;
    rx->seq = 0;
    rx->sync_miss = 0;
    rx->have_header = 0;
    rx->last_text[0] = '\0';
    slow_reset(rx);
}

/* x^7+x^4+1 sequence for one 72-bit voice frame, packed LSB first. */
static void voice_pn(uint8_t pn[DSTAR_VOICE_BYTES])
{
    uint8_t b[72];
    memset(b, 0, sizeof(b));
    dstar_scramble(b, 72);
    dv_pack_lsb(b, 72, pn);
}

static void fast_voice(dstar_rx_t *rx, const uint8_t v[DSTAR_VOICE_BYTES], unsigned at)
{
    uint8_t pn[DSTAR_VOICE_BYTES], d[DSTAR_VOICE_BYTES];
    voice_pn(pn);
    for (unsigned i = 0; i < DSTAR_VOICE_BYTES; i++)
        d[i] = v[i] ^ pn[i];
    memcpy(rx->fast_buf + at, d, 4);      /* d[4] = 0x02 mitigation byte */
    memcpy(rx->fast_buf + at + 4, d + 5, 4);
}

static void slow_block(dstar_rx_t *rx, const uint8_t b[6])
{
    dstar_event_t ev;
    uint8_t mh = b[0], n = mh & 0x0Fu;
    switch (mh >> 4) {
    case 0x3: /* simple data: GPS NMEA, D-PRS, user data */
        if (n < 1 || n > 5)
            break;
        ev.type = DSTAR_EV_SIMPLE_DATA;
        ev.u.data.data = b + 1;
        ev.u.data.len = n;
        emit(rx, &ev);
        for (unsigned i = 0; i < n; i++) {
            char c = (char)b[1 + i];
            if (c == '\r' || c == '\n') {
                if (rx->line_len) {
                    ev.type = DSTAR_EV_GPS_LINE;
                    ev.u.data.data = (const uint8_t *)rx->line;
                    ev.u.data.len = rx->line_len;
                    emit(rx, &ev);
                    rx->line_len = 0;
                }
            } else if (rx->line_len < DSTAR_LINE_MAX) {
                rx->line[rx->line_len++] = c;
            } else {
                rx->line_len = 0; /* overflow: drop the line */
            }
        }
        break;
    case 0x4: /* 20 char message, block number in low nibble */
        if (n > 3)
            break;
        memcpy(rx->text + 5u * n, b + 1, 5);
        rx->text_mask |= (uint8_t)(1u << n);
        if (rx->text_mask == 0x0F) {
            rx->text_mask = 0;
            ev.type = DSTAR_EV_TEXT;
            memcpy(ev.u.text, rx->text, DSTAR_TEXT_LEN);
            ev.u.text[DSTAR_TEXT_LEN] = '\0';
            if (strcmp(ev.u.text, rx->last_text) != 0) {
                memcpy(rx->last_text, ev.u.text, sizeof(rx->last_text));
                emit(rx, &ev);
            }
        }
        break;
    case 0x5: /* header resend */
        if (n < 1 || n > 5)
            break;
        if (rx->hdr_len + n > DSTAR_HEADER_BYTES) {
            rx->hdr_len = 0;
            break;
        }
        memcpy(rx->hdr_buf + rx->hdr_len, b + 1, n);
        rx->hdr_len = (uint8_t)(rx->hdr_len + n);
        if (rx->hdr_len == DSTAR_HEADER_BYTES) {
            dstar_header_t h;
            rx->hdr_len = 0;
            if (dstar_header_unpack(rx->hdr_buf, &h) &&
                (!rx->have_header || memcmp(&h, &rx->hdr, sizeof(h)) != 0)) {
                rx->hdr = h;
                rx->have_header = 1;
                ev.type = DSTAR_EV_HEADER;
                ev.u.header.hdr = h;
                ev.u.header.crc_ok = true;
                ev.u.header.from_slow_data = true;
                ev.u.header.fec_errors = 0;
                emit(rx, &ev);
            }
        }
        break;
    case 0xC: /* code squelch, 2 bytes */
        if (n == 2) {
            ev.type = DSTAR_EV_CSQ;
            ev.u.csq[0] = b[1];
            ev.u.csq[1] = b[2];
            emit(rx, &ev);
        }
        break;
    default: /* 0x66 = no data, reserved types ignored */
        break;
    }
}

static uint32_t bits24(const uint8_t *bits)
{
    uint32_t v = 0;
    for (int i = 0; i < 24; i++)
        v = (v << 1) | bits[i];
    return v;
}

static void lost(dstar_rx_t *rx)
{
    dstar_event_t ev;
    ev.type = DSTAR_EV_LOST;
    emit(rx, &ev);
    dstar_rx_reset(rx);
}

static void voice_frame(dstar_rx_t *rx)
{
    uint8_t v[DSTAR_VOICE_BYTES], d[DSTAR_DATA_BYTES];
    dv_pack_lsb(rx->buf, 72, v);
    dv_pack_lsb(rx->buf + 72, 24, d);
    unsigned dserr = dv_popcount32(bits24(rx->buf + 72) ^ DSTAR_DSYNC);

    if (rx->seq != 0 && dserr <= 1) {
        /* clearly a sync frame at an unexpected position: realign */
        rx->seq = 0;
        slow_reset(rx);
    }
    if (rx->seq == 0) {
        if (dserr <= rx->max_dsync_err) {
            rx->sync_miss = 0;
        } else if (++rx->sync_miss >= 3) {
            lost(rx);
            return;
        }
        rx->hdr_len = 0;
        rx->fast_active = 0;
        memcpy(rx->held_voice, v, sizeof(v)); /* wait for seq 1: fast or voice */
        rx->held_valid = 1;
        rx->seq = 1;
        return;
    }

    for (int i = 0; i < 3; i++)
        d[i] ^= DSTAR_SLOW_SCRAMBLE[i];

    uint8_t seq = rx->seq;
    if (seq & 1u) {
        memcpy(rx->half, d, 3);
        uint8_t mh = d[0];
        if (mh >= 0x81u && mh <= 0x9Cu) { /* fast data: 1..28 bytes (JARL 6.1.3) */
            unsigned len = (mh & 0x10u) ? 16u + (mh & 0x0Fu) : (mh & 0x0Fu);
            rx->fast_active = 1;
            rx->fast_len = (uint8_t)len;
            memset(rx->fast_buf, 0, sizeof(rx->fast_buf));
            rx->fast_buf[0] = d[1];
            rx->fast_buf[1] = d[2];
            fast_voice(rx, v, 4);
            if (seq == 1 && rx->held_valid)
                fast_voice(rx, rx->held_voice, 20);
            rx->held_valid = 0;
        } else {
            rx->fast_active = 0;
            if (rx->held_valid) {
                emit_voice(rx, rx->held_voice, 0);
                rx->held_valid = 0;
            }
            emit_voice(rx, v, seq);
        }
    } else {
        if (rx->fast_active) {
            /* d[0] = guard byte */
            rx->fast_buf[2] = d[1];
            rx->fast_buf[3] = d[2];
            fast_voice(rx, v, 12);
            unsigned cap = (seq == 2) ? 28u : 20u;
            dstar_event_t ev;
            ev.type = DSTAR_EV_FAST_DATA;
            ev.u.data.data = rx->fast_buf;
            ev.u.data.len = rx->fast_len < cap ? rx->fast_len : cap;
            emit(rx, &ev);
            rx->fast_active = 0;
        } else {
            uint8_t blk[6];
            memcpy(blk, rx->half, 3);
            memcpy(blk + 3, d, 3);
            emit_voice(rx, v, seq);
            slow_block(rx, blk);
        }
    }
    rx->seq = (uint8_t)((seq + 1u) % DSTAR_SUPERFRAME);
}

void dstar_rx_push_bit(dstar_rx_t *rx, uint8_t bit)
{
    bit &= 1u;
    rx->sr = (rx->sr << 1) | bit;

    switch (rx->state) {
    case DSTAR_RX_SEARCH:
        if (dv_popcount64((rx->sr ^ HDR_PATTERN) & HDR_MASK) <= rx->max_sync_err) {
            rx->state = DSTAR_RX_HEADER;
            rx->nbits = 0;
        } else if (dv_popcount64((rx->sr ^ DSTAR_DSYNC) & DSYNC_MASK) <= 1u) {
            /* late entry: the 24 bits just received were a sync data frame */
            dstar_event_t ev;
            rx->state = DSTAR_RX_VOICE;
            rx->nbits = 0;
            rx->seq = 1;
            rx->sync_miss = 0;
            slow_reset(rx);
            ev.type = DSTAR_EV_LATE_ENTRY;
            emit(rx, &ev);
        }
        break;

    case DSTAR_RX_HEADER:
        rx->buf[rx->nbits++] = bit;
        if (rx->nbits == DSTAR_HEADER_BITS) {
            uint8_t raw[DSTAR_HEADER_BYTES];
            dstar_event_t ev;
            ev.type = DSTAR_EV_HEADER;
            ev.u.header.fec_errors = dstar_header_decode(rx->buf, raw);
            ev.u.header.crc_ok = dstar_header_unpack(raw, &ev.u.header.hdr);
            ev.u.header.from_slow_data = false;
            if (ev.u.header.crc_ok) {
                rx->hdr = ev.u.header.hdr;
                rx->have_header = 1;
            }
            emit(rx, &ev);
            rx->state = DSTAR_RX_VOICE;
            rx->nbits = 0;
            rx->seq = 0;
            rx->sync_miss = 0;
            slow_reset(rx);
        }
        break;

    case DSTAR_RX_VOICE:
        rx->buf[rx->nbits++] = bit;
        if (rx->nbits == DSTAR_END_BITS &&
            dv_popcount64((rx->sr ^ DSTAR_END_PATTERN) & END_MASK) <= rx->max_end_err) {
            dstar_event_t ev;
            if (rx->held_valid)
                emit_voice(rx, rx->held_voice, 0);
            ev.type = DSTAR_EV_END;
            emit(rx, &ev);
            dstar_rx_reset(rx);
            break;
        }
        if (rx->nbits == DSTAR_FRAME_BITS) {
            voice_frame(rx);
            rx->nbits = 0;
        } else if (rx->sync_miss && (rx->sr & DSYNC_MASK) == DSTAR_DSYNC && rx->nbits >= 24) {
            /* bit slip: exact data sync at an odd position after a missed
             * sync -> the frame ends here; restart at seq 1 */
            rx->nbits = 0;
            rx->seq = 1;
            rx->sync_miss = 0;
            slow_reset(rx);
        }
        break;
    }
}

void dstar_rx_push_bits(dstar_rx_t *rx, const uint8_t *bits, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dstar_rx_push_bit(rx, bits[i]);
}
