/*
 * p25_fec.h - forward error correction and CRC building blocks used by
 * P25 Phase 1 (TIA-102.BAAA-B, TIA-102.BABA) and Phase 2 (TIA-102.BBAC).
 *
 * All functions are pure, re-entrant, malloc-free and table-light (biggest
 * table is the GF(64) log/exp pair, 128 bytes). Bit conventions:
 *   - integers hold code words MSB first: the most significant used bit is the
 *     first bit on air.
 *   - "dibit arrays" hold one symbol (0..3) per byte, first bit in bit 1.
 *   - "byte arrays" are packed MSB first.
 */
#ifndef P25_FEC_H
#define P25_FEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- bit helpers ---------- */
static inline unsigned p25_bit_get(const uint8_t *bytes, unsigned i)
{
    return (bytes[i >> 3] >> (7 - (i & 7))) & 1u;
}
static inline void p25_bit_put(uint8_t *bytes, unsigned i, unsigned v)
{
    uint8_t m = (uint8_t)(0x80u >> (i & 7));
    if (v & 1u) bytes[i >> 3] |= m; else bytes[i >> 3] &= (uint8_t)~m;
}
/* read/write n<=32 bits starting at bit i (MSB first) */
uint32_t p25_bits_get(const uint8_t *bytes, unsigned i, unsigned n);
void     p25_bits_put(uint8_t *bytes, unsigned i, unsigned n, uint32_t v);
/* the same on dibit arrays (bit k lives in dibit k/2) */
static inline unsigned p25_dbit_get(const uint8_t *dib, unsigned k)
{
    return (dib[k >> 1] >> (1 - (k & 1))) & 1u;
}
static inline void p25_dbit_put(uint8_t *dib, unsigned k, unsigned v)
{
    uint8_t m = (uint8_t)(2u >> (k & 1));
    if (v & 1u) dib[k >> 1] |= m; else dib[k >> 1] &= (uint8_t)~m;
}
uint32_t p25_dbits_get(const uint8_t *dib, unsigned k, unsigned n);
void     p25_dbits_put(uint8_t *dib, unsigned k, unsigned n, uint32_t v);
static inline unsigned p25_popcount32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
static inline unsigned p25_popcount64(uint64_t x)
{
    return p25_popcount32((uint32_t)x) + p25_popcount32((uint32_t)(x >> 32));
}

/* ---------- Golay ---------- */
/* Golay(23,12,7), generator 0xC75: returns info<<11 | parity(11). */
uint32_t p25_golay23_encode(uint32_t info12);
/* Corrects up to 3 errors. Returns 12-bit info, *errs = bits corrected
 * (always succeeds: the code is perfect). */
uint32_t p25_golay23_decode(uint32_t cw23, int *errs);
/* Extended Golay(24,12,8) = golay23 << 1 | even parity. */
uint32_t p25_golay24_encode(uint32_t info12);
/* Returns info or -1 if uncorrectable (4 errors detected). */
int      p25_golay24_decode(uint32_t cw24, int *errs);
/* Shortened Golay(18,6,8) (HDU hex words): low 18 bits of golay24(info6). */
static inline uint32_t p25_golay18_encode(uint32_t info6) { return p25_golay24_encode(info6 & 0x3F) & 0x3FFFF; }
int      p25_golay18_decode(uint32_t cw18, int *errs);

/* ---------- Hamming / cyclic ---------- */
uint32_t p25_hamming15_encode(uint32_t info11);      /* Hamming(15,11,3) (IMBE c4..c6) */
uint32_t p25_hamming15_decode(uint32_t cw15, int *errs);
uint32_t p25_hamming10_encode(uint32_t info6);       /* Hamming(10,6,3) (LDU LC/ES hex words) */
uint32_t p25_hamming10_decode(uint32_t cw10, int *errs);
uint32_t p25_cyclic16_encode(uint32_t info8);        /* cyclic(16,8,5) (LSD) */
int      p25_cyclic16_decode(uint32_t cw16, int *errs); /* -1 if > 2 errors */

