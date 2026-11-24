/* Block codes and CRCs. Generator matrices transcribed from ETSI TS 102 361-1 V2.7.1
 * annex B (tables B.11 - B.18); the unit tests check the minimum distance of every code
 * and the spec's own Idle-message example (annex D), which catches transcription errors.
 */
#include "fec.h"
#include "bits.h"

/* ---------------- systematic linear block code helper ---------------- */
typedef struct {
    uint8_t n, k, nparity;
    const uint32_t *rows;   /* parity bits contributed by data bit i (i = 0 is the first/MSB data bit) */
} lincode_t;

static uint32_t lc_parity(const lincode_t *c, const uint8_t *b)
{
    uint32_t p = 0;
    for (unsigned i = 0; i < c->k; i++) if (b[i] & 1u) p ^= c->rows[i];
    return p;
}

static void lc_enc(const lincode_t *c, uint8_t *b)
{
    uint32_t p = lc_parity(c, b);
    for (unsigned i = 0; i < c->nparity; i++) b[c->k + i] = (uint8_t)((p >> (c->nparity - 1 - i)) & 1u);
}

/* single error correction by syndrome match; returns 0, 1 or -1 */
static int lc_dec(const lincode_t *c, uint8_t *b)
{
    uint32_t rx = 0;
    for (unsigned i = 0; i < c->nparity; i++) rx = (rx << 1) | (b[c->k + i] & 1u);
    uint32_t s = rx ^ lc_parity(c, b);
    if (!s) return 0;
    for (unsigned i = 0; i < c->k; i++) {
        if (c->rows[i] == s) { b[i] ^= 1u; return 1; }
    }
    for (unsigned i = 0; i < c->nparity; i++) {
        if (s == (1u << (c->nparity - 1 - i))) { b[c->k + i] ^= 1u; return 1; }
    }
    return -1;
}

/* Table B.15 */
static const uint32_t H15_11[11] = { 0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3 };
/* Table B.14 */
static const uint32_t H13_9[9] = { 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3 };
/* Table B.16 */
static const uint32_t H16_11[11] = { 0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15, 0x0B, 0x16, 0x19, 0x0D, 0x07 };
/* Table B.13 */
static const uint32_t H17_12[12] = { 0x1B, 0x1F, 0x1D, 0x1C, 0x0E, 0x07, 0x11, 0x1A, 0x0D, 0x14, 0x0A, 0x05 };
/* Table B.17 */
static const uint32_t H7_4[4] = { 0x5, 0x7, 0x6, 0x3 };

static const lincode_t C15_11 = { 15, 11, 4, H15_11 };
static const lincode_t C13_9  = { 13, 9, 4, H13_9 };
static const lincode_t C16_11 = { 16, 11, 5, H16_11 };
static const lincode_t C17_12 = { 17, 12, 5, H17_12 };
static const lincode_t C7_4   = { 7, 4, 3, H7_4 };

void fec_hamming_15_11_enc(uint8_t *b) { lc_enc(&C15_11, b); }
int  fec_hamming_15_11_dec(uint8_t *b) { return lc_dec(&C15_11, b); }
void fec_hamming_13_9_enc(uint8_t *b)  { lc_enc(&C13_9, b); }
int  fec_hamming_13_9_dec(uint8_t *b)  { return lc_dec(&C13_9, b); }
void fec_hamming_17_12_enc(uint8_t *b) { lc_enc(&C17_12, b); }
int  fec_hamming_17_12_dec(uint8_t *b) { return lc_dec(&C17_12, b); }
void fec_hamming_7_4_enc(uint8_t *b)   { lc_enc(&C7_4, b); }
int  fec_hamming_7_4_dec(uint8_t *b)   { return lc_dec(&C7_4, b); }
void fec_hamming_16_11_enc(uint8_t *b) { lc_enc(&C16_11, b); }
int  fec_hamming_16_11_dec(uint8_t *b)
{
    /* d = 4: a syndrome that matches no column is a detected double error */
    return lc_dec(&C16_11, b);
}

