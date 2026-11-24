/* DMR link modes (direct / TDMA direct / repeater inbound / BS outbound), talkspurt sequencing and
 * reverse channel bursts (TS 102 361-1 4.4, 5.1.4, 5.1.5, 6.4, annex E table E.12; -2 table 7.27). */
#include "dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

dmr_sync_t dmr_link_voice_sync(dmr_link_t link, uint8_t slot)
{
    switch (link) {
    case DMR_LINK_TDMA_DIRECT: return slot ? DMR_SYNC_TS2_VOICE : DMR_SYNC_TS1_VOICE;
    case DMR_LINK_BS: return DMR_SYNC_BS_VOICE;
    default: return DMR_SYNC_MS_VOICE;          /* direct (talkaround) and repeater inbound */
    }
}

dmr_sync_t dmr_link_data_sync(dmr_link_t link, uint8_t slot)
{
    return dmr_sync_data_for(dmr_link_voice_sync(link, slot));
}

bool dmr_sync_is_bs(dmr_sync_t s) { return s == DMR_SYNC_BS_VOICE || s == DMR_SYNC_BS_DATA; }
bool dmr_sync_is_ms(dmr_sync_t s) { return s == DMR_SYNC_MS_VOICE || s == DMR_SYNC_MS_DATA || s == DMR_SYNC_MS_RC; }
bool dmr_sync_is_direct(dmr_sync_t s)
{
    return s == DMR_SYNC_TS1_VOICE || s == DMR_SYNC_TS1_DATA || s == DMR_SYNC_TS2_VOICE || s == DMR_SYNC_TS2_DATA;
}

/* ---------------- reverse channel ---------------- */
#define RC_CRC_MASK 0x7Au           /* table B.21 */

uint16_t dmr_rc_info(uint8_t cmd)
{
    uint8_t crc = (uint8_t)(crc7_dmr(cmd & 0xFu) ^ RC_CRC_MASK);
    return (uint16_t)(((cmd & 0xFu) << 7) | (crc & 0x7Fu));
}

int dmr_rc_check(uint16_t info11, uint8_t *cmd)
{
    uint8_t c = (uint8_t)((info11 >> 7) & 0xFu);
    if (cmd) *cmd = c;
    return (info11 & 0x7Fu) == ((crc7_dmr(c) ^ RC_CRC_MASK) & 0x7Fu) ? 0 : -1;
}

void dmr_rc_emb32(uint8_t cmd, uint8_t emb32[32])
{
    sbbptc_encode(dmr_rc_info(cmd), 1, emb32);
}

void dmr_tx_rc_standalone(uint8_t cc, uint8_t cmd, uint8_t rc[DMR_RC_BURST_DIBITS])
{
    uint8_t b[96], r32[32];
    uint16_t emb = fec_qr1676_enc((uint8_t)(((cc & 0xFu) << 3) | (1u << 2) | DMR_LCSS_SINGLE));
    dmr_rc_emb32(cmd, r32);
    bits_arr_set(b, 8, emb >> 8);                         /* L24..L21: CC, PI, LCSS, QR(8) */
    memcpy(b + 8, r32, 16);                               /* L20..L13: TX(31)..TX(16) */
    uint64_t s = DMR_SYNC_PATTERN[DMR_SYNC_MS_RC];
    for (int i = 0; i < 48; i++) b[24 + i] = (uint8_t)((s >> (47 - i)) & 1u);
    memcpy(b + 72, r32 + 16, 16);                         /* R13..R20: TX(15)..TX(0) */
    bits_arr_set(b + 88, 8, emb & 0xFFu);                 /* R21..R24: QR(7..0) */
    bits_to_dibits(rc, b, DMR_RC_BURST_DIBITS);
}

/* ---------------- talkspurt sequencing ---------------- */
void dmr_call_tx_start(dmr_call_tx_t *t, dmr_link_t link, uint8_t slot, uint8_t cc, const dmr_lc_t *lc,
                       uint8_t n_headers, dmr_priv_t *priv)
{
    memset(t, 0, sizeof *t);
    t->link = link;
    t->slot = slot & 1u;
    t->cc = cc & 0xFu;
    t->n_headers = n_headers ? n_headers : 1;
    t->lc = *lc;
    t->priv = priv;
    dmr_voice_tx_init(&t->v, lc, t->cc, dmr_link_voice_sync(link, t->slot));
    if (priv) {
        dmr_voice_tx_set_priv(&t->v, priv);
        t->lc = t->v.lc;                  /* privacy bit set in the service options */
        t->pi_pending = true;
    }
}

static void lead(dmr_call_tx_t *t, uint8_t out[DMR_SLOT_DIBITS])
{
    if (t->link == DMR_LINK_BS) {
        static const dmr_slc_t null_slc = { DMR_SLCO_NULL, 0 };
        dmr_tx_cach_slc(t->slc ? t->slc : &null_slc, t->cach_n++, 1, t->slot, out);
    } else {
        memset(out, 0, DMR_CACH_DIBITS);  /* guard / PA ramp: not keyed */
    }
}

dmr_txk_t dmr_call_tx_next(dmr_call_tx_t *t, const uint8_t frames27[27], uint8_t out[DMR_SLOT_DIBITS])
{
    dmr_sync_t ds = dmr_link_data_sync(t->link, t->slot);
    lead(t, out);
    if (t->pi_pending) {
        t->pi_pending = false;
        dmr_tx_pi_header(t->priv, t->cc, ds, out + DMR_CACH_DIBITS);
        return DMR_TXK_PI;
    }
    if (t->sent_headers < t->n_headers) {
        t->sent_headers++;
        dmr_tx_voice_header(&t->lc, t->cc, ds, out + DMR_CACH_DIBITS);
        return DMR_TXK_HEADER;
    }
    uint8_t silent[27];
    if (!frames27) { memset(silent, 0, sizeof silent); frames27 = silent; }
    dmr_voice_tx_burst(&t->v, frames27, out + DMR_CACH_DIBITS);
    return DMR_TXK_VOICE;
}

void dmr_call_tx_end(dmr_call_tx_t *t, uint8_t out[DMR_SLOT_DIBITS])
{
    lead(t, out);
    dmr_tx_terminator(&t->lc, t->cc, dmr_link_data_sync(t->link, t->slot), out + DMR_CACH_DIBITS);
    t->ended = true;
}
