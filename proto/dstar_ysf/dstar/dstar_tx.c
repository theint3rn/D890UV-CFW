/*
 * dstar_tx.c - D-STAR DV transmitter: header, voice frames with slow data
 * scheduling, DV fast data, end pattern.
 *
 * Slow data schedule per superframe (10 blocks of 2 data frames):
 *   block 0 .. : code squelch (if set), then
 *   superframe 0 and every text_period-th: 20 char text (4 blocks),
 *   odd superframes: header resend (8 x 0x55 + 1 x 0x51 = 41 bytes),
 *   free blocks: DV fast data (if queued and no voice), else simple data
 *   (GPS / D-PRS ring, 0x3N), else 0x66 filler.
 */
#include <string.h>

#include "../common/dv_common.h"
#include "dstar.h"

enum { K_FREE = 0, K_TEXT = 1, K_HDR = 2, K_CSQ = 3 };

void dstar_tx_init(dstar_tx_t *tx, const dstar_header_t *hdr)
{
    memset(tx, 0, sizeof(*tx));
    dstar_header_pack(hdr, tx->raw_hdr);
    tx->text_period = 4;
    tx->header_resend = 1;
}

void dstar_tx_set_text(dstar_tx_t *tx, const char *msg)
{
    if (!msg) {
        tx->text_set = 0;
        return;
    }
    size_t i = 0;
    for (; i < DSTAR_TEXT_LEN && msg[i]; i++)
        tx->text[i] = msg[i];
    for (; i < DSTAR_TEXT_LEN; i++)
        tx->text[i] = ' ';
    tx->text_set = 1;
}

void dstar_tx_set_csq(dstar_tx_t *tx, bool on, uint8_t b0, uint8_t b1)
{
    tx->csq_set = on;
    tx->csq[0] = b0;
    tx->csq[1] = b1;
}

size_t dstar_tx_queue_data(dstar_tx_t *tx, const uint8_t *data, size_t len)
{
    size_t n = 0;
    while (n < len) {
        uint16_t next = (uint16_t)((tx->gps_head + 1u) % DSTAR_GPS_RING);
        if (next == tx->gps_tail)
            break;
        tx->gps[tx->gps_head] = data[n++];
        tx->gps_head = next;
    }
    return n;
}

void dstar_tx_set_fast_data(dstar_tx_t *tx, const uint8_t *data, size_t len)
{
    tx->fast = data;
    tx->fast_len = data ? len : 0;
    tx->fast_pos = 0;
}

bool dstar_tx_fast_busy(const dstar_tx_t *tx)
{
    return tx->fast_block || (tx->fast && tx->fast_pos < tx->fast_len);
}

static void put_bits_msb(uint8_t *out, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        out[i] = (uint8_t)((v >> (n - 1u - i)) & 1u);
}

size_t dstar_tx_header(dstar_tx_t *tx, size_t preamble_bits, uint8_t *out)
{
    if (preamble_bits < DSTAR_PREAMBLE_BITS)
        preamble_bits = DSTAR_PREAMBLE_BITS;
    preamble_bits = (preamble_bits + 1u) & ~(size_t)1u; /* end on a '0' */
    for (size_t i = 0; i < preamble_bits; i++)
        out[i] = (uint8_t)((i & 1u) ? 0 : 1);
    put_bits_msb(out + preamble_bits, DSTAR_FSYNC, DSTAR_FSYNC_BITS);
    dstar_header_encode(tx->raw_hdr, out + preamble_bits + DSTAR_FSYNC_BITS);
    tx->seq = 0;
    tx->superframes = 0;
    tx->fast_block = 0;
    return preamble_bits + DSTAR_FSYNC_BITS + DSTAR_HEADER_BITS;
}

static void plan_superframe(dstar_tx_t *tx)
{
    unsigned i = 0;
    memset(tx->plan, 0x66, sizeof(tx->plan));
    memset(tx->plan_kind, K_FREE, sizeof(tx->plan_kind));
    if (tx->csq_set) {
        tx->plan[0][0] = 0xC2;
        tx->plan[0][1] = tx->csq[0];
        tx->plan[0][2] = tx->csq[1];
        tx->plan_kind[0] = K_CSQ;
        i = 1;
    }
    uint32_t sf = tx->superframes;
    if (tx->text_set && tx->text_period && sf % tx->text_period == 0) {
        for (unsigned b = 0; b < 4; b++, i++) {
            tx->plan[i][0] = (uint8_t)(0x40u | b);
            memcpy(&tx->plan[i][1], tx->text + 5u * b, 5);
            tx->plan_kind[i] = K_TEXT;
        }
    } else if (tx->header_resend && (sf & 1u) && 10u - i >= 9u) {
        unsigned pos = 0;
        while (pos < DSTAR_HEADER_BYTES) {
            unsigned n = DSTAR_HEADER_BYTES - pos;
            if (n > 5)
                n = 5;
            tx->plan[i][0] = (uint8_t)(0x50u | n);
            memcpy(&tx->plan[i][1], tx->raw_hdr + pos, n);
            tx->plan_kind[i] = K_HDR;
            pos += n;
            i++;
        }
    }
}