/* ---------------- Golay (20,8) table B.11 ---------------- */
static const uint16_t GOLAY2087_ROWS[8] = { 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB };

uint32_t fec_golay2087_enc(uint8_t data)
{
    uint32_t p = 0;
    for (int i = 0; i < 8; i++) if (data & (0x80u >> i)) p ^= GOLAY2087_ROWS[i];
    return ((uint32_t)data << 12) | p;
}

int fec_golay2087_dec(uint32_t cw, int *errs)
{
    /* d = 8: exhaustive nearest codeword over the 256 words (cheap: one slot type per 30 ms) */
    int best = -1, bestd = 99;
    cw &= 0xFFFFFu;
    for (int d = 0; d < 256; d++) {
        int dist = popcount32(fec_golay2087_enc((uint8_t)d) ^ cw);
        if (dist < bestd) { bestd = dist; best = d; if (!dist) break; }
    }
    if (errs) *errs = bestd;
    return bestd <= 3 ? best : -1;
}

/* ---------------- QR (16,7,6) table B.12 ---------------- */
static const uint16_t QR1676_ROWS[7] = { 0x04F, 0x11E, 0x1B7, 0x1E2, 0x1C9, 0x0E5, 0x073 };

uint16_t fec_qr1676_enc(uint8_t data7)
{
    uint16_t p = 0;
    for (int i = 0; i < 7; i++) if (data7 & (0x40u >> i)) p ^= QR1676_ROWS[i];
    return (uint16_t)(((data7 & 0x7Fu) << 9) | p);
}

int fec_qr1676_dec(uint16_t cw, int *errs)
{
    int best = -1, bestd = 99;
    for (int d = 0; d < 128; d++) {
        int dist = popcount32((uint32_t)(fec_qr1676_enc((uint8_t)d) ^ cw));
        if (dist < bestd) { bestd = dist; best = d; if (!dist) break; }
    }
    if (errs) *errs = bestd;
    return bestd <= 2 ? best : -1;
}

/* ---------------- RS(12,9) ---------------- */
uint8_t fec_gf256_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1u) r ^= a;
        b >>= 1;
        a = (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1Du : 0u));   /* x^8 = x^4+x^3+x^2+1 */
    }
    return r;
}

static uint8_t gf_pow_alpha(unsigned e)
{
    uint8_t r = 1;
    e %= 255u;
    while (e--) r = fec_gf256_mul(r, 2);
    return r;
}

/* Table B.18 parity columns (p2 p1 p0) for message octet i (row i) */
static const uint8_t RS129_G[9][3] = {
    { 0x1C, 0xBC, 0xFD }, { 0x89, 0x31, 0x08 }, { 0xAD, 0x41, 0x36 },
    { 0x7D, 0x71, 0x16 }, { 0xF3, 0xA6, 0x3A }, { 0x08, 0x83, 0x7B },
    { 0x3F, 0x6F, 0x02 }, { 0x6C, 0x0D, 0xA7 }, { 0x0E, 0x38, 0x40 },
};

void fec_rs129_enc(const uint8_t msg[9], uint8_t parity[3])
{
    parity[0] = parity[1] = parity[2] = 0;
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 3; j++) parity[j] ^= fec_gf256_mul(msg[i], RS129_G[i][j]);
}

