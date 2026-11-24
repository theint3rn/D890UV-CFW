/*
 * p25_fec.c - FEC/CRC primitives for P25 (see p25_fec.h).
 *
 * Written from the TIA-102 descriptions; code constants (generator rows,
 * polynomials) were cross-checked against the published OP25 tables
 * (boatbod/op25, GPLv3) by the host tests - no OP25 code is copied here.
 */
#include "p25_fec.h"
#include <string.h>

/* ------------------------------------------------------------------ bits */
uint32_t p25_bits_get(const uint8_t *bytes, unsigned i, unsigned n)
{
    uint32_t v = 0;
    while (n--) v = (v << 1) | p25_bit_get(bytes, i++);
    return v;
}

void p25_bits_put(uint8_t *bytes, unsigned i, unsigned n, uint32_t v)
{
    while (n--) p25_bit_put(bytes, i++, (v >> n) & 1u);
}

uint32_t p25_dbits_get(const uint8_t *dib, unsigned k, unsigned n)
{
    uint32_t v = 0;
    while (n--) v = (v << 1) | p25_dbit_get(dib, k++);
    return v;
}

void p25_dbits_put(uint8_t *dib, unsigned k, unsigned n, uint32_t v)
{
    while (n--) p25_dbit_put(dib, k++, (v >> n) & 1u);
}

/* ---------------------------------------------------------------- Golay */
#define GOLAY_POLY 0xC75u   /* x^11+x^10+x^6+x^5+x^4+x^2+1 */

/* syndrome (remainder) of x^(11+i), i = 0..11 = parity of info bit i */
static const uint16_t golay_info_syn[12] = {
    0x475, 0x49f, 0x54b, 0x6e3, 0x1b3, 0x366, 0x6cc, 0x1ed, 0x3da, 0x7b4, 0x31d, 0x63a
};

static uint32_t golay23_syndrome(uint32_t cw)
{
    uint32_t s = cw & 0x7FFu;
    for (int i = 0; i < 12; i++)
        if (cw & (1u << (11 + i))) s ^= golay_info_syn[i];
    return s;
}

uint32_t p25_golay23_encode(uint32_t info12)
{
    info12 &= 0xFFFu;
    uint32_t p = 0;
    for (int i = 0; i < 12; i++)
        if (info12 & (1u << i)) p ^= golay_info_syn[i];
    return (info12 << 11) | p;
}

uint32_t p25_golay23_decode(uint32_t cw, int *errs)
{
    cw &= 0x7FFFFFu;
    uint32_t s = golay23_syndrome(cw);
    uint32_t e = 0;
    if (s == 0) {
        e = 0;
    } else if (p25_popcount32(s) <= 3) {
        e = s;                                   /* parity bits only */
    } else {
        int found = 0;
        for (int i = 0; i < 12 && !found; i++) {
            uint32_t s1 = s ^ golay_info_syn[i];
            if (p25_popcount32(s1) <= 2) { e = (1u << (11 + i)) | s1; found = 1; }
        }
        for (int i = 0; i < 12 && !found; i++)
            for (int j = i + 1; j < 12 && !found; j++) {
                uint32_t s2 = s ^ golay_info_syn[i] ^ golay_info_syn[j];
                if (p25_popcount32(s2) <= 1) { e = (1u << (11 + i)) | (1u << (11 + j)) | s2; found = 1; }
            }
        for (int i = 0; i < 12 && !found; i++)
            for (int j = i + 1; j < 12 && !found; j++)
                for (int k = j + 1; k < 12 && !found; k++)
                    if ((s ^ golay_info_syn[i] ^ golay_info_syn[j] ^ golay_info_syn[k]) == 0) {
                        e = (1u << (11 + i)) | (1u << (11 + j)) | (1u << (11 + k));
                        found = 1;
                    }
    }
    if (errs) *errs = (int)p25_popcount32(e);
    return ((cw ^ e) >> 11) & 0xFFFu;
}

