/*
 * p25_crypt.c - P25 voice encryption with user keys (see p25_crypt.h).
 * AES-256 (FIPS-197), DES (FIPS 46-3) and ARC4 are written from their public
 * specifications. The P25 keystream layout follows TIA-102.AAAD; it was
 * cross-checked against the description in OP25 (GPLv3, op25_crypt_*.cc).
 * No OP25 code is copied.
 */
#include "p25_crypt.h"
#include <string.h>

/* ---------------------------------------------------------------- keyring */
void p25_keyring_init(p25_keyring_t *r) { memset(r, 0, sizeof *r); }

static unsigned alg_keylen(uint8_t algid)
{
    switch (algid) {
    case P25_ALG_DES_OFB: return 8;
    case P25_ALG_AES256:  return 32;
    case P25_ALG_ADP:     return 5;
    default:              return 0;
    }
}

bool p25_crypt_supported(uint8_t algid) { return alg_keylen(algid) != 0; }

bool p25_keyring_add(p25_keyring_t *r, uint8_t algid, uint16_t kid, const uint8_t *key, size_t len)
{
    unsigned kl = alg_keylen(algid);
    if (!kl || len == 0 || len > kl) return false;
    p25_key_t *k = NULL;
    for (unsigned i = 0; i < r->n; i++)
        if (r->k[i].algid == algid && r->k[i].kid == kid) k = &r->k[i];
    if (!k) {
        if (r->n >= P25_CRYPT_MAX_KEYS) return false;
        k = &r->k[r->n++];
    }
    memset(k, 0, sizeof *k);
    k->algid = algid;
    k->kid = kid;
    k->len = (uint8_t)kl;
    memcpy(k->key + (kl - len), key, len);          /* left-pad with zeros */
    return true;
}

const p25_key_t *p25_keyring_find(const p25_keyring_t *r, uint8_t algid, uint16_t kid)
{
    for (unsigned i = 0; i < r->n; i++)
        if (r->k[i].algid == algid && r->k[i].kid == kid) return &r->k[i];
    return NULL;
}

void p25_keyring_wipe(p25_keyring_t *r)
{
    volatile uint8_t *p = (volatile uint8_t *)r;
    for (size_t i = 0; i < sizeof *r; i++) p[i] = 0;
}

/* ------------------------------------------------------------- MI / IV */
static uint64_t mi64(const uint8_t mi[9])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | mi[i];
    return v;
}

static unsigned lfsr_step(uint64_t *r)
{
    uint64_t x = *r;
    unsigned out = (unsigned)(x >> 63) & 1u;
    uint64_t fb = ((x >> 63) ^ (x >> 61) ^ (x >> 45) ^ (x >> 37) ^ (x >> 26) ^ (x >> 14)) & 1u;
    *r = (x << 1) | fb;
    return out;
}

void p25_mi_next(uint8_t mi[9])
{
    uint64_t r = mi64(mi);
    for (int i = 0; i < 64; i++) lfsr_step(&r);
    for (int i = 7; i >= 0; i--) { mi[i] = (uint8_t)r; r >>= 8; }
    mi[8] = 0;
}

void p25_mi_to_iv128(const uint8_t mi[9], uint8_t iv[16])
{
    /* the 64 bits shifted out are the MI itself, the register then holds the
     * next 64 sequence bits */
    uint64_t r = mi64(mi);
    memcpy(iv, mi, 8);
    for (int i = 0; i < 64; i++) lfsr_step(&r);
    for (int i = 15; i >= 8; i--) { iv[i] = (uint8_t)r; r >>= 8; }
}

/* ---------------------------------------------------------------- AES */
static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes256_expand(const uint8_t key[32], uint8_t rk[240])
{
    uint8_t rcon = 1;
    memcpy(rk, key, 32);
    for (unsigned i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, rk + 4 * (i - 1), 4);
        if (i % 8 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[4 * i + j] = (uint8_t)(rk[4 * (i - 8) + j] ^ t[j]);
    }
}

static void aes256_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int r = 1; r <= 14; r++) {
        /* SubBytes + ShiftRows (state is column-major: s[4*c + row]) */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * c + row] = sbox[s[4 * ((c + row) & 3) + row]];
        if (r != 14)
            for (int c = 0; c < 4; c++) {           /* MixColumns */
                uint8_t *p = t + 4 * c, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
            }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[16 * r + i]);
    }
    memcpy(out, s, 16);
}

void p25_aes256_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[240];
    aes256_expand(key, rk);
    aes256_block(rk, in, out);
    memset(rk, 0, sizeof rk);
}