static void fill_free_block(dstar_tx_t *tx, unsigned blk)
{
    uint8_t *p = tx->plan[blk];
    unsigned n = 0;
    memset(p, 0x66, 6);
    while (n < 5 && tx->gps_tail != tx->gps_head) {
        p[1 + n++] = tx->gps[tx->gps_tail];
        tx->gps_tail = (uint16_t)((tx->gps_tail + 1u) % DSTAR_GPS_RING);
    }
    if (n)
        p[0] = (uint8_t)(0x30u | n);
}

static bool fast_pending(const dstar_tx_t *tx)
{
    return tx->fast && tx->fast_pos < tx->fast_len;
}

static void fast_take(dstar_tx_t *tx, unsigned cap)
{
    size_t n = tx->fast_len - tx->fast_pos;
    if (n > cap)
        n = cap;
    memset(tx->fast_blk, 0x00, sizeof(tx->fast_blk));
    memcpy(tx->fast_blk, tx->fast + tx->fast_pos, n);
    tx->fast_pos += n;
    tx->fast_blk_len = (uint8_t)n;
    tx->fast_block = 1;
}

static void fast_voice(const dstar_tx_t *tx, unsigned at, uint8_t v[DSTAR_VOICE_BYTES])
{
    uint8_t pn_bits[72], pn[DSTAR_VOICE_BYTES];
    memcpy(v, tx->fast_blk + at, 4);
    v[4] = 0x02; /* "mitigation" byte for legacy receivers */
    memcpy(v + 5, tx->fast_blk + at + 4, 4);
    memset(pn_bits, 0, sizeof(pn_bits));
    dstar_scramble(pn_bits, 72);
    dv_pack_lsb(pn_bits, 72, pn);
    for (unsigned i = 0; i < DSTAR_VOICE_BYTES; i++)
        v[i] ^= pn[i];
}

static uint8_t fast_mh(uint8_t len)
{
    return (uint8_t)(len <= 15 ? (0x80u | len) : (0x90u | (len - 16u)));
}

size_t dstar_tx_frame(dstar_tx_t *tx, const uint8_t *ambe, uint8_t out[DSTAR_FRAME_BITS])
{
    uint8_t v[DSTAR_VOICE_BYTES], d[DSTAR_DATA_BYTES];
    uint8_t seq = tx->seq;

    if (seq == 0) {
        plan_superframe(tx);
        tx->fast_block = 0;
        if (!ambe && fast_pending(tx) && tx->plan_kind[0] == K_FREE)
            fast_take(tx, 28);
        if (tx->fast_block)
            fast_voice(tx, 20, v);
        else
            memcpy(v, ambe ? ambe : DSTAR_AMBE_SILENCE, DSTAR_VOICE_BYTES);
        d[0] = 0x55;
        d[1] = 0x2D;
        d[2] = 0x16;
    } else {
        unsigned blk = (seq - 1u) / 2u;
        if ((seq & 1u) && seq != 1) {
            tx->fast_block = 0;
            if (!ambe && fast_pending(tx) && tx->plan_kind[blk] == K_FREE)
                fast_take(tx, 20);
        }
        if ((seq & 1u) && !tx->fast_block && tx->plan_kind[blk] == K_FREE)
            fill_free_block(tx, blk);

        if (tx->fast_block) {
            if (seq & 1u) {
                d[0] = fast_mh(tx->fast_blk_len);
                d[1] = tx->fast_blk[0];
                d[2] = tx->fast_blk[1];
                fast_voice(tx, 4, v);
            } else {
                d[0] = 0x00; /* guard: anything but the packet-loss pattern */
                d[1] = tx->fast_blk[2];
                d[2] = tx->fast_blk[3];
                fast_voice(tx, 12, v);
            }
        } else {
            memcpy(v, ambe ? ambe : DSTAR_AMBE_SILENCE, DSTAR_VOICE_BYTES);
            memcpy(d, &tx->plan[blk][(seq & 1u) ? 0 : 3], 3);
        }
        for (int i = 0; i < 3; i++)
            d[i] ^= DSTAR_SLOW_SCRAMBLE[i];
        if (!(seq & 1u))
            tx->fast_block = 0; /* block ends with the even frame */
    }

    dv_unpack_lsb(v, 72, out);
    dv_unpack_lsb(d, 24, out + 72);
    if (++tx->seq >= DSTAR_SUPERFRAME) {
        tx->seq = 0;
        tx->superframes++;
    }
    return DSTAR_FRAME_BITS;
}

size_t dstar_tx_end(dstar_tx_t *tx, uint8_t out[DSTAR_END_BITS])
{
    (void)tx;
    put_bits_msb(out, DSTAR_END_PATTERN, DSTAR_END_BITS);
    return DSTAR_END_BITS;
}
