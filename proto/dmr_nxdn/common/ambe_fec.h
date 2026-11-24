/* Channel FEC of the AMBE+2 3600x2450 vocoder frame (DMR vocoder socket frames, NXDN EHR VCH):
 * 49 voice bits <-> 72 bits; and of the 7200 bps full rate frame (NXDN EFR VCH): 88 <-> 144 bits. Only the error-correction wrapper; there is no speech codec here.
 *
 *   C0: 12 bits + Golay(23,12) + overall parity = 24     (u0, most important bits)
 *   C1: 12 bits + Golay(23,12) = 23, xored with a PN sequence seeded from u0
 *   C2: 11 bits, C3: 14 bits, unprotected
 *   72-bit order: dibit i carries (C[w][x], C[y][z]) from a fixed 36-entry interleave table.
 * Layout checked against NXDN TS 1-A 7.2.1.1 (EHR tone pattern, bit exact) and the 48 encrypted
 * VCH test vectors of NXDN TS 1-D 7.2.1 (scramble / DES / AES).
 */
#ifndef PROTO_AMBE_FEC_H
#define PROTO_AMBE_FEC_H
#include <stdint.h>

/* d49: 49 bits MSB first in 7 bytes (last byte: top bit only); out72: 9 bytes */
void ambe72_encode(const uint8_t d49[7], uint8_t out72[9]);
/* returns corrected bit errors (0..7), or -1 if C0 or C1 is not correctable (data still filled) */
int  ambe72_decode(const uint8_t in72[9], uint8_t d49[7]);

/* AMBE+2 full rate (NXDN EFR, 7200 bps): 88 voice bits <-> 144-bit VCH. Same FEC layout as the
 * IMBE full rate frame (TIA-102.BABA):
 *   u0..u3: 12 bits each -> Golay(23,12) c0..c3; u4..u6: 11 bits -> Hamming(15,11) c4..c6; u7: 7 bits
 *   c1..c6 (114 bits) xored with the LCG sequence pr = 16 u0, pr = 173 pr + 13849 mod 65536 (bit = pr >= 32768)
 *   144-bit order: c0..c7 concatenated (MSB first) = vector bit i, row r = i / 24, k = i % 24 goes to
 *   air bit 12 (k / 2) + (k even ? r : 6 + (r ^ 1)).
 * Checked bit exact against NXDN TS 1-A 7.2.2 (EFR tone / silence) and the 40 encrypted EFR VCH test
 * vectors of NXDN TS 1-D 7.2.1. d88: 11 bytes MSB first; out144: 18 bytes. */
void ambe144_encode(const uint8_t d88[11], uint8_t out144[18]);
/* returns corrected bit errors, or -1 if a Golay word is not correctable (data still filled) */
int  ambe144_decode(const uint8_t in144[18], uint8_t d88[11]);

/* Hamming(15,11) of the full rate frame: cw = data << 4 | parity */
uint16_t hamming1511_encode(uint16_t d11);
int      hamming1511_decode(uint16_t cw, uint16_t *d11);     /* 0 or 1 errors corrected */

/* Golay(23,12), g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1; cw = data << 11 | parity */
uint32_t golay23_encode(uint16_t d12);
int      golay23_decode(uint32_t cw, uint16_t *d12);   /* errors corrected (0..3) */

#endif