/* ---------------------------------------------------------------- DES */
static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const uint8_t des_e[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const uint8_t des_p[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27,
    19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const uint8_t des_pc2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t des_shift[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t des_s[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
};

/* permutation tables count bits from 1 = MSB of an `inbits`-wide value */
static uint64_t perm(uint64_t in, const uint8_t *t, unsigned n, unsigned inbits)
{
    uint64_t o = 0;
    for (unsigned i = 0; i < n; i++) o = (o << 1) | ((in >> (inbits - t[i])) & 1u);
    return o;
}

static void des_subkeys(const uint8_t key[8], uint64_t sk[16])
{
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | key[i];
    uint64_t cd = perm(k, des_pc1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28) & 0xFFFFFFFu, d = (uint32_t)cd & 0xFFFFFFFu;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < des_shift[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFFu;
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFFu;
        }
        sk[r] = perm(((uint64_t)c << 28) | d, des_pc2, 48, 56);
    }
}

static uint64_t des_block(const uint64_t sk[16], uint64_t in)
{
    uint64_t x = perm(in, des_ip, 64, 64);
    uint32_t l = (uint32_t)(x >> 32), r = (uint32_t)x;
    for (int i = 0; i < 16; i++) {
        uint64_t e = perm(r, des_e, 48, 32) ^ sk[i];
        uint32_t f = 0;
        for (int b = 0; b < 8; b++) {
            unsigned six = (unsigned)(e >> (42 - 6 * b)) & 0x3Fu;
            unsigned row = ((six >> 4) & 2u) | (six & 1u), col = (six >> 1) & 0xFu;
            f = (f << 4) | des_s[b][row * 16 + col];
        }
        f = (uint32_t)perm(f, des_p, 32, 32);
        uint32_t nl = r;
        r = l ^ f;
        l = nl;
    }
    return perm(((uint64_t)r << 32) | l, des_fp, 64, 64);
}

void p25_des_ecb(const uint8_t key[8], const uint8_t in[8], uint8_t out[8])
{
    uint64_t sk[16], v = 0;
    des_subkeys(key, sk);
    for (int i = 0; i < 8; i++) v = (v << 8) | in[i];
    v = des_block(sk, v);
    for (int i = 7; i >= 0; i--) { out[i] = (uint8_t)v; v >>= 8; }
    memset(sk, 0, sizeof sk);
}

/* ------------------------------------------------------------ keystream */
bool p25_crypt_prepare(p25_crypt_t *c, const p25_keyring_t *r, uint8_t algid, uint16_t kid, const uint8_t mi[9],
                       bool phase2)
{
    c->ready = false;
    c->algid = algid;
    c->kid = kid;
    c->phase2 = phase2;
    const p25_key_t *k = r ? p25_keyring_find(r, algid, kid) : NULL;
    if (!k) return false;
    memset(c->ks, 0, sizeof c->ks);
    switch (algid) {
    case P25_ALG_AES256: {
        uint8_t rk[240], blk[16];
        aes256_expand(k->key, rk);
        p25_mi_to_iv128(mi, blk);
        for (unsigned b = 0; b < 15; b++) {                 /* OFB, 240 bytes */
            aes256_block(rk, blk, blk);
            memcpy(c->ks + 16 * b, blk, 16);
        }
        memset(rk, 0, sizeof rk);
        break;
    }
    case P25_ALG_DES_OFB: {
        uint64_t sk[16], v = 0;
        des_subkeys(k->key, sk);
        for (int i = 0; i < 8; i++) v = (v << 8) | mi[i];
        for (unsigned b = 0; b < 28; b++) {                 /* OFB, 224 bytes */
            v = des_block(sk, v);
            for (int i = 0; i < 8; i++) c->ks[8 * b + i] = (uint8_t)(v >> (56 - 8 * i));
        }
        memset(sk, 0, sizeof sk);
        break;
    }
    case P25_ALG_ADP: {                                     /* ARC4, key = key(5) || MI(8) */
        uint8_t S[256], K[13];
        memcpy(K, k->key, 5);
        memcpy(K + 5, mi, 8);
        for (unsigned i = 0; i < 256; i++) S[i] = (uint8_t)i;
        unsigned j = 0;
        for (unsigned i = 0; i < 256; i++) {
            j = (j + S[i] + K[i % 13]) & 0xFFu;
            uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
        }
        unsigned i = 0;
        j = 0;
        for (unsigned n = 0; n < P25_KS_MAX; n++) {
            i = (i + 1) & 0xFFu;
            j = (j + S[i]) & 0xFFu;
            uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
            c->ks[n] = S[(S[i] + S[j]) & 0xFFu];
        }
        memset(S, 0, sizeof S);
        memset(K, 0, sizeof K);
        break;
    }
    default:
        return false;
    }
    c->ready = true;
    return true;
}

static unsigned discard(uint8_t algid)
{
    return algid == P25_ALG_AES256 ? 16u : algid == P25_ALG_DES_OFB ? 8u : 0u;
}

void p25_crypt_imbe(const p25_crypt_t *c, bool ldu2, unsigned sub, uint8_t imbe[11])
{
    if (!c->ready || sub > 8) return;
    unsigned off = (ldu2 ? 101u : 0u) + 11u * sub + (sub < 8 ? 0u : 2u);
    off += c->algid == P25_ALG_ADP ? 267u : discard(c->algid) + 11u;   /* skip LC/ES + reserved */
    for (unsigned i = 0; i < 11; i++) imbe[i] ^= c->ks[off + i];
}

void p25_crypt_ambe(const p25_crypt_t *c, unsigned burst, unsigned sub, uint8_t ambe[7])
{
    if (!c->ready || burst > 4) return;
    unsigned off = (c->algid == P25_ALG_ADP ? 256u : discard(c->algid)) + 7u * (sub + 4u * burst);
    for (unsigned i = 0; i < 7; i++) ambe[i] ^= c->ks[off + i];
    ambe[6] &= 0x80;
}