uint32_t p25_golay24_encode(uint32_t info12)
{
    uint32_t c = p25_golay23_encode(info12);
    return (c << 1) | (p25_popcount32(c) & 1u);
}

int p25_golay24_decode(uint32_t cw24, int *errs)
{
    int e23;
    uint32_t info = p25_golay23_decode(cw24 >> 1, &e23);
    uint32_t re = p25_golay24_encode(info);
    int e = (int)p25_popcount32((re ^ cw24) & 0xFFFFFFu);
    if (errs) *errs = e;
    if (e > 3) return -1;
    return (int)info;
}

int p25_golay18_decode(uint32_t cw18, int *errs)
{
    int e;
    int info = p25_golay24_decode(cw18 & 0x3FFFFu, &e);
    if (errs) *errs = e;
    if (info < 0 || info > 0x3F) return -1;
    return info;
}

/* -------------------------------------------------------------- Hamming */
static const uint16_t ham15_rows[11] = {   /* info bit 10..0 -> code word */
    0x400f, 0x200e, 0x100d, 0x080c, 0x040b, 0x020a, 0x0109, 0x0087, 0x0046, 0x0025, 0x0013
};
static const uint16_t ham10_rows[6] = { 0x20e, 0x10d, 0x08b, 0x047, 0x023, 0x01c };

static uint32_t lin_encode(const uint16_t *rows, int k, uint32_t info)
{
    uint32_t c = 0;
    for (int i = 0; i < k; i++)
        if (info & (1u << (k - 1 - i))) c ^= rows[i];
    return c;
}

/* single error correction for systematic codes with 4 parity bits */
static uint32_t ham_decode(const uint16_t *rows, int k, uint32_t cw, int *errs)
{
    uint32_t info = cw >> 4;
    uint32_t syn = (lin_encode(rows, k, info) ^ cw) & 0xFu;
    int e = 0;
    if (syn) {
        e = 2;                                     /* assume uncorrectable */
        if (p25_popcount32(syn) == 1) e = 1;       /* parity bit error */
        else
            for (int i = 0; i < k; i++)
                if ((rows[i] & 0xFu) == syn) { info ^= 1u << (k - 1 - i); e = 1; break; }
    }
    if (errs) *errs = e;
    return info;
}

uint32_t p25_hamming15_encode(uint32_t info11) { return lin_encode(ham15_rows, 11, info11 & 0x7FF); }
uint32_t p25_hamming15_decode(uint32_t cw15, int *errs) { return ham_decode(ham15_rows, 11, cw15 & 0x7FFF, errs); }
uint32_t p25_hamming10_encode(uint32_t info6) { return lin_encode(ham10_rows, 6, info6 & 0x3F); }
uint32_t p25_hamming10_decode(uint32_t cw10, int *errs) { return ham_decode(ham10_rows, 6, cw10 & 0x3FF, errs); }

/* --------------------------------------------------------- cyclic(16,8) */
static const uint16_t cyc16_rows[8] = { 0x804e, 0x4027, 0x208f, 0x10db, 0x08f1, 0x04e4, 0x0272, 0x0139 };

uint32_t p25_cyclic16_encode(uint32_t info8) { return lin_encode(cyc16_rows, 8, info8 & 0xFF); }

int p25_cyclic16_decode(uint32_t cw16, int *errs)
{
    int best = -1, bestd = 99;
    for (uint32_t m = 0; m < 256; m++) {
        int d = (int)p25_popcount32(p25_cyclic16_encode(m) ^ (cw16 & 0xFFFF));
        if (d < bestd) { bestd = d; best = (int)m; if (d == 0) break; }
    }
    if (errs) *errs = bestd;
    return bestd <= 2 ? best : -1;
}

