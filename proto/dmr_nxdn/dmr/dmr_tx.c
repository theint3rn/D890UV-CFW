/* DMR transmit builders (TS 102 361-1 clause 5.1.2 voice superframe, 7, 8, B). */
#include "dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

void dmr_voice_tx_init(dmr_voice_tx_t *t, const dmr_lc_t *lc, uint8_t cc, dmr_sync_t voice_sync)
{
    memset(t, 0, sizeof *t);
    t->lc = *lc;
    t->cc = cc;
    t->voice_sync = voice_sync;
    emblc_encode(lc->raw, t->emb_bits);
}

void dmr_voice_tx_set_priv(dmr_voice_tx_t *t, dmr_priv_t *priv)
{
    t->priv = priv;
    t->sf = priv ? priv->sf : 0;
    if (priv) {             /* set the service options privacy bit and rebuild the embedded voice LC */
        t->lc.raw[2] |= 0x40u;
        dmr_lc_parse(t->lc.raw, &t->lc);
        emblc_encode(t->lc.raw, t->emb_bits);
    }
}

void dmr_tx_pi_header(const dmr_priv_t *priv, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS])
{
    dmr_priv_info_t i = { priv->alg, priv->key_id, priv->iv, priv->sf };
    uint8_t info[12], bits[196];
    dmr_priv_pi_build(&i, info);
    uint16_t crc = (uint16_t)(crc_ccitt_dmr(info, 10) ^ DMR_MASK_PI_HEADER);
    info[10] = (uint8_t)(crc >> 8);
    info[11] = (uint8_t)crc;
    bptc196_encode(info, bits);
    dmr_burst_build_data(burst, data_sync, cc, DMR_DT_PI_HEADER, bits);
}

void dmr_voice_tx_burst(dmr_voice_tx_t *t, const uint8_t frames_in[27], uint8_t burst[DMR_BURST_DIBITS])
{
    static const uint8_t lcss_seq[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };
    uint8_t frames[27];
    memcpy(frames, frames_in, sizeof frames);
    const uint8_t *emb = t->emb_bits;
    if (t->priv) {
        t->priv->sf = t->sf;
        for (uint8_t f = 0; f < 3; f++) dmr_priv_voice(t->priv, t->n, f, frames + 9 * f);
        if (t->sf & 1u) {   /* odd superframes carry the privacy LC for late entry */
            if (t->n == 0) {
                dmr_priv_info_t i = { t->priv->alg, t->priv->key_id, t->priv->iv, t->sf };
                uint8_t lc[9];
                dmr_priv_lc_build(&i, lc);
                emblc_encode(lc, t->priv_bits);
            }
            emb = t->priv_bits;
        }
    }
    if (t->n == 0) {
        dmr_burst_build_voice_sync(burst, t->voice_sync, frames);
    } else if (t->n <= 4) {
        dmr_burst_build_voice_emb(burst, t->cc, 0, lcss_seq[t->n - 1], emb + 32 * (t->n - 1), frames);
    } else {
        uint8_t null32[32];
        memset(null32, 0, sizeof null32);     /* annex D.1 null embedded message */
        dmr_burst_build_voice_emb(burst, t->cc, 0, DMR_LCSS_SINGLE, null32, frames);
    }
    t->n = (uint8_t)((t->n + 1) % 6);
    if (t->n == 0) t->sf++;
}

void dmr_tx_voice_header(const dmr_lc_t *lc, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS])
{
    uint8_t bits[196];
    dmr_fulllc_encode(lc, DMR_MASK_VOICE_HDR, bits);
    dmr_burst_build_data(burst, data_sync, cc, DMR_DT_VOICE_LC_HEADER, bits);
}

void dmr_tx_terminator(const dmr_lc_t *lc, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS])
{
    uint8_t bits[196];
    dmr_fulllc_encode(lc, DMR_MASK_TERMINATOR, bits);
    dmr_burst_build_data(burst, data_sync, cc, DMR_DT_TERMINATOR_LC, bits);
}

