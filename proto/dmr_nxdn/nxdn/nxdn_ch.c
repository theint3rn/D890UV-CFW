/* NXDN layer 1: LICH, scrambler, functional channel coding (TS 1-A 4.5, 4.6; TS 1-E 4.5.1.1). */
#include "nxdn.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

/* ---------------- LICH ---------------- */
uint8_t nxdn_lich_pack(const nxdn_lich_t *l)
{
    return (uint8_t)(((l->rf & 3u) << 5) | ((l->fct & 3u) << 3) | ((l->option & 3u) << 1) | (l->dir & 1u));
}

void nxdn_lich_unpack(uint8_t v, nxdn_lich_t *l)
{
    l->rf = (v >> 5) & 3u;
    l->fct = (v >> 3) & 3u;
    l->option = (v >> 1) & 3u;
    l->dir = v & 1u;
}

static uint8_t lich_parity(uint8_t v7) { return (uint8_t)(((v7 >> 6) ^ (v7 >> 5) ^ (v7 >> 4) ^ (v7 >> 3)) & 1u); }

void nxdn_lich_encode(uint8_t v7, uint8_t dibits[8])
{
    uint8_t v = (uint8_t)((v7 << 1) | lich_parity(v7));
    for (int i = 0; i < 8; i++) dibits[i] = (uint8_t)((((v >> (7 - i)) & 1u) << 1) | 1u);   /* 0 -> +3, 1 -> -3 */
}

int nxdn_lich_decode(const uint8_t dibits[8], uint8_t *v7)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v = (uint8_t)((v << 1) | ((dibits[i] >> 1) & 1u));          /* sign of the symbol */
    *v7 = v >> 1;
    return (v & 1u) == lich_parity(*v7) ? 0 : -1;
}

/* ---------------- scrambler: X^9 + X^4 + 1, S8..S0 = 0 1110 0100, output S0 ---------------- */
void nxdn_scramble(uint8_t frame[NXDN_FRAME_DIBITS], unsigned first, unsigned last)
{
    unsigned s = 0x0E4u;
    for (unsigned i = 10; i < NXDN_FRAME_DIBITS && i < last; i++) {
        unsigned out = s & 1u;
        if (out && i >= first) frame[i] ^= 2u;       /* sign inversion = flip the first bit of the dibit */
        unsigned fb = (s ^ (s >> 4)) & 1u;
        s = (s >> 1) | (fb << 8);
    }
}

/* ---------------- functional channels ---------------- */
static const uint8_t P_SACCH[12] = { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0 };            /* 111111 / 110110 */
static const uint8_t P_FACCH1[4] = { 1, 0, 1, 1 };                                     /* 11 / 01 */
static const uint8_t P_CAC[14] = { 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1 };         /* 1111111 / 1011101 */
static const uint8_t P_LCAC[26] = { 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1,
                                    1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };              /* 13 x 1 / 0110001110111 */

typedef struct {
    uint16_t info, nulls, crcw;
    uint32_t poly;
    const uint8_t *punct;
    uint8_t period;
    uint8_t rows, cols;
    uint16_t coded;
} chdef_t;

static const chdef_t CH[NXDN_CH_COUNT] = {
    [NXDN_CH_SACCH]  = { 26, 0, 6, 0x27u, P_SACCH, 12, 5, 12, 60 },        /* x^6+x^5+x^2+x+1 */
    [NXDN_CH_FACCH1] = { 80, 0, 12, 0x80Fu, P_FACCH1, 4, 9, 16, 144 },     /* x^12+x^11+x^3+x^2+x+1 */
    [NXDN_CH_UDCH]   = { 184, 0, 15, 0x4CC5u, P_CAC, 14, 29, 12, 348 },    /* x^15+x^14+x^11+x^10+x^7+x^6+x^2+1 */
    [NXDN_CH_CAC]    = { 152, 3, 16, 0x1021u, P_CAC, 14, 25, 12, 300 },
    [NXDN_CH_LCAC]   = { 136, 0, 16, 0x1021u, P_LCAC, 26, 21, 12, 252 },
    [NXDN_CH_SCAC]   = { 104, 2, 16, 0x1021u, NULL, 0, 21, 12, 252 },
    [NXDN_CH_SCCH]   = { 25, 0, 7, 0x09u, P_SACCH, 12, 5, 12, 60 },        /* x^7+x^3+1 (TS 1-E) */
};

unsigned nxdn_ch_info_bits(nxdn_ch_t ch) { return CH[ch].info; }
unsigned nxdn_ch_coded_bits(nxdn_ch_t ch) { return CH[ch].coded; }

#define CH_MAX_IN 210

void nxdn_ch_encode(nxdn_ch_t ch, const uint8_t *info, uint8_t *bits)
{
    const chdef_t *d = &CH[ch];
    uint8_t in[CH_MAX_IN], enc[2 * CH_MAX_IN];
    unsigned n = 0;
    for (unsigned i = 0; i < d->info; i++) in[n++] = (uint8_t)bits_get(info, i);
    for (unsigned i = 0; i < d->nulls; i++) in[n++] = 0;
    uint32_t crc = crc_bits(in, n, d->poly, d->crcw, 0xFFFFFFFFu);
    for (unsigned i = 0; i < d->crcw; i++) in[n++] = (uint8_t)((crc >> (d->crcw - 1 - i)) & 1u);
    for (unsigned i = 0; i < 4; i++) in[n++] = 0;
    conv_punct_t p = { d->punct, d->period };
    unsigned m = conv_k5_encode(in, n, enc, d->punct ? &p : NULL);
    (void)m;
    /* interleave: write rows of cols bits, read columns */
    for (unsigned r = 0; r < d->rows; r++)
        for (unsigned c = 0; c < d->cols; c++) bits[c * d->rows + r] = enc[r * d->cols + c];
}

int nxdn_ch_decode(nxdn_ch_t ch, const uint8_t *bits, uint8_t *info, int *metric)
{
    const chdef_t *d = &CH[ch];
    uint8_t enc[2 * CH_MAX_IN], dec[CH_MAX_IN];
    for (unsigned r = 0; r < d->rows; r++)
        for (unsigned c = 0; c < d->cols; c++) enc[r * d->cols + c] = bits[c * d->rows + r] & 1u;
    unsigned n = d->info + d->nulls + d->crcw + 4u;
    conv_punct_t p = { d->punct, d->period };
    int m = conv_k5_decode(enc, d->coded, dec, n, d->punct ? &p : NULL);
    if (metric) *metric = m;
    unsigned k = d->info + d->nulls;
    uint32_t crc = crc_bits(dec, k, d->poly, d->crcw, 0xFFFFFFFFu);
    memset(info, 0, (d->info + 7u) / 8u);
    for (unsigned i = 0; i < d->info; i++) bits_put(info, i, dec[i]);
    return crc == bits_arr_val(dec + k, d->crcw) ? 0 : -1;
}