/* --------------------------------------------------------------- GF(64) */
static const uint8_t gf_exp[63] = {
    1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 48, 35, 5, 10, 20, 40, 19, 38, 15, 30, 60, 59, 53, 41, 17, 34, 7, 14,
    28, 56, 51, 37, 9, 18, 36, 11, 22, 44, 27, 54, 47, 29, 58, 55, 45, 25, 50, 39, 13, 26, 52, 43, 21, 42,
    23, 46, 31, 62, 63, 61, 57, 49, 33
};
static const int8_t gf_log[64] = {
    -1, 0, 1, 6, 2, 12, 7, 26, 3, 32, 13, 35, 8, 48, 27, 18, 4, 24, 33, 16, 14, 52, 36, 54, 9, 45, 49, 38,
    28, 41, 19, 56, 5, 62, 25, 11, 34, 31, 17, 47, 15, 23, 53, 51, 37, 44, 55, 40, 10, 61, 46, 30, 50, 22,
    39, 43, 29, 60, 42, 21, 20, 59, 57, 58
};

uint8_t p25_gf64_exp(unsigned i) { return gf_exp[i % 63]; }
int     p25_gf64_log(uint8_t a) { return gf_log[a & 63]; }

uint8_t p25_gf64_mul(uint8_t a, uint8_t b)
{
    if (!a || !b) return 0;
    return gf_exp[(gf_log[a & 63] + gf_log[b & 63]) % 63];
}

static uint8_t gf_inv(uint8_t a) { return gf_exp[(63 - gf_log[a]) % 63]; }

/* --------------------------------------------- generic BM error locator */
/* S[1..ns] syndromes; lambda[0..ns] in/out (preloaded with erasure locator
 * of degree n_eras). Returns degree L of the final locator. */
static int berlekamp_massey(const uint8_t *S, int ns, uint8_t *lambda, int n_eras)
{
    uint8_t B[64], T[64];
    int L = n_eras;
    memcpy(B, lambda, (size_t)ns + 1);
    for (int r = n_eras + 1; r <= ns; r++) {
        uint8_t delta = 0;
        for (int i = 0; i < r && i <= ns; i++)
            if (lambda[i] && S[r - i]) delta ^= p25_gf64_mul(lambda[i], S[r - i]);
        /* B = x*B */
        memmove(B + 1, B, (size_t)ns);
        B[0] = 0;
        if (delta) {
            for (int i = 0; i <= ns; i++) T[i] = lambda[i] ^ p25_gf64_mul(delta, B[i]);
            if (2 * L <= r + n_eras - 1) {
                L = r + n_eras - L;
                uint8_t di = gf_inv(delta);
                /* B = lambda / delta (undo the x shift: B was shifted above) */
                for (int i = 0; i <= ns; i++) B[i] = p25_gf64_mul(lambda[i], di);
            }
            memcpy(lambda, T, (size_t)ns + 1);
        }
    }
    return L;
}

/* ------------------------------------------------------------- BCH NID */
/* parity words (47 bits, x^62..x^16 coefficients follow info) are computed by
 * polynomial division with g(x) = 6331141367235453 (octal). */
static const uint64_t BCH_G = 0xCD930BDD3B2BULL; /* = 06331141367235453 (octal), degree 47 */

static uint64_t bch63_encode(uint16_t info)
{
    uint64_t r = (uint64_t)info << 47;
    for (int i = 62; i >= 47; i--)
        if (r & (1ULL << i)) r ^= BCH_G << (i - 47);
    return ((uint64_t)info << 47) | r;
}

uint64_t p25_nid_encode(uint16_t nac, uint8_t duid)
{
    uint16_t info = (uint16_t)(((nac & 0xFFFu) << 4) | (duid & 0xFu));
    uint64_t cw = bch63_encode(info);
    /* 64th bit: TIA-102.BAAA parity rule, effectively duid bit0 ^ duid bit1
     * (1 for LDU1/LDU2, 0 for HDU/TDU/TSBK/PDU/TDULC). */
    unsigned p = (duid ^ (duid >> 1)) & 1u;
    return (cw << 1) | p;
}

