/* DMR link control, CSBK and data headers (TS 102 361-1 7.1, 9.1.6-9.2; TS 102 361-2 7.1). */
#include "dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

/* Annex D.2 Idle message information bits I(95)..I(0) */
const uint8_t DMR_IDLE_INFO[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };

static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

void dmr_lc_parse(const uint8_t raw[9], dmr_lc_t *lc)
{
    memcpy(lc->raw, raw, 9);
    lc->pf = raw[0] >> 7;
    lc->flco = raw[0] & 0x3Fu;
    lc->fid = raw[1];
    lc->svc_opts = raw[2];
    lc->dst = be24(raw + 3);
    lc->src = be24(raw + 6);
}

void dmr_lc_make_voice(dmr_lc_t *lc, bool group, uint32_t dst, uint32_t src, uint8_t svc_opts)
{
    uint8_t raw[9];
    raw[0] = group ? DMR_FLCO_GROUP_VOICE : DMR_FLCO_UNIT_VOICE;
    raw[1] = 0;                 /* SFID */
    raw[2] = svc_opts;
    put24(raw + 3, dst);
    put24(raw + 6, src);
    dmr_lc_parse(raw, lc);
}

int dmr_fulllc_decode(const uint8_t bits196[196], uint32_t mask, dmr_lc_t *lc)
{
    uint8_t info[12];
    if (bptc196_decode(bits196, info) < 0) return -1;
    info[9] ^= (uint8_t)(mask >> 16);
    info[10] ^= (uint8_t)(mask >> 8);
    info[11] ^= (uint8_t)mask;
    int r = fec_rs129_dec(info);
    if (r < 0) return -1;
    dmr_lc_parse(info, lc);
    return r;
}

void dmr_fulllc_encode(const dmr_lc_t *lc, uint32_t mask, uint8_t bits196[196])
{
    uint8_t info[12];
    memcpy(info, lc->raw, 9);
    fec_rs129_enc(info, info + 9);
    info[9] ^= (uint8_t)(mask >> 16);
    info[10] ^= (uint8_t)(mask >> 8);
    info[11] ^= (uint8_t)mask;
    bptc196_encode(info, bits196);
}

static void csbk_fields(dmr_csbk_t *c)
{
    c->lb = c->raw[0] >> 7;
    c->pf = (c->raw[0] >> 6) & 1u;
    c->opcode = c->raw[0] & 0x3Fu;
    c->fid = c->raw[1];
    memcpy(c->data, c->raw + 2, 8);
}

int dmr_csbk_decode(const uint8_t bits196[196], uint16_t mask, dmr_csbk_t *c)
{
    if (bptc196_decode(bits196, c->raw) < 0) return -1;
    uint16_t crc = (uint16_t)(((c->raw[10] << 8) | c->raw[11]) ^ mask);
    c->raw[10] = (uint8_t)(crc >> 8);
    c->raw[11] = (uint8_t)crc;
    csbk_fields(c);
    return crc == crc_ccitt_dmr(c->raw, 10) ? 0 : -1;
}

void dmr_csbk_set(dmr_csbk_t *c, uint8_t opcode, uint8_t fid, const uint8_t data[8])
{
    memset(c->raw, 0, sizeof c->raw);
    c->raw[0] = (uint8_t)(0x80u | (opcode & 0x3Fu));     /* last block = 1 */
    c->raw[1] = fid;
    memcpy(c->raw + 2, data, 8);
    csbk_fields(c);
}

void dmr_csbk_encode(dmr_csbk_t *c, uint16_t mask, uint8_t bits196[196])
{
    uint16_t crc = crc_ccitt_dmr(c->raw, 10);
    c->raw[10] = (uint8_t)(crc >> 8);
    c->raw[11] = (uint8_t)crc;
    uint8_t info[12];
    memcpy(info, c->raw, 12);
    info[10] ^= (uint8_t)(mask >> 8);
    info[11] ^= (uint8_t)mask;
    bptc196_encode(info, bits196);
}

static void dh_fields(dmr_data_hdr_t *h)
{
    const uint8_t *r = h->raw;
    h->gi = r[0] >> 7;
    h->a = (r[0] >> 6) & 1u;
    h->dpf = r[0] & 0xFu;
    h->sap = r[1] >> 4;
    h->poc = (uint8_t)(((r[0] >> 4) & 1u) << 4 | (r[1] & 0xFu));
    h->dst = be24(r + 2);
    h->src = be24(r + 5);
    h->fmf = r[8] >> 7;
    h->bf = r[8] & 0x7Fu;
    h->s = r[9] >> 7;
    h->ns = (r[9] >> 4) & 7u;
    h->fsn = r[9] & 0xFu;
}

int dmr_data_header_decode(const uint8_t bits196[196], dmr_data_hdr_t *h)
{
    if (bptc196_decode(bits196, h->raw) < 0) return -1;
    uint16_t crc = (uint16_t)(((h->raw[10] << 8) | h->raw[11]) ^ DMR_MASK_DATA_HEADER);
    dh_fields(h);
    return crc == crc_ccitt_dmr(h->raw, 10) ? 0 : -1;
}

void dmr_data_header_build_unconfirmed(dmr_data_hdr_t *h, bool group, uint32_t dst, uint32_t src,
                                       uint8_t sap, uint8_t blocks, uint8_t pad_octets)
{
    uint8_t *r = h->raw;
    memset(r, 0, 12);
    r[0] = (uint8_t)((group ? 0x80u : 0u) | ((pad_octets & 0x10u) ? 0x10u : 0u) | DMR_DPF_UNCONFIRMED);
    r[1] = (uint8_t)((sap << 4) | (pad_octets & 0xFu));
    put24(r + 2, dst);
    put24(r + 5, src);
    r[8] = (uint8_t)(0x80u | (blocks & 0x7Fu));         /* FMF = 1 */
    r[9] = 0;                                           /* FSN 0: unfragmented */
    dh_fields(h);
}

void dmr_data_header_encode(const dmr_data_hdr_t *h, uint8_t bits196[196])
{
    uint8_t info[12];
    memcpy(info, h->raw, 10);
    uint16_t crc = (uint16_t)(crc_ccitt_dmr(info, 10) ^ DMR_MASK_DATA_HEADER);
    info[10] = (uint8_t)(crc >> 8);
    info[11] = (uint8_t)crc;
    bptc196_encode(info, bits196);
}
