/* FEC and CRC primitives for DMR (ETSI TS 102 361-1 annex B) and NXDN (NXDN TS 1-A 4.5).
 * Own implementation from the spec text; no lookup tables beyond the generator matrices
 * printed in the spec. No dynamic memory, no global state.
 */
#ifndef PROTO_FEC_H
#define PROTO_FEC_H
#include <stdint.h>
#include <stddef.h>

/* ---------------- Hamming codes (TS 102 361-1 B.3.3 - B.3.5) ----------------
 * Operate in place on a bit array: data bits first, parity bits last.
 * enc: fills the parity bits. dec: returns 0 = clean, 1 = one bit corrected,
 * -1 = uncorrectable (for (16,11,4): double error detected).
 */
void fec_hamming_15_11_enc(uint8_t *b);
int  fec_hamming_15_11_dec(uint8_t *b);
void fec_hamming_13_9_enc(uint8_t *b);
int  fec_hamming_13_9_dec(uint8_t *b);
void fec_hamming_16_11_enc(uint8_t *b);
int  fec_hamming_16_11_dec(uint8_t *b);
void fec_hamming_17_12_enc(uint8_t *b);
int  fec_hamming_17_12_dec(uint8_t *b);
void fec_hamming_7_4_enc(uint8_t *b);
int  fec_hamming_7_4_dec(uint8_t *b);

/* ---------------- Golay (20,8) (B.3.1): slot type ----------------
 * codeword = data << 12 | parity (20 bits, MSB first on air).
 * dec returns data (0..255) and sets *errs, or -1 if more than 3 errors.
 */
uint32_t fec_golay2087_enc(uint8_t data);
int      fec_golay2087_dec(uint32_t cw, int *errs);

/* ---------------- Quadratic residue (16,7,6) (B.3.2): EMB ----------------
 * codeword = data << 9 | parity. dec corrects up to 2 errors. */
uint16_t fec_qr1676_enc(uint8_t data7);
int      fec_qr1676_dec(uint16_t cw, int *errs);

/* ---------------- Reed-Solomon (12,9) over GF(2^8) (B.3.6) ----------------
 * cw[0..8] = message octets (LC octets 0..8), cw[9..11] = p2 p1 p0.
 * dec corrects one symbol error; returns symbols corrected (0/1) or -1. */
void fec_rs129_enc(const uint8_t msg[9], uint8_t parity[3]);
int  fec_rs129_dec(uint8_t cw[12]);
uint8_t fec_gf256_mul(uint8_t a, uint8_t b);    /* poly x^8+x^4+x^3+x^2+1 */

/* ---------------- CRCs (B.3.7 - B.3.13) ---------------- */
uint8_t  crc8_dmr(const uint8_t *bits, unsigned nbits);          /* G8, init 0, over a bit array */
uint16_t crc_ccitt_dmr(const uint8_t *bytes, size_t len);        /* B.3.8 incl. inversion, no mask */
uint16_t crc9_dmr(const uint8_t *bytes, size_t len, uint8_t dbsn); /* B.3.10 incl. inversion, no mask */
uint32_t crc32_dmr(const uint8_t *bytes, size_t len);            /* B.3.9 (word byte order handled); sent LS octet first */
uint32_t crc32_dmr_cont(uint32_t crc, const uint8_t *bytes, size_t len);   /* continue; len even */
uint8_t  crc7_dmr(uint8_t rc4);                                  /* B.3.13, RC info */
uint8_t  cs5_dmr(const uint8_t lc[9]);                           /* B.3.11 */

/* generic MSB-first CRC over a bit array (NXDN 4.5.5: init all ones) */
uint32_t crc_bits(const uint8_t *bits, unsigned nbits, uint32_t poly, unsigned width, uint32_t init);

/* ---------------- Rate 1/2 K=5 convolutional code (NXDN TS 1-A 4.5) ----------------
 * G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4; outputs G1,G2 per input bit.
 * The caller appends the 4 zero tail bits to the input.
 * puncture: pattern of keep flags over one period of output bits, e.g. "11101110111111"
 * means G1,G2 pairs column-wise as in the spec matrices; NULL = no puncturing.
 */
typedef struct {
    const uint8_t *keep;    /* keep[i] for i in 0..period-1, in G1,G2,G1,G2.. order */
    unsigned period;
} conv_punct_t;

/* encode nin bits (tail included) -> returns number of output bits written */
unsigned conv_k5_encode(const uint8_t *in, unsigned nin, uint8_t *out, const conv_punct_t *p);
/* Viterbi decode: in = received punctured bits (0/1), nout = number of decoded bits (tail incl).
 * Returns the path metric (number of bit disagreements, 0 = error free). */
int conv_k5_decode(const uint8_t *in, unsigned nin, uint8_t *out, unsigned nout, const conv_punct_t *p);

/* ---------------- DMR rate 3/4 trellis (B.2.4) ----------------
 * in: 18 octets (144 bits). out: 98 dibits in transmit order (Trellis_Dibit(97) first).
 * dec returns metric (0 = clean) or -1 when the best path does not end in state 0. */
void trellis34_encode(const uint8_t in[18], uint8_t dibits[98]);
int  trellis34_decode(const uint8_t dibits[98], uint8_t out[18]);

/* ---------------- DMR BPTC (B.1.1, B.2) ---------------- */
/* (196,96): info = 12 octets, bits[196] = TX(195)..TX(0) */
void bptc196_encode(const uint8_t info[12], uint8_t bits[196]);
int  bptc196_decode(const uint8_t bits[196], uint8_t info[12]);   /* corrected bits or -1 */

/* embedded LC (B.2.1): 72 bit LC (9 octets) + 5 bit checksum -> 128 bits (4 x 32, burst B..E) */
void emblc_encode(const uint8_t lc[9], uint8_t bits[128]);
int  emblc_decode(const uint8_t bits[128], uint8_t lc[9]);       /* -1 on FEC or checksum error */

/* CACH short LC (B.2.3): 28 bit SLC (lc[0] low nibble = SLCO, lc[1..3]) -> 68 bits (4 x 17) */
void cach_slc_encode(const uint8_t slc[4], uint8_t bits[68]);
int  cach_slc_decode(const uint8_t bits[68], uint8_t slc[4]);    /* -1 on FEC or CRC error */

/* single burst non-RC / RC BPTC (B.2.2): 11 bits -> 32 */
void sbbptc_encode(uint16_t info11, int rc, uint8_t bits[32]);
int  sbbptc_decode(const uint8_t bits[32], int rc, uint16_t *info11);

#endif