int p25_nid_decode(uint64_t nid, uint16_t *nac, uint8_t *duid, int *errs)
{
    uint64_t r = nid >> 1;                      /* 63-bit BCH word, bit i = x^i */
    uint8_t S[23];
    int any = 0;
    for (int j = 1; j <= 22; j++) {
        uint8_t s = 0;
        for (int i = 0; i < 63; i++)
            if (r & (1ULL << i)) s ^= gf_exp[(i * j) % 63];
        S[j] = s;
        any |= s;
    }
    int nerr = 0;
    if (any) {
        uint8_t lambda[23];
        memset(lambda, 0, sizeof lambda);
        lambda[0] = 1;
        int L = berlekamp_massey(S, 22, lambda, 0);
        if (L > 11) return -1;
        for (int i = 0; i < 63; i++) {
            /* evaluate lambda(alpha^-i) */
            uint8_t v = 0;
            for (int k = 0; k <= L; k++)
                if (lambda[k]) v ^= p25_gf64_mul(lambda[k], gf_exp[(63 * 11 - i * k) % 63]);
            if (v == 0) { r ^= 1ULL << i; nerr++; }
        }
        if (nerr != L) return -1;
        if (bch63_encode((uint16_t)(r >> 47)) != r) return -1;
    }
    if (nac) *nac = (uint16_t)((r >> 51) & 0xFFFu);
    if (duid) *duid = (uint8_t)((r >> 47) & 0xFu);
    if (errs) *errs = nerr;
    return 0;
}

/* ------------------------------------------------------- Reed-Solomon */
static void rs_genpoly(unsigned nroots, uint8_t *g)  /* g[0..nroots], g[nroots] = 1 (x^nroots) */
{
    memset(g, 0, nroots + 1);
    g[0] = 1;                                   /* g(x) = 1, g[i] = coeff of x^i */
    for (unsigned i = 1; i <= nroots; i++) {
        uint8_t root = gf_exp[i];               /* fcr = 1 */
        /* g = g * (x + root) */
        for (int j = (int)i; j > 0; j--)
            g[j] = g[j - 1] ^ p25_gf64_mul(g[j], root);
        g[0] = p25_gf64_mul(g[0], root);
    }
}

void p25_rs_encode(const uint8_t *msg, unsigned k, unsigned nroots, uint8_t *parity)
{
    uint8_t g[64], reg[64];
    rs_genpoly(nroots, g);
    memset(reg, 0, sizeof reg);                 /* reg[0] = highest remainder coeff */
    for (unsigned i = 0; i < k; i++) {
        uint8_t fb = (uint8_t)((msg[i] & 63) ^ reg[0]);
        memmove(reg, reg + 1, nroots - 1);
        reg[nroots - 1] = 0;
        if (fb)
            for (unsigned j = 0; j < nroots; j++)
                reg[j] ^= p25_gf64_mul(fb, g[nroots - 1 - j]);
    }
    memcpy(parity, reg, nroots);
}

