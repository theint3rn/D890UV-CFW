/*
 * p25_p1.c - P25 Phase 1 frame assembly / disassembly (see p25_p1.h).
 * References: TIA-102.BAAA-B sections 5 (frame formats), 6 (status symbols,
 * NID), 7 (FEC: Golay, Hamming, RS, trellis, interleave); TIA-102.BABA 7
 * (IMBE FEC + bit interleave); TIA-102.AABF (link control words).
 */
#include "p25_p1.h"
#include "p25_fec.h"
#include <string.h>

/* data-bit offsets inside an LDU (status symbols removed) */
static const uint16_t ldu_voice_off[9] = { 112, 256, 440, 624, 808, 992, 1176, 1360, 1536 };
static const uint16_t ldu_lc_off[6] = { 400, 584, 768, 952, 1136, 1320 };
#define LDU_LSD_OFF 1504
#define LDU_DATA_DIBITS 840
#define HDU_DATA_DIBITS (56 + 324)
#define TDULC_DATA_DIBITS (56 + 144)

/* ------------------------------------------------------------- air I/O */
/* air buffer addressed by data-bit offset (status symbols skipped) */
static void air_put(uint8_t *air, unsigned k, unsigned n, uint64_t v)
{
    while (n--) {
        unsigned b = (unsigned)(v >> n) & 1u, d = k >> 1, a = d + d / 35;
        uint8_t m = (uint8_t)(2u >> (k & 1));
        if (b) air[a] |= m; else air[a] &= (uint8_t)~m;
        k++;
    }
}

static size_t air_finish(uint8_t *air, size_t data_dibits, uint8_t status)
{
    size_t blocks = (data_dibits + 34) / 35, len = blocks * 36;
    for (size_t f = 35; f < len; f += 36) air[f] = status & 3;
    return len;
}

static void air_header(uint8_t *air, size_t len, uint16_t nac, uint8_t duid)
{
    memset(air, 0, len);
    air_put(air, 0, 48, P25_SYNC_P1);
    air_put(air, 48, 64, p25_nid_encode(nac, duid));
}

size_t p25_p1_insert_status(const uint8_t *data, size_t n, uint8_t *air, uint8_t status)
{
    size_t a = 0;
    size_t blocks = (n + 34) / 35;
    for (size_t i = 0; i < blocks * 35; i++) {
        air[a++] = i < n ? (data[i] & 3) : 0;
        if (i % 35 == 34) air[a++] = status & 3;
    }
    return a;
}

/* --------------------------------------------------------------- hexbits */
static void bytes_to_hex(const uint8_t *b, unsigned nhex, uint8_t *hex)
{
    for (unsigned i = 0; i < nhex; i++) hex[i] = (uint8_t)p25_bits_get(b, 6 * i, 6);
}
static void hex_to_bytes(const uint8_t *hex, unsigned nhex, uint8_t *b)
{
    for (unsigned i = 0; i < nhex; i++) p25_bits_put(b, 6 * i, 6, hex[i]);
}

/* -------------------------------------------------------------- IMBE */
static const uint8_t il_base[6][2] = { { 0, 7 }, { 1, 6 }, { 2, 9 }, { 3, 8 }, { 4, 11 }, { 5, 10 } };

unsigned p25_imbe_il(unsigned j)
{
    unsigned row = j / 24, k = j % 24;
    return 12 * (k >> 1) + il_base[row][k & 1];
}

/* u vector widths and c vector widths */
static const uint8_t u_w[8] = { 12, 12, 12, 12, 11, 11, 11, 7 };
static const uint8_t c_w[8] = { 23, 23, 23, 23, 15, 15, 15, 7 };

void p25_imbe_encode(const uint8_t frame[P25_IMBE_BYTES], uint8_t air[18])
{
    uint32_t u[8], c[8];
    unsigned pos = 0;
    for (int i = 0; i < 8; i++) { u[i] = p25_bits_get(frame, pos, u_w[i]); pos += u_w[i]; }
    uint32_t pn = 16u * u[0];
    for (int i = 0; i < 8; i++) {
        if (i < 4) c[i] = p25_golay23_encode(u[i]);
        else if (i < 7) c[i] = p25_hamming15_encode(u[i]);
        else c[i] = u[i];
        if (i >= 1 && i <= 6) c[i] ^= p25_pn_bits(&pn, c_w[i]);
    }
    memset(air, 0, 18);
    unsigned j = 0;
    for (int i = 0; i < 8; i++)
        for (int b = c_w[i] - 1; b >= 0; b--, j++)
            p25_bit_put(air, p25_imbe_il(j), (c[i] >> b) & 1u);
}

