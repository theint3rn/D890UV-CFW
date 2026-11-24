/*
 * ysf_fec.c - YSF channel coding (Yaesu spec rev 1.02 section 4.5 and
 * appendices A/B).
 *
 *  FICH: 32 bits + CRC16 (x^16+x^12+x^5+1, registers 0, inverted) = 48 ->
 *        4 x Golay(24,12,8) = 96 -> + 4 zero bits -> trellis R=1/2 K=5
 *        (G1 = x^4+x^3+1, G2 = x^4+x^2+x+1, output G1 then G2) = 200 ->
 *        dibits written in rows of M=5, N=20 rows, read by column.
 *  DCH:  unit (160 or 80 bits) -> PN(9,5) whitening (register S08..S00 =
 *        1 1100 1001, new per unit) -> CRC16 -> + 4 zero bits -> trellis ->
 *        dibit interleave M=9/N=20 (360 bits) or M=5/N=20 (200 bits).
 *  V/D 2 voice: 49 bits -> first 27 bits each sent 3 times (81) + 22 bits +
 *        one 0 bit = 104 -> whitening -> bits written in 4 rows of 26, read by
 *        column -> VCH (first 72) + VeCH (last 32).
 */
#include <string.h>

#include "../common/dv_common.h"
#include "ysf.h"

static const dv_conv_t YSF_CONV = {5, 0x19, 0x17};

/* Appendix A generator matrix, parity part of the rows for d11..d0. */
static const uint16_t GOLAY_ROWS[12] = {
    0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB,
};

static uint16_t golay_parity(uint16_t d)
{
    uint16_t p = 0;
    for (int i = 0; i < 12; i++)
        if (d & (1u << (11 - i)))
            p ^= GOLAY_ROWS[i];
    return p;
}

uint32_t ysf_golay24_encode(uint16_t data12)
{
    data12 &= 0xFFFu;
    return ((uint32_t)data12 << 12) | golay_parity(data12);
}

/* syndrome contribution of codeword bit b (0..23, bit 23 = d11) */
static uint16_t col_syn(unsigned b)
{
    return b >= 12 ? GOLAY_ROWS[23 - b] : (uint16_t)(1u << b);
}

int ysf_golay24_decode(uint32_t w, int *nerr)
{
    uint16_t d = (uint16_t)((w >> 12) & 0xFFFu);
    uint16_t s = (uint16_t)(golay_parity(d) ^ (w & 0xFFFu));
    if (nerr)
        *nerr = 0;
    if (s == 0)
        return d;
    /* error patterns of weight <= 3, smallest weight first */
    for (unsigned a = 0; a < 24; a++) {
        uint16_t sa = col_syn(a);
        if (sa == s) {
            if (nerr)
                *nerr = 1;
            return (int)((w ^ (1ul << a)) >> 12) & 0xFFF;
        }
    }
    for (unsigned a = 0; a < 24; a++)
        for (unsigned b = a + 1; b < 24; b++)
            if ((col_syn(a) ^ col_syn(b)) == s) {
                if (nerr)
                    *nerr = 2;
                return (int)((w ^ (1ul << a) ^ (1ul << b)) >> 12) & 0xFFF;
            }
    for (unsigned a = 0; a < 24; a++)
        for (unsigned b = a + 1; b < 24; b++) {
            uint16_t sab = col_syn(a) ^ col_syn(b);
            for (unsigned c = b + 1; c < 24; c++)
                if ((sab ^ col_syn(c)) == s) {
                    if (nerr)
                        *nerr = 3;
                    return (int)((w ^ (1ul << a) ^ (1ul << b) ^ (1ul << c)) >> 12) & 0xFFF;
                }
        }
    return -1;
}

void ysf_whiten(uint8_t *bits, size_t n)
{
    unsigned s = 0x1C9u; /* S08..S00 = 1 1100 1001 */
    for (size_t i = 0; i < n; i++) {
        unsigned out = s & 1u;
        unsigned fb = (s ^ (s >> 4)) & 1u;
        s = (s >> 1) | (fb << 8);
        bits[i] ^= (uint8_t)out;
    }
}