int p25_rs_decode(uint8_t *cw, unsigned n, unsigned nroots, const uint8_t *eras, unsigned n_eras)
{
    if (n > 63 || nroots >= n || n_eras > nroots) return -1;
    uint8_t S[64];
    int any = 0;
    for (unsigned j = 1; j <= nroots; j++) {
        uint8_t a = gf_exp[j], s = 0;
        for (unsigned m = 0; m < n; m++) s = (uint8_t)(p25_gf64_mul(s, a) ^ (cw[m] & 63));
        S[j] = s;
        any |= s;
    }
    if (!any) return 0;

    uint8_t lambda[64];
    memset(lambda, 0, sizeof lambda);
    lambda[0] = 1;
    for (unsigned e = 0; e < n_eras; e++) {
        uint8_t X = gf_exp[(n - 1 - eras[e]) % 63];
        for (int i = (int)e + 1; i > 0; i--) lambda[i] ^= p25_gf64_mul(lambda[i - 1], X);
    }
    int L = berlekamp_massey(S, (int)nroots, lambda, (int)n_eras);
    if ((unsigned)(2 * L) > nroots + n_eras) return -1;

    /* omega = S(x) * lambda(x) mod x^nroots, S(x) = sum S[j+1] x^j */
    uint8_t omega[64];
    for (unsigned i = 0; i < nroots; i++) {
        uint8_t v = 0;
        for (unsigned j = 0; j <= i && j <= (unsigned)L; j++) v ^= p25_gf64_mul(lambda[j], S[i - j + 1]);
        omega[i] = v;
    }
    int nfound = 0;
    uint8_t fixpos[64], fixval[64];
    for (unsigned p = 0; p < 63; p++) {       /* location X = alpha^p, symbol index n-1-p */
        unsigned xinv = (63 - p) % 63;
        uint8_t v = 0, d = 0, o = 0;
        for (int k = 0; k <= L; k++)
            if (lambda[k]) {
                uint8_t t = p25_gf64_mul(lambda[k], gf_exp[(xinv * (unsigned)k) % 63]);
                v ^= t;
                if (k & 1) d ^= p25_gf64_mul(lambda[k], gf_exp[(xinv * (unsigned)(k - 1)) % 63]);
            }
        if (v) continue;
        if (p >= n || !d) return -1;             /* root outside the (shortened) code */
        for (unsigned i = 0; i < nroots; i++)
            if (omega[i]) o ^= p25_gf64_mul(omega[i], gf_exp[(xinv * i) % 63]);
        fixpos[nfound] = (uint8_t)(n - 1 - p);
        fixval[nfound] = p25_gf64_mul(o, gf_inv(d));
        nfound++;
    }
    if (nfound != L) return -1;
    int corrected = 0;
    for (int i = 0; i < nfound; i++)
        if (fixval[i]) { cw[fixpos[i]] ^= fixval[i]; corrected++; }
    /* verify */
    for (unsigned j = 1; j <= nroots; j++) {
        uint8_t a = gf_exp[j], s = 0;
        for (unsigned m = 0; m < n; m++) s = (uint8_t)(p25_gf64_mul(s, a) ^ (cw[m] & 63));
        if (s) return -1;
    }
    return corrected;
}

int p25_rs_decode_punct(uint8_t *cw, unsigned n_full, unsigned nroots, unsigned punct)
{
    uint8_t eras[64];
    for (unsigned i = 0; i < punct; i++) {
        eras[i] = (uint8_t)(n_full - punct + i);
        cw[n_full - punct + i] = 0;
    }
    return p25_rs_decode(cw, n_full, nroots, eras, punct);
}

/* -------------------------------------------------------------- trellis */
/* constellation point -> dibit pair (4 bits: first dibit in bits 3..2) */
static const uint8_t const_to_nib[16] = { 2, 10, 7, 15, 14, 6, 11, 3, 13, 5, 8, 0, 1, 9, 4, 12 };
static const uint8_t t12[4][4] = {
    { 0, 15, 12, 3 }, { 4, 11, 8, 7 }, { 13, 2, 1, 14 }, { 9, 6, 5, 10 }
};
static const uint8_t t34[8][8] = {
    { 0, 8, 4, 12, 2, 10, 6, 14 }, { 4, 12, 2, 10, 6, 14, 0, 8 },
    { 1, 9, 5, 13, 3, 11, 7, 15 }, { 5, 13, 3, 11, 7, 15, 1, 9 },
    { 3, 11, 7, 15, 1, 9, 5, 13 }, { 7, 15, 1, 9, 5, 13, 3, 11 },
    { 2, 10, 6, 14, 0, 8, 4, 12 }, { 6, 14, 0, 8, 4, 12, 2, 10 }
};

/* data interleave: symbol i (0..48) is sent at position */
static unsigned tr_pos(unsigned i)
{
    if (i < 13) return 4 * i;
    if (i < 25) return 4 * (i - 13) + 1;
    if (i < 37) return 4 * (i - 25) + 2;
    return 4 * (i - 37) + 3;
}

