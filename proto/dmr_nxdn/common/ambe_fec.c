/* AMBE+2 3600x2450 frame FEC (see ambe_fec.h). */
#include "ambe_fec.h"
#include "bits.h"
#include <string.h>
#include <stdbool.h>

#define GOLAY_G 0xC75u

uint32_t golay23_encode(uint16_t d12)
{
    uint32_t r = (uint32_t)(d12 & 0xFFFu) << 11;
    for (int i = 22; i >= 11; i--) if (r & (1u << i)) r ^= GOLAY_G << (i - 11);
    return ((uint32_t)(d12 & 0xFFFu) << 11) | r;
}

static uint32_t syndrome(uint32_t cw) { return (golay23_encode((uint16_t)(cw >> 11)) ^ cw) & 0x7FFu; }

int golay23_decode(uint32_t cw, uint16_t *d12)
{
    /* perfect code, t = 3: try error patterns on the 12 data bits (0..3 of them); the rest of the
     * syndrome must then be a parity error pattern of weight <= 3 - data errors */
    cw &= 0x7FFFFFu;
    uint32_t s = syndrome(cw);
    uint32_t sd[12];
    for (int i = 0; i < 12; i++) sd[i] = golay23_encode((uint16_t)(1u << i)) & 0x7FFu;
    if (popcount32(s) <= 3) { *d12 = (uint16_t)(cw >> 11); return popcount32(s); }
    for (int a = 0; a < 12; a++) {
        uint32_t s1 = s ^ sd[a];
        if (popcount32(s1) <= 2) { *d12 = (uint16_t)((cw >> 11) ^ (1u << a)); return 1 + popcount32(s1); }
    }
    for (int a = 0; a < 12; a++)
        for (int b = a + 1; b < 12; b++) {
            uint32_t s2 = s ^ sd[a] ^ sd[b];
            if (popcount32(s2) <= 1) { *d12 = (uint16_t)((cw >> 11) ^ (1u << a) ^ (1u << b)); return 2 + popcount32(s2); }
        }
    for (int a = 0; a < 12; a++)
        for (int b = a + 1; b < 12; b++)
            for (int c = b + 1; c < 12; c++)
                if ((s ^ sd[a] ^ sd[b] ^ sd[c]) == 0) {
                    *d12 = (uint16_t)((cw >> 11) ^ (1u << a) ^ (1u << b) ^ (1u << c));
                    return 3;
                }
    *d12 = (uint16_t)(cw >> 11);
    return -1;
}

/* interleave: dibit i = (fr[W[i]][X[i]], fr[Y[i]][Z[i]]); fr[v][j] = bit j of vector v (j = 0 LSB) */
static const uint8_t W[36] = { 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,2,0,2,0,2,0,2,0,2,0,2,0,2 };
static const uint8_t X[36] = { 23,10,22,9,21,8,20,7,19,6,18,5,17,4,16,3,15,2,14,1,13,0,12,10,11,9,10,8,9,7,8,6,7,5,6,4 };
static const uint8_t Y[36] = { 0,2,0,2,0,2,0,2,0,3,0,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3 };
static const uint8_t Z[36] = { 5,3,4,2,3,1,2,0,1,13,0,12,22,11,21,10,20,9,19,8,18,7,17,6,16,5,15,4,14,3,13,2,12,1,11,0 };

/* C1 whitening: 23 bits from the LCG pr[i] = 173 pr[i-1] + 13849 mod 65536, pr[0] = 16 u0, bit = pr >= 32768 */
static uint32_t pn23(uint16_t u0)
{
    uint32_t pr = 16u * u0, m = 0;
    for (int k = 22; k >= 0; k--) {          /* pr[1] applies to C1 bit 22 ... pr[23] to bit 0 */
        pr = (173u * pr + 13849u) & 0xFFFFu;
        if (pr >= 32768u) m |= 1u << k;
    }
    return m;
}

static void put_vec(uint32_t fr[4], uint8_t out72[9])
{
    memset(out72, 0, 9);
    for (unsigned i = 0; i < 36; i++) {
        if ((fr[W[i]] >> X[i]) & 1u) out72[(2 * i) >> 3] |= (uint8_t)(0x80u >> ((2 * i) & 7u));
        if ((fr[Y[i]] >> Z[i]) & 1u) out72[(2 * i + 1) >> 3] |= (uint8_t)(0x80u >> ((2 * i + 1) & 7u));
    }
}

void ambe72_encode(const uint8_t d49[7], uint8_t out72[9])
{
    uint16_t u0 = (uint16_t)bits_field(d49, 0, 12), u1 = (uint16_t)bits_field(d49, 12, 12);
    uint32_t u2 = bits_field(d49, 24, 11), u3 = bits_field(d49, 35, 14);
    uint32_t fr[4];
    uint32_t c0 = golay23_encode(u0);
    fr[0] = (c0 << 1) | (uint32_t)(popcount32(c0) & 1);       /* even overall parity in bit 0 */
    fr[1] = golay23_encode(u1) ^ pn23(u0);
    fr[2] = u2;
    fr[3] = u3;
    put_vec(fr, out72);
}

