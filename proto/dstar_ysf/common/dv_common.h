/*
 * dv_common.h - small helpers shared by the D-STAR and YSF protocol layers.
 *
 * Portable C11, no hardware access, no malloc. Bits are kept "unpacked"
 * (one bit per uint8_t, value 0 or 1) inside the protocol layers because the
 * frames are short and it keeps the FEC code simple.
 *
 * Written from the public specifications (JARL D-STAR, Yaesu "Amateur Radio
 * Digital Standards" rev 1.02). No code copied from GPL projects.
 */
#ifndef DV_COMMON_H
#define DV_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- bit packing ---------------------------------------------------------- */

/* LSB of each byte first (D-STAR air order). */
void dv_unpack_lsb(const uint8_t *bytes, size_t nbits, uint8_t *bits);
void dv_pack_lsb(const uint8_t *bits, size_t nbits, uint8_t *bytes);
/* MSB of each byte first (YSF air order). */
void dv_unpack_msb(const uint8_t *bytes, size_t nbits, uint8_t *bits);
void dv_pack_msb(const uint8_t *bits, size_t nbits, uint8_t *bytes);

unsigned dv_popcount32(uint32_t v);
unsigned dv_popcount64(uint64_t v);

/* ---- CRC ------------------------------------------------------------------ */

/* CRC-16/X.25 (D-STAR P_FCS): poly x^16+x^12+x^5+1 reflected (0x8408), init
 * 0xFFFF, final XOR 0xFFFF. check("123456789") = 0x906E. */
uint16_t dv_crc16_x25(const uint8_t *data, size_t len);

/* YSF CRC: poly 0x1021 MSB first, init 0x0000, all bits inverted at the end.
 * check("123456789") = 0xCE3C. */
uint16_t dv_crc16_ysf(const uint8_t *data, size_t len);

/* ---- rate 1/2 convolutional code ----------------------------------------- */

/* Generator bit i = coefficient of D^i (bit 0 = current input).
 * D-STAR: K=3, g1=0x7 (1+D+D^2), g2=0x5 (1+D^2).
 * YSF:    K=5, g1=0x19 (1+D^3+D^4), g2=0x17 (1+D+D^2+D^4). */
typedef struct {
    uint8_t k;   /* constraint length, 2..5 */
    uint8_t g1;
    uint8_t g2;
} dv_conv_t;

#define DV_VITERBI_MAX_STEPS 336u   /* enough for the D-STAR header (330) */

/* Encode n input bits (caller appends the K-1 zero tail bits) -> 2n bits,
 * output order g1, g2, g1, g2, ... */
void dv_conv_encode(const dv_conv_t *c, const uint8_t *in, size_t n, uint8_t *out);

/* Viterbi decode of 2n code symbols into n bits, assumes start and end in
 * state 0 (zero tail included in n). `in` holds hard bits (0/1) when soft==0,
 * or soft values 0..255 (0 = sure 0, 255 = sure 1) when soft!=0.
 * Returns the final path metric (hard: number of corrected bit errors), or -1
 * when n is too large. Uses about 2*DV_VITERBI_MAX_STEPS bytes of stack. */
int dv_viterbi_decode(const dv_conv_t *c, const uint8_t *in, size_t n, int soft, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
