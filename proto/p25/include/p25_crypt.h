/*
 * p25_crypt.h - P25 voice encryption/decryption with the user's own keys:
 * TIA-102.AAAD-B (block encryption protocol: DES-OFB, AES-256 OFB),
 * TIA-102.BBAC/BBAD (Phase 2 use), ADP (ARC4, Motorola, ALGID 0xAA).
 *
 * All algorithms are OFB/stream ciphers: one keystream per superframe,
 * derived from key + message indicator (MI), XORed onto the voice frames, so
 * encrypt == decrypt. Keys come from the user's key table (key ID KID +
 * ALGID); nothing here recovers or guesses keys.
 *
 * Legal note: encrypting transmissions to obscure their meaning is not
 * permitted on amateur frequencies in most countries (e.g. US 47 CFR
 * 97.113(a)(4)). TX encryption is for systems where the user is authorised.
 *
 * Keystream layout per superframe (bytes):
 *   Phase 1: [discard: DES 8 / AES 16 / ADP 0] then per LDU 101 bytes:
 *            LC/ES 9 + reserved 2, IMBE 1..8 (11 each), LSD 2, IMBE 9.
 *            ADP starts its LDU1 voice at byte 267 (after a 256-byte skip).
 *   Phase 2: [discard] then 7 bytes per AMBE frame: 4V bursts 0..3 (4 frames
 *            each), then the 2V burst (2 frames).
 * AES IV = MI(64) || LFSR^64(MI); DES IV = MI(64); ADP RC4 key = key(5)||MI(8).
 * MI LFSR: C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1; the next
 * superframe's MI is the register after 64 steps.
 */
#ifndef P25_CRYPT_H
#define P25_CRYPT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum p25_algid {
    P25_ALG_CLEAR = 0x80,
    P25_ALG_DES_OFB = 0x81,
    P25_ALG_AES256 = 0x84,
    P25_ALG_ADP = 0xAA,
};

#define P25_CRYPT_MAX_KEYS 16
#define P25_KS_MAX 469          /* ADP needs the longest keystream */

typedef struct {
    uint8_t  algid;
    uint16_t kid;
    uint8_t  len;               /* DES 8, AES 32, ADP 5 */
    uint8_t  key[32];
} p25_key_t;

typedef struct {
    p25_key_t k[P25_CRYPT_MAX_KEYS];
    uint8_t   n;
} p25_keyring_t;

void  p25_keyring_init(p25_keyring_t *r);
/* key shorter than the algorithm's size is left-padded with zeros */
bool  p25_keyring_add(p25_keyring_t *r, uint8_t algid, uint16_t kid, const uint8_t *key, size_t len);
const p25_key_t *p25_keyring_find(const p25_keyring_t *r, uint8_t algid, uint16_t kid);
void  p25_keyring_wipe(p25_keyring_t *r);

/* one superframe of keystream */
typedef struct {
    uint8_t  ks[P25_KS_MAX];
    uint8_t  algid;
    uint16_t kid;
    bool     ready;
    bool     phase2;
} p25_crypt_t;

bool p25_crypt_supported(uint8_t algid);
/* Build the keystream for one superframe. Returns false if the algorithm is
 * not supported or the key is missing (c->ready = false). */
bool p25_crypt_prepare(p25_crypt_t *c, const p25_keyring_t *r, uint8_t algid, uint16_t kid,
                       const uint8_t mi[9], bool phase2);
/* Phase 1: XOR the keystream onto IMBE frame `sub` (0..8) of LDU1/LDU2. */
void p25_crypt_imbe(const p25_crypt_t *c, bool ldu2, unsigned sub, uint8_t imbe[11]);
/* Phase 2: burst 0..3 = 4V bursts of the superframe, 4 = the 2V burst;
 * sub = frame inside the burst. Keeps the 49-bit frame shape. */
void p25_crypt_ambe(const p25_crypt_t *c, unsigned burst, unsigned sub, uint8_t ambe[7]);

/* MI handling */
void p25_mi_next(uint8_t mi[9]);                       /* MI of the next superframe */
void p25_mi_to_iv128(const uint8_t mi[9], uint8_t iv[16]);

/* raw block ciphers (exposed for tests) */
void p25_aes256_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
void p25_des_ecb(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

#ifdef __cplusplus
}
#endif
#endif