int fec_rs129_dec(uint8_t cw[12])
{
    /* c(x) = cw[0] x^11 + ... + cw[11]; syndromes S_j = c(alpha^j), j = 1..3 */
    uint8_t s[3];
    for (int j = 0; j < 3; j++) {
        uint8_t a = gf_pow_alpha((unsigned)j + 1u), acc = 0;
        for (int i = 0; i < 12; i++) acc = (uint8_t)(fec_gf256_mul(acc, a) ^ cw[i]);
        s[j] = acc;
    }
    if (!s[0] && !s[1] && !s[2]) return 0;
    if (!s[0] || !s[1]) return -1;
    /* single error e at x^k: S1 = e a^k, S2 = e a^2k, S3 = e a^3k */
    for (unsigned k = 0; k < 12; k++) {
        uint8_t ak = gf_pow_alpha(k);
        if (fec_gf256_mul(s[0], ak) != s[1]) continue;
        if (fec_gf256_mul(s[1], ak) != s[2]) return -1;
        uint8_t e = fec_gf256_mul(s[0], gf_pow_alpha(255u - k));
        cw[11 - k] ^= e;
        return 1;
    }
    return -1;
}

/* ---------------- CRCs ---------------- */
uint32_t crc_bits(const uint8_t *bits, unsigned nbits, uint32_t poly, unsigned width, uint32_t init)
{
    uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    uint32_t top = 1u << (width - 1);
    uint32_t crc = init & mask;
    for (unsigned i = 0; i < nbits; i++) {
        uint32_t fb = ((crc & top) ? 1u : 0u) ^ (bits[i] & 1u);
        crc = (crc << 1) & mask;
        if (fb) crc ^= poly & mask;
    }
    return crc;
}

static uint32_t crc_bytes(const uint8_t *bytes, size_t len, uint32_t poly, unsigned width, uint32_t crc)
{
    uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    uint32_t top = 1u << (width - 1);
    for (size_t i = 0; i < len; i++)
        for (int b = 7; b >= 0; b--) {
            uint32_t fb = ((crc & top) ? 1u : 0u) ^ ((bytes[i] >> b) & 1u);
            crc = (crc << 1) & mask;
            if (fb) crc ^= poly & mask;
        }
    return crc;
}

uint8_t crc8_dmr(const uint8_t *bits, unsigned nbits)
{
    return (uint8_t)crc_bits(bits, nbits, 0x07u, 8, 0);          /* x^8+x^2+x+1 */
}

uint16_t crc_ccitt_dmr(const uint8_t *bytes, size_t len)
{
    return (uint16_t)(crc_bytes(bytes, len, 0x1021u, 16, 0) ^ 0xFFFFu);
}

uint16_t crc9_dmr(const uint8_t *bytes, size_t len, uint8_t dbsn)
{
    uint32_t crc = crc_bytes(bytes, len, 0x059u, 9, 0);           /* x^9+x^6+x^4+x^3+1 */
    for (int b = 6; b >= 0; b--) {
        uint32_t fb = ((crc >> 8) & 1u) ^ ((dbsn >> b) & 1u);
        crc = (crc << 1) & 0x1FFu;
        if (fb) crc ^= 0x059u;
    }
    return (uint16_t)(crc ^ 0x1FFu);
}

uint32_t crc32_dmr(const uint8_t *bytes, size_t len)
{
    return crc32_dmr_cont(0, bytes, len);
}

uint32_t crc32_dmr_cont(uint32_t crc, const uint8_t *bytes, size_t len)
{
    /* B.3.9: the octets are taken as 16-bit words (MSO, LSO) and linearised LSO first */
    for (size_t i = 0; i < len; i += 2) {
        uint8_t pair[2];
        size_t n = 0;
        if (i + 1 < len) pair[n++] = bytes[i + 1];
        pair[n++] = bytes[i];
        crc = crc_bytes(pair, n, 0x04C11DB7u, 32, crc);
    }
    return crc;
}

uint8_t crc7_dmr(uint8_t rc4)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)((rc4 >> (3 - i)) & 1u);
    return (uint8_t)crc_bits(b, 4, 0x27u, 7, 0);                  /* x^7+x^5+x^2+x+1 */
}

uint8_t cs5_dmr(const uint8_t lc[9])
{
    unsigned s = 0;
    for (int i = 0; i < 9; i++) s += lc[i];
    return (uint8_t)(s % 31u);
}