int ambe72_decode(const uint8_t in72[9], uint8_t d49[7])
{
    uint32_t fr[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < 36; i++) {
        if (bits_get(in72, 2 * i)) fr[W[i]] |= 1u << X[i];
        if (bits_get(in72, 2 * i + 1)) fr[Y[i]] |= 1u << Z[i];
    }
    uint16_t u0, u1;
    int e0 = golay23_decode(fr[0] >> 1, &u0);
    int e1 = golay23_decode(fr[1] ^ pn23(u0), &u1);
    memset(d49, 0, 7);
    bits_set_field(d49, 0, 12, u0);
    bits_set_field(d49, 12, 12, u1);
    bits_set_field(d49, 24, 11, fr[2] & 0x7FFu);
    bits_set_field(d49, 35, 14, fr[3] & 0x3FFFu);
    if (e0 < 0 || e1 < 0) return -1;
    return e0 + e1;
}

/* ---------------- full rate (EFR) 88 <-> 144 ---------------- */
/* parity check rows over (11 data bits << 4 | 4 parity bits); data columns are the 4-bit values of
 * weight >= 2 in descending order (TIA-102.BABA Hamming(15,11)) */
static const uint16_t HROW[4] = { 0x7F08, 0x78E4, 0x66D2, 0x55B1 };

uint16_t hamming1511_encode(uint16_t d11)
{
    uint16_t p = 0;
    d11 &= 0x7FFu;
    for (int r = 0; r < 4; r++) p = (uint16_t)((p << 1) | (popcount32((uint32_t)((HROW[r] >> 4) & d11)) & 1u));
    return (uint16_t)((d11 << 4) | p);
}

int hamming1511_decode(uint16_t cw, uint16_t *d11)
{
    cw &= 0x7FFFu;
    uint16_t s = (uint16_t)((hamming1511_encode((uint16_t)(cw >> 4)) ^ cw) & 0xFu);
    if (s) {
        for (int b = 0; b < 15; b++) {              /* column of bit b = its syndrome */
            uint16_t col = 0;
            for (int r = 0; r < 4; r++) col = (uint16_t)((col << 1) | ((HROW[r] >> b) & 1u));
            if (col == s) { cw ^= (uint16_t)(1u << b); break; }
        }
    }
    *d11 = (uint16_t)(cw >> 4);
    return s ? 1 : 0;
}

static unsigned efr_air(unsigned i)
{
    unsigned r = i / 24u, k = i % 24u;
    return 12u * (k / 2u) + ((k & 1u) ? 6u + (r ^ 1u) : r);
}

static const uint8_t EFR_LEN[8] = { 23, 23, 23, 23, 15, 15, 15, 7 };

/* 114 whitening bits for c1..c6, MSB first in m[0..14] */
static void efr_pn(uint16_t u0, uint8_t m[15])
{
    uint32_t pr = 16u * u0;
    memset(m, 0, 15);
    for (unsigned i = 0; i < 114; i++) {
        pr = (173u * pr + 13849u) & 0xFFFFu;
        if (pr >= 32768u) m[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
}

void ambe144_encode(const uint8_t d88[11], uint8_t out144[18])
{
    uint32_t c[8];
    unsigned o = 0;
    for (int v = 0; v < 8; v++) {
        unsigned n = v < 4 ? 12u : v < 7 ? 11u : 7u;
        uint32_t u = bits_field(d88, o, n);
        o += n;
        c[v] = v < 4 ? golay23_encode((uint16_t)u) : v < 7 ? hamming1511_encode((uint16_t)u) : u;
    }
    uint8_t m[15];
    efr_pn((uint16_t)(c[0] >> 11), m);
    memset(out144, 0, 18);
    unsigned i = 0;
    for (int v = 0; v < 8; v++)
        for (int j = EFR_LEN[v] - 1; j >= 0; j--, i++) {
            unsigned b = (c[v] >> j) & 1u;
            if (i >= 23 && i < 137) b ^= (m[(i - 23) >> 3] >> (7u - ((i - 23) & 7u))) & 1u;
            if (b) { unsigned a = efr_air(i); out144[a >> 3] |= (uint8_t)(0x80u >> (a & 7u)); }
        }
}

int ambe144_decode(const uint8_t in144[18], uint8_t d88[11])
{
    uint32_t c[8] = { 0 };
    unsigned i = 0;
    for (int v = 0; v < 8; v++)
        for (int j = 0; j < EFR_LEN[v]; j++, i++) c[v] = (c[v] << 1) | bits_get(in144, efr_air(i));
    uint16_t u[8];
    int err = 0, e = golay23_decode(c[0], &u[0]);
    bool bad = e < 0;
    err += e > 0 ? e : 0;
    uint8_t m[15];
    efr_pn(u[0], m);
    i = 23;
    for (int v = 1; v < 7; v++) {
        uint32_t w = 0;
        for (int j = 0; j < EFR_LEN[v]; j++, i++) w = (w << 1) | ((m[(i - 23) >> 3] >> (7u - ((i - 23) & 7u))) & 1u);
        c[v] ^= w;
        if (v < 4) { e = golay23_decode(c[v], &u[v]); if (e < 0) bad = true; else err += e; }
        else err += hamming1511_decode((uint16_t)c[v], &u[v]);
    }
    u[7] = (uint16_t)c[7];
    memset(d88, 0, 11);
    unsigned o = 0;
    for (int v = 0; v < 8; v++) {
        unsigned n = v < 4 ? 12u : v < 7 ? 11u : 7u;
        bits_set_field(d88, o, n, u[v]);
        o += n;
    }
    return bad ? -1 : err;
}
