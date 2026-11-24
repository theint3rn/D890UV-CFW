/* dv_common.c - see dv_common.h */
#include "dv_common.h"

#include <string.h>

void dv_unpack_lsb(const uint8_t *bytes, size_t nbits, uint8_t *bits)
{
    for (size_t i = 0; i < nbits; i++)
        bits[i] = (uint8_t)((bytes[i >> 3] >> (i & 7)) & 1u);
}

void dv_pack_lsb(const uint8_t *bits, size_t nbits, uint8_t *bytes)
{
    memset(bytes, 0, (nbits + 7) / 8);
    for (size_t i = 0; i < nbits; i++)
        if (bits[i] & 1u)
            bytes[i >> 3] |= (uint8_t)(1u << (i & 7));
}

void dv_unpack_msb(const uint8_t *bytes, size_t nbits, uint8_t *bits)
{
    for (size_t i = 0; i < nbits; i++)
        bits[i] = (uint8_t)((bytes[i >> 3] >> (7 - (i & 7))) & 1u);
}

void dv_pack_msb(const uint8_t *bits, size_t nbits, uint8_t *bytes)
{
    memset(bytes, 0, (nbits + 7) / 8);
    for (size_t i = 0; i < nbits; i++)
        if (bits[i] & 1u)
            bytes[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
}

unsigned dv_popcount32(uint32_t v)
{
    unsigned n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

unsigned dv_popcount64(uint64_t v)
{
    return dv_popcount32((uint32_t)v) + dv_popcount32((uint32_t)(v >> 32));
}

uint16_t dv_crc16_x25(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

uint16_t dv_crc16_ysf(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000u;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

static unsigned parity8(unsigned v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

void dv_conv_encode(const dv_conv_t *c, const uint8_t *in, size_t n, uint8_t *out)
{
    unsigned reg = 0; /* bit i = input i steps ago */
    unsigned mask = (1u << c->k) - 1u;
    for (size_t i = 0; i < n; i++) {
        reg = ((reg << 1) | (in[i] & 1u)) & mask;
        out[2 * i] = (uint8_t)parity8(reg & c->g1);
        out[2 * i + 1] = (uint8_t)parity8(reg & c->g2);
    }
}

int dv_viterbi_decode(const dv_conv_t *c, const uint8_t *in, size_t n, int soft, uint8_t *out)
{
    /* K <= 5 -> at most 16 states, one uint16 of decisions per step. */
    if (n > DV_VITERBI_MAX_STEPS || c->k < 2 || c->k > 5)
        return -1;
    const unsigned nstates = 1u << (c->k - 1);
    const unsigned smask = nstates - 1u;
    uint16_t dec[DV_VITERBI_MAX_STEPS];
    uint32_t metric[16], next[16];
    const uint32_t INF = 0x3FFFFFFFu;

    for (unsigned s = 0; s < nstates; s++)
        metric[s] = INF;
    metric[0] = 0;

    for (size_t t = 0; t < n; t++) {
        unsigned r0 = in[2 * t], r1 = in[2 * t + 1];
        if (!soft) {
            r0 = r0 ? 255u : 0u;
            r1 = r1 ? 255u : 0u;
        }
        uint16_t d = 0;
        for (unsigned ns = 0; ns < nstates; ns++) {
            unsigned b = ns & 1u;
            uint32_t best = INF;
            unsigned bestx = 0;
            for (unsigned x = 0; x < 2; x++) {
                unsigned ps = (ns >> 1) | (x << (c->k - 2));
                if (metric[ps] >= INF)
                    continue;
                unsigned reg = ((ps << 1) | b) & ((1u << c->k) - 1u);
                unsigned e0 = parity8(reg & c->g1), e1 = parity8(reg & c->g2);
                uint32_t bm = (e0 ? 255u - r0 : r0) + (e1 ? 255u - r1 : r1);
                uint32_t m = metric[ps] + bm;
                if (m < best) {
                    best = m;
                    bestx = x;
                }
            }
            next[ns] = best;
            if (bestx)
                d |= (uint16_t)(1u << ns);
        }
        dec[t] = d;
        memcpy(metric, next, nstates * sizeof(uint32_t));
    }

    unsigned s = 0; /* tail-terminated: end in state 0 */
    for (size_t t = n; t-- > 0;) {
        out[t] = (uint8_t)(s & 1u);
        unsigned x = (dec[t] >> s) & 1u;
        s = ((s >> 1) | (x << (c->k - 2))) & smask;
    }
    uint32_t m = metric[0];
    return (int)(soft ? m : (m + 127u) / 255u);
}
