/* Bit helpers shared by the DMR and NXDN layers.
 *
 * Conventions used everywhere in this library:
 *  - "bytes" buffers are MSB-first: bit 0 of a field is the MSB of byte 0.
 *  - "bit arrays" are uint8_t per bit (0/1), index 0 = first transmitted.
 *  - a dibit is 0..3, (first bit << 1) | second bit. 4FSK mapping for both
 *    DMR (TS 102 361-1 10.2) and NXDN (TS 1-A 3.3): 01=+3 00=+1 10=-1 11=-3.
 */
#ifndef PROTO_BITS_H
#define PROTO_BITS_H
#include <stdint.h>
#include <stddef.h>

static inline int bits_get(const uint8_t *buf, unsigned pos)
{
    return (buf[pos >> 3] >> (7u - (pos & 7u))) & 1;
}

static inline void bits_put(uint8_t *buf, unsigned pos, int v)
{
    uint8_t m = (uint8_t)(0x80u >> (pos & 7u));
    if (v) buf[pos >> 3] |= m; else buf[pos >> 3] &= (uint8_t)~m;
}

/* read an n-bit (n <= 32) MSB-first field starting at bit pos */
static inline uint32_t bits_field(const uint8_t *buf, unsigned pos, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) v = (v << 1) | (uint32_t)bits_get(buf, pos + i);
    return v;
}

static inline void bits_set_field(uint8_t *buf, unsigned pos, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++) bits_put(buf, pos + i, (int)((v >> (n - 1 - i)) & 1u));
}

/* bytes -> bit array (nbits bits) and back */
static inline void bits_unpack(uint8_t *bits, const uint8_t *bytes, unsigned nbits)
{
    for (unsigned i = 0; i < nbits; i++) bits[i] = (uint8_t)bits_get(bytes, i);
}

static inline void bits_pack(uint8_t *bytes, const uint8_t *bits, unsigned nbits)
{
    for (unsigned i = 0; i < (nbits + 7) / 8; i++) bytes[i] = 0;
    for (unsigned i = 0; i < nbits; i++) if (bits[i]) bytes[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
}

/* value of n bits from a bit array, MSB first */
static inline uint32_t bits_arr_val(const uint8_t *bits, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) v = (v << 1) | (bits[i] & 1u);
    return v;
}

static inline void bits_arr_set(uint8_t *bits, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++) bits[i] = (uint8_t)((v >> (n - 1 - i)) & 1u);
}

static inline int popcount32(uint32_t v)
{
    int c = 0;
    while (v) { v &= v - 1; c++; }
    return c;
}

/* dibit stream <-> bit array */
static inline void dibits_to_bits(uint8_t *bits, const uint8_t *dibits, unsigned ndibits)
{
    for (unsigned i = 0; i < ndibits; i++) {
        bits[2 * i] = (uint8_t)((dibits[i] >> 1) & 1u);
        bits[2 * i + 1] = (uint8_t)(dibits[i] & 1u);
    }
}

static inline void bits_to_dibits(uint8_t *dibits, const uint8_t *bits, unsigned ndibits)
{
    for (unsigned i = 0; i < ndibits; i++) dibits[i] = (uint8_t)(((bits[2 * i] & 1u) << 1) | (bits[2 * i + 1] & 1u));
}

#endif
