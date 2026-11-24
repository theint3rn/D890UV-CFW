/*
 * ysf_tx.c - YSF transmitter: HC, CC (V/D 1, V/D 2, Voice FR, Data FR), TC.
 *
 * Rolling CSD/DT in CC frames follows spec Tables 4-3 (V/D 1), 4-4 (V/D 2)
 * and 4-5 (Data FR). Voice FR: the first CC after HC is CC0 (FT=1, FN=0) with
 * CSD3; then FT=1/FN=1, then FT=0/FN=0 (spec Figure 4-20).
 */
#include <string.h>

#include "../common/dv_common.h"
#include "ysf.h"

static uint8_t calc_ft(const ysf_tx_t *tx)
{
    unsigned n;
    switch (tx->dt) {
    case YSF_DT_VD1:
        n = (tx->text_len + 19u) / 20u;
        return (uint8_t)(2u + (n > 5 ? 5 : n));
    case YSF_DT_VD2:
        n = (tx->text_len + 9u) / 10u;
        return (uint8_t)(5u + (n > 2 ? 2 : n));
    default:
        return 0;
    }
}

void ysf_tx_init(ysf_tx_t *tx, uint8_t dt, const ysf_csd_t *csd, uint8_t dgid)
{
    memset(tx, 0, sizeof(*tx));
    tx->dt = dt & 3u;
    tx->csd = *csd;
    tx->base.cs = 2; /* "10": callsigns 1, 2 and 3 assigned */
    tx->base.cm = YSF_CM_GROUP;
    tx->base.dt = tx->dt;
    ysf_fich_set_dgid(&tx->base, dgid);
    tx->ft = calc_ft(tx);
}

void ysf_tx_set_data(ysf_tx_t *tx, const uint8_t *data, unsigned len)
{
    unsigned max = tx->dt == YSF_DT_VD2 ? 20u : 100u;
    if (len > max)
        len = max;
    memcpy(tx->text, data, len);
    tx->text_len = len;
    tx->ft = calc_ft(tx);
    if (tx->fn > tx->ft)
        tx->fn = 0;
}

static void put_dch20_split(uint8_t payload[720], const uint8_t d[20], unsigned off, unsigned stride)
{
    uint8_t u[360];
    ysf_dch_encode(d, 20, u);
    for (unsigned i = 0; i < 5; i++)
        memcpy(payload + stride * i + off, u + 72 * i, 72);
}

static void hc_tc(ysf_tx_t *tx, uint8_t fi, uint8_t dibits[YSF_FRAME_DIBITS])
{
    uint8_t payload[720], d[20];
    ysf_fich_t f = tx->base;
    f.fi = fi;
    f.fn = 0;
    f.ft = tx->ft;
    ysf_csd1_pack(&tx->csd, d);
    put_dch20_split(payload, d, 0, 144);
    ysf_csd2_pack(&tx->csd, d);
    put_dch20_split(payload, d, 72, 144);
    ysf_build_frame(&f, payload, dibits);
}

void ysf_tx_header(ysf_tx_t *tx, uint8_t dibits[YSF_FRAME_DIBITS])
{
    tx->fn = 0;
    tx->cc_count = 0;
    hc_tc(tx, YSF_FI_HEADER, dibits);
}

void ysf_tx_terminator(ysf_tx_t *tx, uint8_t dibits[YSF_FRAME_DIBITS])
{
    hc_tc(tx, YSF_FI_TERM, dibits);
}

static void unit20_for(const ysf_tx_t *tx, unsigned u, uint8_t d[20])
{
    switch (u) {
    case 0: ysf_csd1_pack(&tx->csd, d); break;
    case 1: ysf_csd2_pack(&tx->csd, d); break;
    case 2: ysf_csd3_pack(&tx->csd, d); break;
    default: {
        unsigned off = (u - 3u) * 20u;
        memset(d, 0x20, 20);
        if (off < tx->text_len)
            memcpy(d, tx->text + off, tx->text_len - off < 20 ? tx->text_len - off : 20);
        break;
    }
    }
}

static void unit10_for(const ysf_tx_t *tx, unsigned fn, uint8_t d[10])
{
    switch (fn) {
    case 0: memcpy(d, tx->csd.dest, 10); break;
    case 1: memcpy(d, tx->csd.src, 10); break;
    case 2: memcpy(d, tx->csd.down, 10); break;
    case 3: memcpy(d, tx->csd.up, 10); break;
    case 4: memcpy(d, tx->csd.rem[0], 10); break;
    case 5: memcpy(d, tx->csd.rem[2], 10); break;
    default: {
        unsigned off = (fn - 6u) * 10u;
        memset(d, 0x20, 10);
        if (off < tx->text_len)
            memcpy(d, tx->text + off, tx->text_len - off < 10 ? tx->text_len - off : 10);
        break;
    }
    }
}