int p25_imbe_decode(const uint8_t air[18], uint8_t frame[P25_IMBE_BYTES])
{
    uint32_t c[8] = { 0 }, u[8];
    unsigned j = 0;
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < c_w[i]; b++, j++)
            c[i] = (c[i] << 1) | p25_bit_get(air, p25_imbe_il(j));
    int errs = 0, e;
    u[0] = p25_golay23_decode(c[0], &e);
    errs += e;
    uint32_t pn = 16u * u[0];
    for (int i = 1; i < 8; i++) {
        if (i <= 6) c[i] ^= p25_pn_bits(&pn, c_w[i]);
        if (i < 4) u[i] = p25_golay23_decode(c[i], &e);
        else if (i < 7) u[i] = p25_hamming15_decode(c[i], &e);
        else { u[i] = c[i]; e = 0; }
        errs += e;
    }
    memset(frame, 0, P25_IMBE_BYTES);
    unsigned pos = 0;
    for (int i = 0; i < 8; i++) { p25_bits_put(frame, pos, u_w[i], u[i]); pos += u_w[i]; }
    return errs;
}

/* ---------------------------------------------------------- link control */
void p25_lc_group_voice(p25_lc_t *lc, uint8_t svc, uint16_t tgid, uint32_t src)
{
    memset(lc, 0, sizeof *lc);
    lc->b[0] = P25_LCO_GRP_V_USER;
    lc->b[2] = svc;
    lc->b[4] = (uint8_t)(tgid >> 8); lc->b[5] = (uint8_t)tgid;
    lc->b[6] = (uint8_t)(src >> 16); lc->b[7] = (uint8_t)(src >> 8); lc->b[8] = (uint8_t)src;
}

void p25_lc_unit_voice(p25_lc_t *lc, uint8_t svc, uint32_t dst, uint32_t src)
{
    memset(lc, 0, sizeof *lc);
    lc->b[0] = P25_LCO_UU_V_USER;
    lc->b[2] = svc;
    lc->b[3] = (uint8_t)(dst >> 16); lc->b[4] = (uint8_t)(dst >> 8); lc->b[5] = (uint8_t)dst;
    lc->b[6] = (uint8_t)(src >> 16); lc->b[7] = (uint8_t)(src >> 8); lc->b[8] = (uint8_t)src;
}