/* Dibit block interleave: written in rows of m dibits (nrows rows), read by
 * column. Air dibit k <- code dibit (k % nrows) * m + k / nrows. */
void ysf_interleave_dibits(const uint8_t *in, uint8_t *out, unsigned m, unsigned nrows, int inverse)
{
    unsigned nd = m * nrows;
    for (unsigned k = 0; k < nd; k++) {
        unsigned src = (k % nrows) * m + k / nrows;
        if (!inverse) {
            out[2 * k] = in[2 * src];
            out[2 * k + 1] = in[2 * src + 1];
        } else {
            out[2 * src] = in[2 * k];
            out[2 * src + 1] = in[2 * k + 1];
        }
    }
}

/* ---- FICH ------------------------------------------------------------------ */

uint32_t ysf_fich_pack(const ysf_fich_t *f)
{
    uint32_t v = 0;
    v |= (uint32_t)(f->fi & 3u) << 30;
    v |= (uint32_t)(f->cs & 3u) << 28;
    v |= (uint32_t)(f->cm & 3u) << 26;
    v |= (uint32_t)(f->bn & 3u) << 24;
    v |= (uint32_t)(f->bt & 3u) << 22;
    v |= (uint32_t)(f->fn & 7u) << 19;
    v |= (uint32_t)(f->ft & 7u) << 16;
    v |= (uint32_t)(f->rsv & 1u) << 15;
    v |= (uint32_t)(f->dev & 1u) << 14;
    v |= (uint32_t)(f->mr & 7u) << 11;
    v |= (uint32_t)(f->voip & 1u) << 10;
    v |= (uint32_t)(f->dt & 3u) << 8;
    v |= (uint32_t)(f->sql & 1u) << 7;
    v |= (uint32_t)(f->sc & 0x7Fu);
    return v;
}

void ysf_fich_unpack(uint32_t v, ysf_fich_t *f)
{
    f->fi = (uint8_t)((v >> 30) & 3u);
    f->cs = (uint8_t)((v >> 28) & 3u);
    f->cm = (uint8_t)((v >> 26) & 3u);
    f->bn = (uint8_t)((v >> 24) & 3u);
    f->bt = (uint8_t)((v >> 22) & 3u);
    f->fn = (uint8_t)((v >> 19) & 7u);
    f->ft = (uint8_t)((v >> 16) & 7u);
    f->rsv = (uint8_t)((v >> 15) & 1u);
    f->dev = (uint8_t)((v >> 14) & 1u);
    f->mr = (uint8_t)((v >> 11) & 7u);
    f->voip = (uint8_t)((v >> 10) & 1u);
    f->dt = (uint8_t)((v >> 8) & 3u);
    f->sql = (uint8_t)((v >> 7) & 1u);
    f->sc = (uint8_t)(v & 0x7Fu);
}

void ysf_fich_encode(const ysf_fich_t *f, uint8_t bits[200])
{
    uint8_t b[6], in[100], code[200];
    uint32_t v = ysf_fich_pack(f);
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
    uint16_t crc = dv_crc16_ysf(b, 4);
    b[4] = (uint8_t)(crc >> 8);
    b[5] = (uint8_t)crc;
    for (int g = 0; g < 4; g++) {
        unsigned bit0 = 12u * (unsigned)g;
        uint16_t d = 0;
        for (unsigned i = 0; i < 12; i++) {
            unsigned p = bit0 + i;
            d = (uint16_t)((d << 1) | ((b[p >> 3] >> (7 - (p & 7))) & 1u));
        }
        uint32_t cw = ysf_golay24_encode(d);
        for (int i = 0; i < 24; i++)
            in[24 * g + i] = (uint8_t)((cw >> (23 - i)) & 1u);
    }
    memset(in + 96, 0, 4);
    dv_conv_encode(&YSF_CONV, in, 100, code);
    ysf_interleave_dibits(code, bits, 5, 20, 0);
}

