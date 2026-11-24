/* MDC1200 packet coding (the D890UV CPS calls it MDC1200, the D168UV CPS and both stock radios' menus QDC1200: the
 * same format, both stock firmwares run the same decoder, sync 0x07 / 0x092A446F in FUN_0813c16c (D890UV V1.05) and
 * FUN_08012xxx (D168UV V1.07, constant at 0x08013130), 112 coded bits, see cfw/docs/AUDIO_PATHS.md "Signalling").
 *
 * Packet = preamble 0x55 x7, sync 07 09 2A 44 6F, then 14 bytes: data op, arg, unit id high, unit id low, CRC-16
 * (MDC variant: CCITT 0x1021 fed LSB first, bit-reversed, inverted) low, high, status 0, + 7 parity bytes of a rate
 * 1/2 convolutional code (taps 0, 2, 5, 6 of the last 7 data bits), interleaved 16 x 7. Bits are sent MSB first,
 * differentially: a bit equal to the previous one = one 1200 Hz cycle, a changed bit = 1.5 cycles of 1800 Hz.
 * Same coding as emu/streams/gen_mdc1200.py (the stock V1.05 decoder accepts its packets, emu/streams/test_stock_mdc.py). */
#include <string.h>
#include "sig.h"

uint16_t mdc_crc(const uint8_t *d, int n)
{
    uint16_t crc = 0;
    for(int i = 0; i < n; i++) {
        for(uint8_t j = 1; j; j = (uint8_t)(j << 1)) {
            uint16_t bit = crc & 0x8000u;
            crc = (uint16_t)(crc << 1);
            if(d[i] & j) { bit ^= 0x8000u; }
            if(bit) { crc ^= 0x1021u; }
        }
    }
    uint16_t r = 0;
    for(int i = 0; i < 16; i++) { if(crc & (1u << i)) { r |= (uint16_t)(1u << (15 - i)); } }
    return (uint16_t)(r ^ 0xFFFFu);
}

/* 14 data+parity bytes -> 112 interleaved bits (lbits[k]) */
static void code14(const uint8_t d7[7], uint8_t lbits[112])
{
    uint8_t d[14];
    memcpy(d, d7, 7);
    memset(d + 7, 0, 7);
    uint8_t csr[7] = { 0 };
    for(int i = 0; i < 7; i++) {
        for(int j = 0; j < 8; j++) {
            memmove(csr + 1, csr, 6);
            csr[0] = (uint8_t)((d[i] >> j) & 1u);
            uint8_t b = (uint8_t)((csr[0] + csr[2] + csr[5] + csr[6]) & 1u);
            d[i + 7] |= (uint8_t)(b << j);
        }
    }
    int k = 0, m = 0;
    for(int i = 0; i < 14; i++) {
        for(int j = 0; j < 8; j++) {
            lbits[k] = (uint8_t)((d[i] >> j) & 1u);
            k += 16;
            if(k > 111) { m++; k = m; }
        }
    }
}

static void put_block(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t *out)   /* 14 coded bytes */
{
    uint8_t d[7] = { b0, b1, b2, b3, 0, 0, 0 };
    uint16_t c = mdc_crc(d, 4);
    d[4] = (uint8_t)c; d[5] = (uint8_t)(c >> 8);
    uint8_t lb[112];
    code14(d, lb);
    for(int i = 0; i < 14; i++) {
        uint8_t v = 0;
        for(int j = 0; j < 8; j++) { v = (uint8_t)(v << 1 | lb[i * 8 + j]); }
        out[i] = v;
    }
}

static const uint8_t k_lead[12] = { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x07, 0x09, 0x2A, 0x44, 0x6F };

int mdc_build(uint8_t op, uint8_t arg, uint16_t unit, uint8_t out[MDC_PACKET_BYTES])
{
    memcpy(out, k_lead, sizeof k_lead);
    put_block(op, arg, (uint8_t)(unit >> 8), (uint8_t)unit, out + 12);
    return MDC_PACKET_BYTES;
}

/* double packet (op 0x35 / 0x55): the second 112-bit block follows the first without a new sync */
int mdc_build2(uint8_t op, uint8_t arg, uint16_t unit, const uint8_t ext[4], uint8_t out[MDC_PACKET2_BYTES])
{
    mdc_build(op, arg, unit, out);
    put_block(ext[0], ext[1], ext[2], ext[3], out + MDC_PACKET_BYTES);
    return MDC_PACKET2_BYTES;
}

/* 112 received bits (after the sync, in air order) -> op/arg/unit if the CRC matches. Single bit errors in the data
 * bits are corrected with the convolutional parity (syndrome check of each data bit), as far as one error per
 * 7-bit window goes. */
bool mdc_decode112(const uint8_t bits[112], uint8_t *op, uint8_t *arg, uint16_t *unit)
{
    uint8_t d[14];
    memset(d, 0, sizeof d);
    int k = 0, m = 0;
    for(int i = 0; i < 14; i++) {                       /* de-interleave: inverse of code14 */
        for(int j = 0; j < 8; j++) {
            if(bits[k]) { d[i] |= (uint8_t)(1u << j); }
            k += 16;
            if(k > 111) { m++; k = m; }
        }
    }
    for(int pass = 0; pass < 2; pass++) {
        uint16_t c = mdc_crc(d, 4);
        if(d[4] == (uint8_t)c && d[5] == (uint8_t)(c >> 8)) {
            *op = d[0]; *arg = d[1]; *unit = (uint16_t)(d[2] << 8 | d[3]);
            return true;
        }
        if(pass) { break; }
        /* syndrome: recompute parity from the data bits, flip a data bit whose 4 parity checks all fail */
        uint8_t db[56], pb[56], syn[56];
        for(int i = 0; i < 56; i++) { db[i] = (uint8_t)((d[i / 8] >> (i % 8)) & 1u); pb[i] = (uint8_t)((d[7 + i / 8] >> (i % 8)) & 1u); }
        for(int i = 0; i < 56; i++) {
            uint8_t s = db[i];
            if(i >= 2) { s ^= db[i - 2]; }
            if(i >= 5) { s ^= db[i - 5]; }
            if(i >= 6) { s ^= db[i - 6]; }
            syn[i] = (uint8_t)(s ^ pb[i]);
        }
        for(int i = 0; i < 50; i++) {
            if(syn[i] && syn[i + 2] && syn[i + 5] && syn[i + 6]) {
                d[i / 8] ^= (uint8_t)(1u << (i % 8));
                syn[i] = syn[i + 2] = syn[i + 5] = syn[i + 6] = 0;
            }
        }
    }
    return false;
}