static void tr_encode(const uint8_t *in, unsigned bits_per_sym, const uint8_t *tab, unsigned nstates,
                      uint8_t *out)
{
    unsigned state = 0;
    for (unsigned i = 0; i < 49; i++) {
        unsigned x = i < 48 ? p25_bits_get(in, i * bits_per_sym, bits_per_sym) : 0;
        uint8_t nib = const_to_nib[tab[state * nstates + x]];
        unsigned p = tr_pos(i);
        out[2 * p] = nib >> 2;
        out[2 * p + 1] = nib & 3;
        state = x;
    }
}

static int tr_decode(const uint8_t *in, unsigned bits_per_sym, const uint8_t *tab, unsigned nstates,
                     uint8_t *out, unsigned out_bytes)
{
    uint16_t metric[8], nm[8];
    uint8_t surv[49][8];
    for (unsigned s = 0; s < nstates; s++) metric[s] = s ? 1000 : 0;
    for (unsigned i = 0; i < 49; i++) {
        unsigned p = tr_pos(i);
        unsigned r = ((in[2 * p] & 3u) << 2) | (in[2 * p + 1] & 3u);
        for (unsigned ns = 0; ns < nstates; ns++) {
            uint16_t best = 0xFFFF;
            uint8_t bp = 0;
            for (unsigned ps = 0; ps < nstates; ps++) {
                uint16_t m = (uint16_t)(metric[ps] + p25_popcount32(const_to_nib[tab[ps * nstates + ns]] ^ r));
                if (m < best) { best = m; bp = (uint8_t)ps; }
            }
            nm[ns] = best;
            surv[i][ns] = bp;
        }
        memcpy(metric, nm, sizeof metric);
    }
    /* flush symbol forces final state 0 */
    memset(out, 0, out_bytes);
    unsigned s = 0;
    for (int i = 48; i > 0; i--) {
        s = surv[i][s];                         /* state before step i = input of step i-1 */
        p25_bits_put(out, (unsigned)(i - 1) * bits_per_sym, bits_per_sym, s);
    }
    return metric[0];
}

void p25_trellis12_encode(const uint8_t in[12], uint8_t out[98]) { tr_encode(in, 2, &t12[0][0], 4, out); }
int  p25_trellis12_decode(const uint8_t in[98], uint8_t out[12]) { return tr_decode(in, 2, &t12[0][0], 4, out, 12); }
void p25_trellis34_encode(const uint8_t in[18], uint8_t out[98]) { tr_encode(in, 3, &t34[0][0], 8, out); }
int  p25_trellis34_decode(const uint8_t in[98], uint8_t out[18]) { return tr_decode(in, 3, &t34[0][0], 8, out, 18); }

/* ----------------------------------------------------------------- CRCs */
static uint32_t crc_bits(const uint8_t *b, unsigned nbits, uint32_t poly, unsigned width, uint32_t xorout)
{
    uint32_t crc = 0, top = 1u << (width - 1), mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
    for (unsigned i = 0; i < nbits; i++) {
        uint32_t fb = ((crc & top) ? 1u : 0u) ^ p25_bit_get(b, i);
        crc = (crc << 1) & mask;
        if (fb) crc ^= poly;
    }
    return (crc ^ xorout) & mask;
}

uint16_t p25_crc16(const uint8_t *b, unsigned n) { return (uint16_t)crc_bits(b, n, 0x1021u, 16, 0xFFFFu); }
uint16_t p25_crc9(const uint8_t *b, unsigned n)  { return (uint16_t)crc_bits(b, n, 0x059u, 9, 0x1FFu); }
uint32_t p25_crc32(const uint8_t *b, unsigned n) { return crc_bits(b, n, 0x04C11DB7u, 32, 0xFFFFFFFFu); }
uint16_t p25_crc12(const uint8_t *b, unsigned n) { return (uint16_t)crc_bits(b, n, 0x897u, 12, 0xFFFu); }

/* ------------------------------------------------------------------ PN */
uint32_t p25_pn_bits(uint32_t *state, unsigned nbits)
{
    uint32_t v = 0, p = *state;
    while (nbits--) {
        p = (173u * p + 13849u) & 0xFFFFu;
        v = (v << 1) | (p >> 15);
    }
    *state = p;
    return v;
}
