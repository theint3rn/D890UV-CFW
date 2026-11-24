/* AES (FIPS-197), DES (FIPS 46-3), RC4. Encrypt direction only. */
#include "cipher.h"

void cipher_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* ---------------- AES ---------------- */
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1u) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

/* FIPS-197 S-box (the unit test recomputes it from the inverse + affine definition) */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

int aes_init(aes_ctx_t *c, const uint8_t *key, size_t key_len)
{
    if (key_len != 16 && key_len != 24 && key_len != 32) return -1;
    unsigned nk = (unsigned)key_len / 4u;
    c->rounds = (uint8_t)(nk + 6u);
    unsigned total = 4u * (c->rounds + 1u);
    for (unsigned i = 0; i < key_len; i++) c->rk[i] = key[i];
    uint8_t rcon = 1;
    for (unsigned i = nk; i < total; i++) {
        uint8_t t[4];
        for (int k = 0; k < 4; k++) t[k] = c->rk[4 * (i - 1) + (unsigned)k];
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon); t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = SBOX[t[k]];
        }
        for (int k = 0; k < 4; k++) c->rk[4 * i + (unsigned)k] = (uint8_t)(c->rk[4 * (i - nk) + (unsigned)k] ^ t[k]);
    }
    return 0;
}

void aes_encrypt(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ c->rk[i]);
    for (unsigned r = 1; r <= c->rounds; r++) {
        /* SubBytes + ShiftRows (state is column-major: s[4*col + row]) */
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) t[4 * col + row] = SBOX[s[4 * ((col + row) & 3) + row]];
        if (r != c->rounds) {      /* MixColumns */
            for (int col = 0; col < 4; col++) {
                uint8_t *a = &t[4 * col], a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                a[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                a[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                a[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ c->rk[16 * r + (unsigned)i]);
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* ---------------- DES ---------------- */
static const uint8_t IP[64] = { 58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
                                57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7 };
static const uint8_t FP[64] = { 40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
                                36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25 };
static const uint8_t EXP[48] = { 32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
                                 16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1 };
static const uint8_t PERM[32] = { 16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25 };
static const uint8_t PC1[56] = { 57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
                                 63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4 };
static const uint8_t PC2[48] = { 14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
                                 41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32 };
static const uint8_t SHIFTS[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };
static const uint8_t SB[8][64] = {
    { 14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7, 0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
      4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13 },
    { 15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10, 3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
      0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9 },
    { 10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8, 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
      13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7, 1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12 },
    { 7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
      10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4, 3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14 },
    { 2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
      4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3 },
    { 12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11, 10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
      9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6, 4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13 },
    { 4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
      1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2, 6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12 },
    { 13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7, 1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
      7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8, 2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11 },
};

/* bit n (1 = MSB) of a w-bit value */
static uint64_t permute(uint64_t in, unsigned win, const uint8_t *tab, unsigned wout)
{
    uint64_t o = 0;
    for (unsigned i = 0; i < wout; i++) o = (o << 1) | ((in >> (win - tab[i])) & 1u);
    return o;
}

void des_init(des_ctx_t *c, const uint8_t key[8])
{
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | key[i];
    uint64_t cd = permute(k, 64, PC1, 56);
    uint32_t C = (uint32_t)(cd >> 28) & 0xFFFFFFFu, D = (uint32_t)cd & 0xFFFFFFFu;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < SHIFTS[r]; s++) {
            C = ((C << 1) | (C >> 27)) & 0xFFFFFFFu;
            D = ((D << 1) | (D >> 27)) & 0xFFFFFFFu;
        }
        uint64_t sk = permute(((uint64_t)C << 28) | D, 56, PC2, 48);
        c->sk[2 * r] = (uint32_t)(sk >> 24);
        c->sk[2 * r + 1] = (uint32_t)sk & 0xFFFFFFu;
    }
}

static uint32_t des_f(uint32_t r, uint64_t sk)
{
    uint64_t e = permute(r, 32, EXP, 48) ^ sk;
    uint32_t o = 0;
    for (int s = 0; s < 8; s++) {
        unsigned six = (unsigned)(e >> (42 - 6 * s)) & 0x3Fu;
        unsigned row = ((six >> 4) & 2u) | (six & 1u), col = (six >> 1) & 0xFu;
        o = (o << 4) | SB[s][row * 16 + col];
    }
    return (uint32_t)permute(o, 32, PERM, 32);
}

void des_encrypt(const des_ctx_t *c, const uint8_t in[8], uint8_t out[8])
{
    uint64_t b = 0;
    for (int i = 0; i < 8; i++) b = (b << 8) | in[i];
    b = permute(b, 64, IP, 64);
    uint32_t L = (uint32_t)(b >> 32), R = (uint32_t)b;
    for (int r = 0; r < 16; r++) {
        uint64_t sk = ((uint64_t)c->sk[2 * r] << 24) | c->sk[2 * r + 1];
        uint32_t t = R;
        R = L ^ des_f(R, sk);
        L = t;
    }
    b = permute(((uint64_t)R << 32) | L, 64, FP, 64);
    for (int i = 7; i >= 0; i--) { out[i] = (uint8_t)b; b >>= 8; }
}

/* ---------------- RC4 ---------------- */
void rc4_init(rc4_ctx_t *c, const uint8_t *key, size_t len)
{
    for (int i = 0; i < 256; i++) c->s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + c->s[i] + key[(size_t)i % len]);
        uint8_t t = c->s[i]; c->s[i] = c->s[j]; c->s[j] = t;
    }
    c->i = c->j = 0;
}

uint8_t rc4_byte(rc4_ctx_t *c)
{
    c->i = (uint8_t)(c->i + 1);
    c->j = (uint8_t)(c->j + c->s[c->i]);
    uint8_t t = c->s[c->i]; c->s[c->i] = c->s[c->j]; c->s[c->j] = t;
    return c->s[(uint8_t)(c->s[c->i] + c->s[c->j])];
}