void dmr_tx_csbk(dmr_csbk_t *c, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS])
{
    uint8_t bits[196];
    dmr_csbk_encode(c, DMR_MASK_CSBK, bits);
    dmr_burst_build_data(burst, data_sync, cc, DMR_DT_CSBK, bits);
}

void dmr_tx_idle(uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS])
{
    uint8_t bits[196];
    bptc196_encode(DMR_IDLE_INFO, bits);
    dmr_burst_build_data(burst, data_sync, cc, DMR_DT_IDLE, bits);
}

static size_t last_len(size_t b, size_t blocks) { return b + 1 == blocks ? 8u : 12u; }

int dmr_tx_data_unconfirmed(bool group, uint32_t dst, uint32_t src, uint8_t sap, const uint8_t *payload,
                            size_t len, dmr_priv_t *priv, uint8_t cc, dmr_sync_t data_sync,
                            uint8_t bursts[][DMR_BURST_DIBITS], int max_bursts)
{
    size_t blocks = (len + 4u + 11u) / 12u;
    size_t pad = blocks * 12u - 4u - len;
    int first = priv ? 1 : 0;
    if (blocks > 127 || pad > 31 || (int)blocks + 1 + first > max_bursts) return -1;
    if (priv) {
        dmr_tx_pi_header(priv, cc, data_sync, bursts[0]);
        bursts++;
    }
    dmr_data_hdr_t h;
    uint8_t bits[196];
    dmr_data_header_build_unconfirmed(&h, group, dst, src, sap, (uint8_t)blocks, (uint8_t)pad);
    dmr_data_header_encode(&h, bits);
    dmr_burst_build_data(bursts[0], data_sync, cc, DMR_DT_DATA_HEADER, bits);
    /* CRC-32 over user data + pad octets (B.3.9), computed block by block */
    uint32_t crc = 0;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t blk[12];
        size_t off = 12u * b, n = off < len ? (len - off < 12u ? len - off : 12u) : 0u;
        memset(blk, 0, sizeof blk);
        if (n) memcpy(blk, payload + off, n);
        if (priv) {             /* user data and pad octets are encrypted, the CRC covers the ciphertext */
            dmr_priv_sync(priv, priv->iv, 0);
            dmr_priv_data(priv, (uint32_t)off, blk, last_len(b, blocks));
        }
        bool last = b + 1 == blocks;
        crc = crc32_dmr_cont(crc, blk, last ? 8u : 12u);
        if (last) {
            /* B.3.9 / figure B.8C: less significant CRC octets first (x7..x0 first on air) */
            blk[8] = (uint8_t)crc;
            blk[9] = (uint8_t)(crc >> 8);
            blk[10] = (uint8_t)(crc >> 16);
            blk[11] = (uint8_t)(crc >> 24);
        }
        bptc196_encode(blk, bits);
        dmr_burst_build_data(bursts[1 + b], data_sync, cc, DMR_DT_RATE_12, bits);
    }
    return (int)blocks + 1 + first;
}

void dmr_tx_cach_slc(const dmr_slc_t *slc, unsigned n, uint8_t at, uint8_t tc, uint8_t dibits[DMR_CACH_DIBITS])
{
    static const uint8_t lcss_seq[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };
    uint8_t raw[4], bits[68];
    raw[0] = slc->slco & 0xFu;
    raw[1] = (uint8_t)(slc->data24 >> 16);
    raw[2] = (uint8_t)(slc->data24 >> 8);
    raw[3] = (uint8_t)slc->data24;
    cach_slc_encode(raw, bits);
    dmr_cach_t c;
    c.at = at;
    c.tc = tc;
    c.lcss = lcss_seq[n & 3u];
    memcpy(c.payload, bits + 17 * (n & 3u), 17);
    dmr_cach_encode(&c, dibits);
}