void p25_lc_parse(const p25_lc_t *lc, p25_lc_info_t *o)
{
    const uint8_t *b = lc->b;
    memset(o, 0, sizeof *o);
    o->protect = (b[0] & 0x80) != 0;
    o->sf = (b[0] & 0x40) != 0;
    o->lco = b[0] & 0x3F;
    o->mfid = b[1];
    if (o->mfid != 0x00 && o->mfid != 0x01 && !o->sf) return;   /* manufacturer specific */
    switch (o->lco) {
    case P25_LCO_GRP_V_USER:
        o->svc_opts = b[2];
        o->tgid = ((uint32_t)b[4] << 8) | b[5];
        o->src = ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | b[8];
        o->is_group = o->known = true;
        break;
    case P25_LCO_UU_V_USER:
        o->svc_opts = b[2];
        o->tgid = ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
        o->src = ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | b[8];
        o->known = true;
        break;
    case P25_LCO_CALL_TERM:
        o->src = ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | b[8];
        o->known = true;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- builders */
size_t p25_p1_build_hdu(uint8_t *air, uint16_t nac, const p25_hdu_t *h, uint8_t status)
{
    uint8_t b[15], hex[36];
    memcpy(b, h->mi, 9);
    b[9] = h->mfid; b[10] = h->algid;
    b[11] = (uint8_t)(h->kid >> 8); b[12] = (uint8_t)h->kid;
    b[13] = (uint8_t)(h->tgid >> 8); b[14] = (uint8_t)h->tgid;
    bytes_to_hex(b, 20, hex);
    p25_rs_encode(hex, 20, 16, hex + 20);
    air_header(air, P25_HDU_AIR, nac, P25_DUID_HDU);
    for (unsigned i = 0; i < 36; i++) air_put(air, 112 + 18 * i, 18, p25_golay18_encode(hex[i]));
    return air_finish(air, HDU_DATA_DIBITS, status);
}

static void put_ldu_common(uint8_t *air, const uint8_t imbe[9][P25_IMBE_BYTES], const uint8_t *hex24,
                           const uint8_t lsd[2])
{
    uint8_t v[18];
    for (unsigned k = 0; k < 9; k++) {
        p25_imbe_encode(imbe[k], v);
        for (unsigned j = 0; j < 144; j++) air_put(air, ldu_voice_off[k] + j, 1, p25_bit_get(v, j));
    }
    for (unsigned i = 0; i < 24; i++)
        air_put(air, ldu_lc_off[i / 4] + 10 * (i % 4), 10, p25_hamming10_encode(hex24[i]));
    air_put(air, LDU_LSD_OFF, 16, p25_cyclic16_encode(lsd ? lsd[0] : 0));
    air_put(air, LDU_LSD_OFF + 16, 16, p25_cyclic16_encode(lsd ? lsd[1] : 0));
}

size_t p25_p1_build_ldu1(uint8_t *air, uint16_t nac, const uint8_t imbe[9][P25_IMBE_BYTES],
                         const p25_lc_t *lc, const uint8_t lsd[2], uint8_t status)
{
    uint8_t hex[24];
    bytes_to_hex(lc->b, 12, hex);
    p25_rs_encode(hex, 12, 12, hex + 12);
    air_header(air, P25_LDU_AIR, nac, P25_DUID_LDU1);
    put_ldu_common(air, imbe, hex, lsd);
    return air_finish(air, LDU_DATA_DIBITS, status);
}

size_t p25_p1_build_ldu2(uint8_t *air, uint16_t nac, const uint8_t imbe[9][P25_IMBE_BYTES],
                         const p25_es_t *es, const uint8_t lsd[2], uint8_t status)
{
    uint8_t b[12], hex[24];
    memcpy(b, es->mi, 9);
    b[9] = es->algid; b[10] = (uint8_t)(es->kid >> 8); b[11] = (uint8_t)es->kid;
    bytes_to_hex(b, 16, hex);
    p25_rs_encode(hex, 16, 8, hex + 16);
    air_header(air, P25_LDU_AIR, nac, P25_DUID_LDU2);
    put_ldu_common(air, imbe, hex, lsd);
    return air_finish(air, LDU_DATA_DIBITS, status);
}

size_t p25_p1_build_tdu(uint8_t *air, uint16_t nac, uint8_t status)
{
    air_header(air, P25_TDU_AIR, nac, P25_DUID_TDU);
    return air_finish(air, 70, status);
}

size_t p25_p1_build_tdulc(uint8_t *air, uint16_t nac, const p25_lc_t *lc, uint8_t status)
{
    uint8_t hex[24];
    bytes_to_hex(lc->b, 12, hex);
    p25_rs_encode(hex, 12, 12, hex + 12);
    air_header(air, P25_TDULC_AIR, nac, P25_DUID_TDULC);
    for (unsigned i = 0; i < 12; i++)
        air_put(air, 112 + 24 * i, 24, p25_golay24_encode(((uint32_t)hex[2 * i] << 6) | hex[2 * i + 1]));
    return air_finish(air, TDULC_DATA_DIBITS, status);
}

static void air_put_dibits(uint8_t *air, unsigned d0, const uint8_t *dib, unsigned n)
{
    for (unsigned i = 0; i < n; i++) air_put(air, 2 * (d0 + i), 2, dib[i]);
}

size_t p25_p1_build_tsbk(uint8_t *air, uint16_t nac, const uint8_t blocks[][10], unsigned n, uint8_t status)
{
    if (n < 1) n = 1;
    if (n > 3) n = 3;
    air_header(air, P25_TSBK_AIR(n), nac, P25_DUID_TSBK);
    for (unsigned i = 0; i < n; i++) {
        uint8_t t[12], d[98];
        memcpy(t, blocks[i], 10);
        if (i == n - 1) t[0] |= 0x80; else t[0] &= 0x7F;
        uint16_t crc = p25_crc16(t, 80);
        t[10] = (uint8_t)(crc >> 8); t[11] = (uint8_t)crc;
        p25_trellis12_encode(t, d);
        air_put_dibits(air, 56 + 98 * i, d, 98);
    }
    return air_finish(air, 56 + 98 * n, status);
}

/* ------------------------------------------------------------------ PDU */
void p25_pdu_hdr_pack(const p25_pdu_hdr_t *h, uint8_t o[12])
{
    o[0] = (uint8_t)((h->confirmed ? 0x40 : 0) | (h->outbound ? 0x20 : 0) | (h->format & 0x1F));
    o[1] = (uint8_t)(0xC0 | (h->sap & 0x3F));
    o[2] = h->mfid;
    o[3] = (uint8_t)(h->llid >> 16); o[4] = (uint8_t)(h->llid >> 8); o[5] = (uint8_t)h->llid;
    o[6] = (uint8_t)((h->fmf ? 0x80 : 0) | (h->blocks & 0x7F));
    o[7] = h->pad & 0x1F;
    o[8] = h->ns;
    o[9] = h->offset & 0x3F;
    uint16_t crc = p25_crc16(o, 80);
    o[10] = (uint8_t)(crc >> 8); o[11] = (uint8_t)crc;
}

bool p25_pdu_hdr_unpack(const uint8_t in[12], p25_pdu_hdr_t *h)
{
    h->confirmed = (in[0] & 0x40) != 0;
    h->outbound = (in[0] & 0x20) != 0;
    h->format = in[0] & 0x1F;
    h->sap = in[1] & 0x3F;
    h->mfid = in[2];
    h->llid = ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 8) | in[5];
    h->fmf = (in[6] & 0x80) != 0;
    h->blocks = in[6] & 0x7F;
    h->pad = in[7] & 0x1F;
    h->ns = in[8];
    h->offset = in[9] & 0x3F;
    return p25_crc16(in, 80) == (((uint16_t)in[10] << 8) | in[11]);
}

static unsigned pdu_nblocks(size_t len, bool conf)
{
    unsigned per = conf ? 16 : 12;
    return (unsigned)((len + 4 + per - 1) / per);
}

size_t p25_p1_pdu_air(size_t len, bool conf)
{
    unsigned nb = pdu_nblocks(len, conf);
    if (nb > 127) return 0;
    size_t dd = 56 + 98 * (1 + (size_t)nb);
    return (dd + 34) / 35 * 36;
}

size_t p25_p1_build_pdu(uint8_t *air, uint16_t nac, const p25_pdu_hdr_t *tmpl, const uint8_t *data,
                        size_t len, uint8_t status)
{
    bool conf = tmpl->format == P25_PDU_FMT_CONF;
    unsigned per = conf ? 16 : 12, nb = pdu_nblocks(len, conf);
    if (nb > 127) return 0;
    size_t air_len = p25_p1_pdu_air(len, conf);
    p25_pdu_hdr_t h = *tmpl;
    h.confirmed = conf;
    h.blocks = (uint8_t)nb;
    h.pad = (uint8_t)(nb * per - 4 - len);
    h.fmf = true;
    uint8_t blk[18], d[98];
    air_header(air, air_len, nac, P25_DUID_PDU);
    p25_pdu_hdr_pack(&h, blk);
    p25_trellis12_encode(blk, d);
    air_put_dibits(air, 56, d, 98);
    /* user data + pad (zeros) + CRC-32 over data+pad */
    size_t total = (size_t)nb * per;          /* octets incl. CRC */
    uint32_t crc = 0;
    {
        /* CRC-32 over data+pad, computed bitwise without a big buffer */
        uint32_t c = 0;
        for (size_t i = 0; i < total - 4; i++) {
            uint8_t byte = i < len ? data[i] : 0;
            for (int b = 7; b >= 0; b--) {
                uint32_t fb = ((c >> 31) & 1u) ^ ((byte >> b) & 1u);
                c <<= 1;
                if (fb) c ^= 0x04C11DB7u;
            }
        }
        crc = ~c;
    }
    for (unsigned k = 0; k < nb; k++) {
        uint8_t payload[16];
        for (unsigned i = 0; i < per; i++) {
            size_t o = (size_t)k * per + i;
            if (o < len) payload[i] = data[o];
            else if (o < total - 4) payload[i] = 0;
            else payload[i] = (uint8_t)(crc >> (8 * (3 - (o - (total - 4)))));
        }
        if (conf) {
            memset(blk, 0, sizeof blk);
            blk[0] = (uint8_t)((k & 0x7F) << 1);
            memcpy(blk + 2, payload, 16);
            /* CRC-9 over serial (7 bits) + 128 data bits */
            uint8_t tmp[17];
            memset(tmp, 0, sizeof tmp);
            p25_bits_put(tmp, 0, 7, k & 0x7F);
            for (unsigned i = 0; i < 128; i++) p25_bit_put(tmp, 7 + i, p25_bit_get(payload, i));
            uint16_t c9 = p25_crc9(tmp, 135);
            p25_bits_put(blk, 7, 9, c9);
            p25_trellis34_encode(blk, d);
        } else {
            p25_trellis12_encode(payload, d);
        }
        air_put_dibits(air, 56 + 98 * (1 + k), d, 98);
    }
    return air_finish(air, 56 + 98 * (1 + (size_t)nb), status);
}

/* ------------------------------------------------------------------- RX */
enum { RX_HUNT = 0, RX_FRAME = 1 };

void p25_p1_rx_init(p25_p1_rx_t *rx, uint16_t nac_filter, const p25_p1_cb_t *cb, void *ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->nac_filter = nac_filter;
    rx->sync_max_err = 4;
    rx->cb = cb;
    rx->ctx = ctx;
}

void p25_p1_rx_reset(p25_p1_rx_t *rx)
{
    rx->state = RX_HUNT;
    rx->sreg = 0;
}

static void ev_init(p25_p1_rx_t *rx, p25_p1_event_t *ev, p25_p1_ev_type_t t)
{
    memset(ev, 0, sizeof *ev);
    ev->type = t;
    ev->nac = rx->nac;
    ev->duid = rx->duid;
    ev->errs = rx->errs;
    ev->rssi = rx->rssi_n ? (int16_t)(rx->rssi_sum / rx->rssi_n) : 0;
    ev->status = rx->status;
    ev->inverted = rx->inverted;
    ev->crypt = rx->vc;
}

void p25_p1_rx_set_crypt(p25_p1_rx_t *rx, const p25_keyring_t *keys, p25_crypt_t *crypt)
{
    rx->keys = keys;
    rx->crypt = crypt;
    rx->vc = P25_VC_CLEAR;
}

/* new ALGID/KID/MI for the next voice frames */
static void crypt_set(p25_p1_rx_t *rx, uint8_t algid, uint16_t kid, const uint8_t mi[9])
{
    rx->enc_algid = algid;
    rx->enc_kid = kid;
    if (algid == P25_ALG_CLEAR) { rx->vc = P25_VC_CLEAR; return; }
    if (rx->crypt && rx->keys && p25_crypt_prepare(rx->crypt, rx->keys, algid, kid, mi, false))
        rx->vc = P25_VC_DECRYPTED;
    else
        rx->vc = P25_VC_NOKEY;
}

static void emit(p25_p1_rx_t *rx, p25_p1_event_t *ev)
{
    if (rx->cb && rx->cb->on_event) rx->cb->on_event(rx->ctx, ev);
}

static void rx_voice(p25_p1_rx_t *rx, unsigned k)
{
    uint8_t v[18], f[P25_IMBE_BYTES];
    for (unsigned j = 0; j < 18; j++) v[j] = 0;
    for (unsigned j = 0; j < 144; j++) p25_bit_put(v, j, p25_dbit_get(rx->d, ldu_voice_off[k] + j));
    int e = p25_imbe_decode(v, f);
    rx->errs += e;
    if (rx->vc == P25_VC_NOKEY || rx->vc == P25_VC_WAIT_MI) return;          /* muted */
    if (rx->vc == P25_VC_DECRYPTED) p25_crypt_imbe(rx->crypt, rx->duid == P25_DUID_LDU2, k, f);
    if (rx->cb && rx->cb->on_voice) rx->cb->on_voice(rx->ctx, f, e);
}

/* decode 24 Hamming(10,6) hex words + RS; returns true when RS ok */
static bool rx_ldu_hex(p25_p1_rx_t *rx, unsigned k_data, unsigned nroots, uint8_t *bytes)
{
    uint8_t hex[24], eras[24];
    unsigned ne = 0;
    for (unsigned i = 0; i < 24; i++) {
        int e;
        hex[i] = (uint8_t)p25_hamming10_decode(p25_dbits_get(rx->d, ldu_lc_off[i / 4] + 10 * (i % 4), 10), &e);
        if (e > 1 && ne < nroots) eras[ne++] = (uint8_t)i;
        rx->errs += e > 1 ? 1 : e;
    }
    int r = p25_rs_decode(hex, 24, nroots, eras, ne);
    if (r < 0) return false;
    rx->errs += r;
    hex_to_bytes(hex, k_data, bytes);
    return true;
}

static void rx_lsd(p25_p1_rx_t *rx, uint8_t lsd[2])
{
    for (int i = 0; i < 2; i++) {
        int v = p25_cyclic16_decode(p25_dbits_get(rx->d, LDU_LSD_OFF + 16 * (unsigned)i, 16), NULL);
        lsd[i] = v < 0 ? 0 : (uint8_t)v;
    }
}

static void rx_hdu(p25_p1_rx_t *rx)
{
    uint8_t hex[36], eras[36], b[15];
    unsigned ne = 0;
    for (unsigned i = 0; i < 36; i++) {
        int e;
        int v = p25_golay18_decode(p25_dbits_get(rx->d, 112 + 18 * i, 18), &e);
        if (v < 0) { hex[i] = 0; if (ne < 16) eras[ne++] = (uint8_t)i; }
        else { hex[i] = (uint8_t)v; rx->errs += e; }
    }
    p25_p1_event_t ev;
    int r = p25_rs_decode(hex, 36, 16, eras, ne);
    if (r < 0) { ev_init(rx, &ev, P25_P1_EV_NID_FAIL); emit(rx, &ev); return; }
    rx->errs += r;
    hex_to_bytes(hex, 20, b);
    ev_init(rx, &ev, P25_P1_EV_HDU);
    memcpy(ev.u.hdu.mi, b, 9);
    ev.u.hdu.mfid = b[9];
    ev.u.hdu.algid = b[10];
    ev.u.hdu.kid = (uint16_t)((b[11] << 8) | b[12]);
    ev.u.hdu.tgid = (uint16_t)((b[13] << 8) | b[14]);
    crypt_set(rx, ev.u.hdu.algid, ev.u.hdu.kid, ev.u.hdu.mi);
    ev.crypt = rx->vc;
    emit(rx, &ev);
}

static void rx_tdulc(p25_p1_rx_t *rx)
{
    uint8_t hex[24], eras[24];
    unsigned ne = 0;
    for (unsigned i = 0; i < 12; i++) {
        int e;
        int v = p25_golay24_decode(p25_dbits_get(rx->d, 112 + 24 * i, 24), &e);
        if (v < 0) { hex[2 * i] = hex[2 * i + 1] = 0; eras[ne++] = (uint8_t)(2 * i); eras[ne++] = (uint8_t)(2 * i + 1); }
        else { hex[2 * i] = (uint8_t)(v >> 6); hex[2 * i + 1] = (uint8_t)(v & 63); rx->errs += e; }
    }
    p25_p1_event_t ev;
    ev_init(rx, &ev, P25_P1_EV_TDULC);
    int r = ne <= 12 ? p25_rs_decode(hex, 24, 12, eras, ne) : -1;
    ev.u.tdulc.lc_ok = r >= 0;
    if (r >= 0) { ev.errs += r; hex_to_bytes(hex, 12, ev.u.tdulc.lc.b); }
    emit(rx, &ev);
}

static void rx_ldu_end(p25_p1_rx_t *rx)
{
    p25_p1_event_t ev;
    if (rx->duid == P25_DUID_LDU1) {
        ev_init(rx, &ev, P25_P1_EV_LDU1);
        ev.u.ldu1.lc_ok = rx_ldu_hex(rx, 12, 12, ev.u.ldu1.lc.b);
        rx_lsd(rx, ev.u.ldu1.lsd);
        if (ev.u.ldu1.lc_ok) {
            p25_lc_info_t li;
            p25_lc_parse(&ev.u.ldu1.lc, &li);
            /* late entry into an encrypted call: mute until the ES gives the MI */
            if (li.known && (li.svc_opts & 0x40) && rx->vc == P25_VC_CLEAR) rx->vc = P25_VC_WAIT_MI;
        }
    } else {
        uint8_t b[12];
        ev_init(rx, &ev, P25_P1_EV_LDU2);
        ev.u.ldu2.es_ok = rx_ldu_hex(rx, 16, 8, b);
        if (ev.u.ldu2.es_ok) {
            memcpy(ev.u.ldu2.es.mi, b, 9);
            ev.u.ldu2.es.algid = b[9];
            ev.u.ldu2.es.kid = (uint16_t)((b[10] << 8) | b[11]);
        }
        rx_lsd(rx, ev.u.ldu2.lsd);
    }
    ev.errs = rx->errs;
    emit(rx, &ev);
    /* the LDU2 ES carries the MI of the next superframe */
    if (rx->duid == P25_DUID_LDU2 && ev.u.ldu2.es_ok)
        crypt_set(rx, ev.u.ldu2.es.algid, ev.u.ldu2.es.kid, ev.u.ldu2.es.mi);
}

/* returns true when the frame is complete */
static bool rx_tsbk_block(p25_p1_rx_t *rx)
{
    p25_p1_event_t ev;
    ev_init(rx, &ev, P25_P1_EV_TSBK);
    int m = p25_trellis12_decode(rx->d + 56 + 98 * rx->blk, ev.u.tsbk.tsbk);
    const uint8_t *t = ev.u.tsbk.tsbk;
    ev.u.tsbk.crc_ok = p25_crc16(t, 80) == (((uint16_t)t[10] << 8) | t[11]);
    ev.u.tsbk.index = rx->blk;
    ev.errs = m;
    emit(rx, &ev);
    rx->blk++;
    /* last block flag, or a garbage block: stop */
    return (t[0] & 0x80) || !ev.u.tsbk.crc_ok || rx->blk >= 3;
}

static bool rx_pdu_block(p25_p1_rx_t *rx)
{
    const uint8_t *dib = rx->d + 56;
    if (rx->blk == 0) {
        uint8_t h[12];
        p25_trellis12_decode(dib, h);
        rx->blk = 1;
        if (!p25_pdu_hdr_unpack(h, &rx->pdu_hdr)) {
            p25_p1_event_t ev;
            ev_init(rx, &ev, P25_P1_EV_PDU);
            ev.u.pdu.hdr = rx->pdu_hdr;
            ev.u.pdu.crc_ok = false;
            emit(rx, &ev);
            return true;
        }
        rx->pdu_blocks = rx->pdu_hdr.blocks;
        rx->pdu_len = 0;
        return rx->pdu_blocks == 0;
    }
    bool conf = rx->pdu_hdr.format == P25_PDU_FMT_CONF;
    uint8_t blk[18];
    const uint8_t *payload;
    unsigned per;
    if (conf) {
        p25_trellis34_decode(dib, blk);
        payload = blk + 2;
        per = 16;
    } else {
        p25_trellis12_decode(dib, blk);
        payload = blk;
        per = 12;
    }
    if (rx->pdu_len + per <= sizeof rx->pdu) {
        memcpy(rx->pdu + rx->pdu_len, payload, per);
    }
    rx->pdu_len = (uint16_t)(rx->pdu_len + per);
    rx->blk++;
    if (rx->blk <= rx->pdu_blocks) return false;

    p25_p1_event_t ev;
    ev_init(rx, &ev, P25_P1_EV_PDU);
    ev.u.pdu.hdr = rx->pdu_hdr;
    if (rx->pdu_len <= sizeof rx->pdu && rx->pdu_len >= 4u + rx->pdu_hdr.pad) {
        unsigned n = rx->pdu_len - 4u;
        uint32_t want = ((uint32_t)rx->pdu[n] << 24) | ((uint32_t)rx->pdu[n + 1] << 16) |
                        ((uint32_t)rx->pdu[n + 2] << 8) | rx->pdu[n + 3];
        ev.u.pdu.crc_ok = p25_crc32(rx->pdu, n * 8) == want;
        ev.u.pdu.data = rx->pdu;
        ev.u.pdu.len = n - rx->pdu_hdr.pad;
    }
    emit(rx, &ev);
    return true;
}

static void rx_start_frame(p25_p1_rx_t *rx, bool inv)
{
    rx->state = RX_FRAME;
    rx->inverted = inv;
    rx->air_idx = 24;
    rx->d_idx = 24;
    rx->errs = 0;
    rx->rssi_sum = 0;
    rx->rssi_n = 0;
    rx->blk = 0;
    rx->next_voice = 0;
    rx->status = 0;
}

static const uint64_t SYNC_INV_MASK = 0xAAAAAAAAAAAAULL;

/* process one data dibit in a frame; returns true when frame complete */
static bool rx_data(p25_p1_rx_t *rx)
{
    unsigned d = rx->d_idx;
    if (d == 56) {
        uint64_t nid = 0;
        for (unsigned i = 24; i < 56; i++) nid = (nid << 2) | rx->d[i];
        uint16_t nac; uint8_t duid; int e;
        p25_p1_event_t ev;
        if (p25_nid_decode(nid, &nac, &duid, &e) < 0) {
            ev_init(rx, &ev, P25_P1_EV_NID_FAIL);
            emit(rx, &ev);
            return true;
        }
        rx->nac = nac;
        rx->duid = duid;
        rx->errs = e;
        if (rx->nac_filter != P25_NAC_ANY && nac != rx->nac_filter) return true;
        ev_init(rx, &ev, P25_P1_EV_SYNC);
        emit(rx, &ev);
        switch (duid) {
        case P25_DUID_TDU:
            rx->vc = P25_VC_CLEAR;
            ev_init(rx, &ev, P25_P1_EV_TDU);
            emit(rx, &ev);
            return true;
        case P25_DUID_HDU: case P25_DUID_LDU1: case P25_DUID_LDU2: case P25_DUID_TSBK:
        case P25_DUID_PDU: case P25_DUID_TDULC:
            return false;
        default:
            return true;
        }
    }
    if (d < 56) return false;
    switch (rx->duid) {
    case P25_DUID_HDU:
        if (d == HDU_DATA_DIBITS) { rx_hdu(rx); return true; }
        return false;
    case P25_DUID_TDULC:
        if (d == TDULC_DATA_DIBITS) { rx->vc = P25_VC_CLEAR; rx_tdulc(rx); return true; }
        return false;
    case P25_DUID_LDU1: case P25_DUID_LDU2:
        if (rx->next_voice < 9 && d * 2 == (unsigned)ldu_voice_off[rx->next_voice] + 144)
            rx_voice(rx, rx->next_voice++);
        if (d == LDU_DATA_DIBITS) { rx_ldu_end(rx); return true; }
        return false;
    case P25_DUID_TSBK:
        if (d == 56 + 98 * ((unsigned)rx->blk + 1)) return rx_tsbk_block(rx);
        return false;
    case P25_DUID_PDU:
        if (d == 56 + 98) {
            bool done = rx_pdu_block(rx);
            rx->d_idx = 56;                      /* reuse the block buffer */
            return done;
        }
        return false;
    default:
        return true;
    }
}

void p25_p1_rx_push(p25_p1_rx_t *rx, const uint8_t *dib, size_t n, int16_t rssi)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t raw = dib[i] & 3;
        rx->sreg = ((rx->sreg << 2) | raw) & 0xFFFFFFFFFFFFULL;
        unsigned e = p25_popcount64(rx->sreg ^ P25_SYNC_P1);
        unsigned ei = p25_popcount64(rx->sreg ^ P25_SYNC_P1 ^ SYNC_INV_MASK);
        if (rx->state == RX_HUNT) {
            if (e <= rx->sync_max_err) rx_start_frame(rx, false);
            else if (ei <= rx->sync_max_err) rx_start_frame(rx, true);
            continue;
        }
        /* inside a frame: a clean sync restarts (lost frame boundary) */
        if (rx->air_idx >= 72 && (e <= 1 || ei <= 1)) {
            rx_start_frame(rx, ei <= 1 && e > 1);
            continue;
        }
        uint8_t x = rx->inverted ? (uint8_t)(raw ^ 2) : raw;
        rx->rssi_sum += rssi;
        rx->rssi_n++;
        if (rx->air_idx % 36 == 35) {
            rx->status = x;
            rx->air_idx++;
            continue;
        }
        rx->air_idx++;
        if (rx->d_idx < sizeof rx->d) rx->d[rx->d_idx] = x;
        rx->d_idx++;
        if (rx_data(rx)) rx->state = RX_HUNT;
    }
}

