/* Block / stream cipher primitives for the privacy layers (encrypt direction only: every mode used
 * here is OFB or a keystream). Own implementations from FIPS-197 (AES), FIPS 46-3 (DES) and the
 * RC4 description in ETSI TS 102 361-4 6.4.8. No tables in RAM, no dynamic memory.
 * Not hardened against side channels (no constant-time S-box); fine for a handheld radio.
 */
#ifndef PROTO_CIPHER_H
#define PROTO_CIPHER_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t rk[240];        /* expanded key, up to AES-256 (15 round keys) */
    uint8_t rounds;
} aes_ctx_t;

/* key_len 16, 24 or 32 bytes; returns 0, -1 on a bad length */
int  aes_init(aes_ctx_t *c, const uint8_t *key, size_t key_len);
void aes_encrypt(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]);

typedef struct { uint32_t sk[32]; } des_ctx_t;   /* 16 x 48-bit subkeys as two 24-bit halves */
void des_init(des_ctx_t *c, const uint8_t key[8]);
void des_encrypt(const des_ctx_t *c, const uint8_t in[8], uint8_t out[8]);

typedef struct { uint8_t s[256]; uint8_t i, j; } rc4_ctx_t;
void    rc4_init(rc4_ctx_t *c, const uint8_t *key, size_t len);
uint8_t rc4_byte(rc4_ctx_t *c);

/* wipe key material */
void cipher_wipe(void *p, size_t n);

#endif