bool ysf_fich_decode(const uint8_t bits[200], ysf_fich_t *f, int *errs)
{
    uint8_t code[200], dec[100], b[6];
    ysf_interleave_dibits(bits, code, 5, 20, 1);
    int e = dv_viterbi_decode(&YSF_CONV, code, 100, 0, dec);
    memset(b, 0, sizeof(b));
    bool ok = true;
    for (int g = 0; g < 4; g++) {
        uint32_t w = 0;
        for (int i = 0; i < 24; i++)
            w = (w << 1) | dec[24 * g + i];
        int ne = 0;
        int d = ysf_golay24_decode(w, &ne);
        if (d < 0) {
            ok = false;
            d = (int)(w >> 12);
        }
        e += ne;
        for (unsigned i = 0; i < 12; i++) {
            unsigned p = 12u * (unsigned)g + i;
            if ((unsigned)d & (1u << (11 - i)))
                b[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
        }
    }
    uint16_t crc = dv_crc16_ysf(b, 4);
    if (b[4] != (uint8_t)(crc >> 8) || b[5] != (uint8_t)crc)
        ok = false;
    ysf_fich_unpack(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3], f);
    if (errs)
        *errs = e;
    return ok;
}

/* ---- DCH ------------------------------------------------------------------- */

void ysf_dch_encode(const uint8_t *data, unsigned nbytes, uint8_t *bits)
{
    uint8_t w[22], in[180], code[360];
    unsigned nb = nbytes * 8u;
    dv_unpack_msb(data, nb, in);
    ysf_whiten(in, nb);
    dv_pack_msb(in, nb, w);
    uint16_t crc = dv_crc16_ysf(w, nbytes);
    w[nbytes] = (uint8_t)(crc >> 8);
    w[nbytes + 1] = (uint8_t)crc;
    dv_unpack_msb(w, nb + 16u, in);
    memset(in + nb + 16u, 0, 4);
    unsigned n = nb + 20u;
    dv_conv_encode(&YSF_CONV, in, n, code);
    ysf_interleave_dibits(code, bits, nbytes == 20 ? 9 : 5, 20, 0);
}

bool ysf_dch_decode(const uint8_t *bits, unsigned nbytes, uint8_t *data, int *errs)
{
    uint8_t code[360], dec[180], w[22];
    unsigned nb = nbytes * 8u, n = nb + 20u;
    ysf_interleave_dibits(bits, code, nbytes == 20 ? 9 : 5, 20, 1);
    int e = dv_viterbi_decode(&YSF_CONV, code, n, 0, dec);
    if (errs)
        *errs = e;
    dv_pack_msb(dec, nb + 16u, w);
    uint16_t crc = dv_crc16_ysf(w, nbytes);
    bool ok = w[nbytes] == (uint8_t)(crc >> 8) && w[nbytes + 1] == (uint8_t)crc;
    ysf_whiten(dec, nb);
    dv_pack_msb(dec, nb, data);
    return ok;
}

/* ---- V/D mode 2 voice -------------------------------------------------------- */

void ysf_vd2_voice_encode(const uint8_t voice[7], uint8_t bits[104])
{
    uint8_t b[49], a[104];
    dv_unpack_msb(voice, 49, b);
    for (int i = 0; i < 27; i++)
        a[3 * i] = a[3 * i + 1] = a[3 * i + 2] = b[i];
    memcpy(a + 81, b + 27, 22);
    a[103] = 0;
    ysf_whiten(a, 104);
    for (unsigned k = 0; k < 104; k++)
        bits[k] = a[(k % 4u) * 26u + k / 4u];
}

int ysf_vd2_voice_decode(const uint8_t bits[104], uint8_t voice[7])
{
    uint8_t a[104], b[49];
    int weak = 0;
    for (unsigned k = 0; k < 104; k++)
        a[(k % 4u) * 26u + k / 4u] = bits[k];
    ysf_whiten(a, 104);
    for (int i = 0; i < 27; i++) {
        int s = a[3 * i] + a[3 * i + 1] + a[3 * i + 2];
        b[i] = (uint8_t)(s >= 2);
        if (s == 1 || s == 2)
            weak++;
    }
    memcpy(b + 27, a + 81, 22);
    dv_pack_msb(b, 49, voice);
    return weak;
}