/* -------------------------------------------------------- voice TX helper */
size_t p25_p1_voice_tx_start(p25_p1_voice_tx_t *tx, uint8_t *air, uint16_t nac, const p25_lc_t *lc,
                             uint8_t status)
{
    memset(tx, 0, sizeof *tx);
    tx->nac = nac;
    tx->status = status;
    tx->lc = *lc;
    tx->es.algid = 0x80;
    p25_hdu_t h;
    memset(&h, 0, sizeof h);
    h.algid = 0x80;
    h.mfid = lc->b[1];
    p25_lc_info_t li;
    p25_lc_parse(lc, &li);
    h.tgid = li.is_group ? (uint16_t)li.tgid : 0;
    return p25_p1_build_hdu(air, nac, &h, status);
}

size_t p25_p1_voice_tx_start_enc(p25_p1_voice_tx_t *tx, uint8_t *air, uint16_t nac, const p25_lc_t *lc,
                                 uint8_t status, p25_crypt_t *crypt, const p25_keyring_t *keys,
                                 uint8_t algid, uint16_t kid, const uint8_t mi[9])
{
    if (!crypt || !p25_crypt_prepare(crypt, keys, algid, kid, mi, false)) return 0;
    memset(tx, 0, sizeof *tx);
    tx->nac = nac;
    tx->status = status;
    tx->lc = *lc;
    if ((lc->b[0] & 0x3F) == P25_LCO_GRP_V_USER || (lc->b[0] & 0x3F) == P25_LCO_UU_V_USER)
        tx->lc.b[2] |= 0x40;                                  /* service option: encrypted */
    tx->crypt = crypt;
    tx->keys = keys;
    memcpy(tx->mi, mi, 9);
    tx->es.algid = algid;
    tx->es.kid = kid;
    p25_hdu_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.mi, mi, 9);
    h.algid = algid;
    h.kid = kid;
    h.mfid = lc->b[1];
    p25_lc_info_t li;
    p25_lc_parse(lc, &li);
    h.tgid = li.is_group ? (uint16_t)li.tgid : 0;
    return p25_p1_build_hdu(air, nac, &h, status);
}