void ysf_tx_voice(ysf_tx_t *tx, const uint8_t *frames, uint8_t dibits[YSF_FRAME_DIBITS])
{
    uint8_t payload[720];
    uint8_t u[360], d[20];
    ysf_fich_t f = tx->base;
    f.fi = YSF_FI_COMM;
    f.fn = tx->fn;
    f.ft = tx->ft;
    memset(payload, 0, sizeof(payload));

    switch (tx->dt) {
    case YSF_DT_VD1:
        unit20_for(tx, tx->fn, d);
        put_dch20_split(payload, d, 0, 144);
        for (unsigned i = 0; i < 5; i++)
            dv_unpack_msb(frames + 9 * i, 72, payload + 144 * i + 72);
        break;
    case YSF_DT_VD2:
        unit10_for(tx, tx->fn, d);
        ysf_dch_encode(d, 10, u);
        for (unsigned i = 0; i < 5; i++) {
            uint8_t v[104];
            memcpy(payload + 144 * i, u + 40 * i, 40);
            ysf_vd2_voice_encode(frames + 7 * i, v);
            memcpy(payload + 144 * i + 40, v, 72);
            memcpy(payload + 144 * i + 112, v + 72, 32);
        }
        break;
    case YSF_DT_VOICE_FR:
        if (tx->cc_count == 0) {
            f.ft = 1;
            f.fn = 0;
            ysf_csd3_pack(&tx->csd, d);
            ysf_dch_encode(d, 20, payload); /* DCH(0..4) contiguous, 360 bits */
            /* 360..431 reserved (zeros) */
            dv_unpack_msb(frames + 18 * 3, 144, payload + 432);
            dv_unpack_msb(frames + 18 * 4, 144, payload + 576);
        } else {
            f.ft = tx->cc_count == 1 ? 1 : 0;
            f.fn = tx->cc_count == 1 ? 1 : 0;
            for (unsigned i = 0; i < 5; i++)
                dv_unpack_msb(frames + 18 * i, 144, payload + 144 * i);
        }
        break;
    default: /* Data FR has its own call */
        break;
    }
    ysf_build_frame(&f, payload, dibits);
    tx->cc_count++;
    tx->fn = (uint8_t)(tx->ft ? (tx->fn + 1u) % (tx->ft + 1u) : 0u);
}

/* ---- Data FR ------------------------------------------------------------------- */

#define FR_BLOCK 260u

static unsigned fr_block_frames(unsigned blen)
{
    unsigned units = 3u + (blen + 19u) / 20u;
    return (units + 1u) / 2u;
}

unsigned ysf_tx_data_fr_frames(unsigned len)
{
    if (len > 4u * FR_BLOCK)
        len = 4u * FR_BLOCK;
    unsigned total = 0;
    do {
        unsigned b = len > FR_BLOCK ? FR_BLOCK : len;
        total += fr_block_frames(b);
        len -= b;
    } while (len);
    return total;
}

void ysf_tx_data_fr(ysf_tx_t *tx, const uint8_t *data, unsigned len, unsigned k,
                    uint8_t dibits[YSF_FRAME_DIBITS])
{
    uint8_t payload[720], d[20];
    if (len > 4u * FR_BLOCK)
        len = 4u * FR_BLOCK;
    unsigned nblocks = len ? (len + FR_BLOCK - 1u) / FR_BLOCK : 1u;
    unsigned blk = 0, off = 0;
    unsigned blen = len > FR_BLOCK ? FR_BLOCK : len;
    unsigned nf = fr_block_frames(blen);
    while (k >= nf && blk + 1u < nblocks) {
        k -= nf;
        blk++;
        off += FR_BLOCK;
        blen = len - off > FR_BLOCK ? FR_BLOCK : len - off;
        nf = fr_block_frames(blen);
    }
    ysf_fich_t f = tx->base;
    f.fi = YSF_FI_COMM;
    f.dt = YSF_DT_DATA_FR;
    f.bn = (uint8_t)blk;
    f.bt = (uint8_t)(nblocks - 1u);
    f.fn = (uint8_t)k;
    f.ft = (uint8_t)(nf - 1u);
    for (unsigned j = 0; j < 2; j++) {
        unsigned u = 2u * k + j;
        if (u < 3) {
            unit20_for(tx, u, d);
        } else {
            unsigned o = (u - 3u) * 20u;
            memset(d, 0x20, 20);
            if (o < blen)
                memcpy(d, data + off + o, blen - o < 20 ? blen - o : 20);
        }
        put_dch20_split(payload, d, 72 * j, 144);
    }
    ysf_build_frame(&f, payload, dibits);
}