/* ---------- BCH(63,16,23) + parity: P25 NID ---------- */
/* Returns the 64-bit NID: 16 info bits, 47 BCH parity bits, 1 parity bit. */
uint64_t p25_nid_encode(uint16_t nac, uint8_t duid);
/* Decodes a received 64-bit NID; up to 11 bit errors are corrected in the BCH
 * part. Returns 0 and fills nac/duid, or -1. *errs = corrected bits. The 64th
 * bit is not used for decoding (it only depends on the DUID). */
int      p25_nid_decode(uint64_t nid, uint16_t *nac, uint8_t *duid, int *errs);

/* ---------- GF(2^6) Reed-Solomon (x^6+x+1, fcr 1, prim 1) ---------- */
/* Code words are arrays of 6-bit symbols, message first, parity last; a
 * shortened code (n<63) is the full code with leading zero symbols removed.
 * Used: Phase 1 (24,12,13) (24,16,9) (36,20,17); Phase 2 (63,35) shortened +
 * punctured: FACCH (45,26,20), SACCH (52,30,23), ESS (44,16,29). */
void p25_rs_encode(const uint8_t *msg, unsigned k, unsigned nroots, uint8_t *parity);
/* cw: n = k + nroots symbols (modified in place). erasures: positions inside
 * cw (0 = first symbol), may be NULL. Returns number of corrected symbols or
 * -1 if uncorrectable. */
int  p25_rs_decode(uint8_t *cw, unsigned n, unsigned nroots, const uint8_t *erasures, unsigned n_eras);
/* Punctured decoding helper: code word of length n_full (incl. the punctured
 * tail of `punct` parity symbols, which need not be present in cw). cw must
 * have room for n_full symbols. */
int  p25_rs_decode_punct(uint8_t *cw, unsigned n_full, unsigned nroots, unsigned punct);

/* GF(64) arithmetic (exposed for tests / BCH) */
uint8_t p25_gf64_mul(uint8_t a, uint8_t b);
uint8_t p25_gf64_exp(unsigned i);       /* alpha^i */
int     p25_gf64_log(uint8_t a);        /* -1 for 0 */

/* ---------- trellis codes (TSBK, PDU) ---------- */
/* 1/2 rate: 12 bytes in -> 98 dibits out (interleaved, ready for air). */
void p25_trellis12_encode(const uint8_t in[12], uint8_t out_dibits[98]);
/* Viterbi decode; returns path metric (bit errors on the best path). */
int  p25_trellis12_decode(const uint8_t in_dibits[98], uint8_t out[12]);
/* 3/4 rate (confirmed PDU data blocks): 18 bytes <-> 98 dibits. */
void p25_trellis34_encode(const uint8_t in[18], uint8_t out_dibits[98]);
int  p25_trellis34_decode(const uint8_t in_dibits[98], uint8_t out[18]);

/* ---------- CRCs (bit-serial, over byte arrays, MSB first) ---------- */
/* CRC-CCITT x^16+x^12+x^5+1, init 0, inverted (TSBK, PDU header, LCCH). */
uint16_t p25_crc16(const uint8_t *bytes, unsigned nbits);
/* CRC-9 x^9+x^6+x^4+x^3+1, inverted (confirmed data block serial+data). */
uint16_t p25_crc9(const uint8_t *bytes, unsigned nbits);
/* CRC-32 0x04C11DB7, init 0, inverted (PDU packet CRC). */
uint32_t p25_crc32(const uint8_t *bytes, unsigned nbits);
/* CRC-12 x^12+x^11+x^7+x^4+x^2+x+1, init 0, inverted (Phase 2 MAC). */
uint16_t p25_crc12(const uint8_t *bytes, unsigned nbits);

/* ---------- IMBE / AMBE pseudo-random masking ---------- */
/* LCG of TIA-102.BABA: p(0)=16*u0, p(n)=(173 p(n-1)+13849) mod 65536,
 * mask bit = p(n) >> 15. Fills nbits mask bits MSB first into *state. */
uint32_t p25_pn_bits(uint32_t *state, unsigned nbits);

#ifdef __cplusplus
}
#endif
#endif