size_t p25_p1_voice_tx_push(p25_p1_voice_tx_t *tx, uint8_t *air, const uint8_t imbe[P25_IMBE_BYTES])
{
    memcpy(tx->imbe[tx->n], imbe, P25_IMBE_BYTES);
    if (tx->crypt) p25_crypt_imbe(tx->crypt, tx->ldu2_next, tx->n, tx->imbe[tx->n]);
    if (++tx->n < 9) return 0;
    tx->n = 0;
    size_t r;
    if (!tx->ldu2_next) {
        r = p25_p1_build_ldu1(air, tx->nac, (const uint8_t(*)[P25_IMBE_BYTES])tx->imbe, &tx->lc, tx->lsd, tx->status);
    } else {
        if (tx->crypt) {                                       /* ES = MI of the next superframe */
            memcpy(tx->es.mi, tx->mi, 9);
            p25_mi_next(tx->es.mi);
        }
        r = p25_p1_build_ldu2(air, tx->nac, (const uint8_t(*)[P25_IMBE_BYTES])tx->imbe, &tx->es, tx->lsd + 2, tx->status);
        if (tx->crypt) {
            memcpy(tx->mi, tx->es.mi, 9);
            p25_crypt_prepare(tx->crypt, tx->keys, tx->es.algid, tx->es.kid, tx->mi, false);
        }
    }
    tx->ldu2_next = !tx->ldu2_next;
    return r;
}

size_t p25_p1_voice_tx_end(p25_p1_voice_tx_t *tx, uint8_t *air, const uint8_t silence[P25_IMBE_BYTES])
{
    size_t r = 0;
    if (tx->n && silence)
        while (!r) r = p25_p1_voice_tx_push(tx, air, silence);
    tx->n = 0;
    return r + p25_p1_build_tdu(air + r, tx->nac, tx->status);
}
