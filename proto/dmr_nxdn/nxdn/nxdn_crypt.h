/* NXDN security (NXDN TS 1-D v1.3): scramble (PN X^15+X+1), DES-OFB and AES-256-OFB encryption
 * of voice and user data, 64-bit IV LFSR (P(x) = 1+X^15+X^27+X^38+X^46+X^62+X^64).
 *
 * Voice: TS 1-D 5.2 puts the cipher between the vocoder and its FEC, i.e. on the 49-bit (EHR) /
 * 88-bit (EFR) voice coding data. For EHR the receiver strips / re-applies the AMBE+2 frame FEC
 * (common/ambe_fec.c) and hands plain 72-bit VCH frames to the vocoder; TX uses
 * nxdn_vcrypt_apply72(). EFR: same with the 88 <-> 144 bit full rate FEC, nxdn_vcrypt_apply144().
 * TX call sequencing (cipher field, VCALL_IV, sessions): nxdn_vtx_*.
 * User data (DCALL User Data octets 2..21) is encrypted/decrypted inside the library.
 *
 * Keys are the user's own; nothing here recovers or guesses keys.
 */
#ifndef NXDN_CRYPT_H
#define NXDN_CRYPT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../common/cipher.h"

enum { NXDN_CIPHER_NONE = 0, NXDN_CIPHER_SCRAMBLE = 1, NXDN_CIPHER_DES = 2, NXDN_CIPHER_AES = 3 };

/* ---------------- keys ---------------- */
typedef struct {
    uint8_t cipher;           /* NXDN_CIPHER_* */
    uint8_t key_id;           /* 0..63 */
    uint8_t len;              /* 2 (scramble, 15 bits right aligned), 8 (DES), 32 (AES-256) */
    uint8_t key[32];
} nxdn_key_t;

#define NXDN_MAX_KEYS 8
typedef struct { nxdn_key_t k[NXDN_MAX_KEYS]; uint8_t n; } nxdn_keyring_t;
const nxdn_key_t *nxdn_keyring_find(const nxdn_keyring_t *kr, uint8_t cipher, uint8_t key_id);

/* ---------------- IV generator (5.3.2.2 / 5.3.3.2) ----------------
 * state = b63..b0. Returns the 64-bit output for this session (the bits shifted out = the current
 * state) and advances the register by 64 shifts (the new state is the next seed sent over the air). */
uint64_t nxdn_iv_next(uint64_t *state);

/* ---------------- keystream ---------------- */
typedef struct {
    uint8_t cipher;
    uint16_t key15, pn;       /* scramble */
    des_ctx_t des;
    aes_ctx_t aes;
    uint8_t blk[16];          /* current OFB output block */
    uint8_t blen, bpos;       /* block length in bytes, bits used */
} nxdn_ks_t;

int  nxdn_ks_init(nxdn_ks_t *ks, const nxdn_key_t *key);          /* -1 bad key */
/* start a session; iv_seed = the 64-bit IV element sent in VCALL_IV / DCALL header (ignored: scramble) */
void nxdn_ks_start(nxdn_ks_t *ks, uint64_t iv_seed);
uint8_t nxdn_ks_bit(nxdn_ks_t *ks);
void nxdn_ks_xor_bits(nxdn_ks_t *ks, uint8_t *buf, unsigned nbits);   /* MSB-first buffer, in place */
void nxdn_ks_skip(nxdn_ks_t *ks, unsigned nbits);
void nxdn_ks_wipe(nxdn_ks_t *ks);

/* ---------------- voice sessions (5.4.4) ---------------- */
#define NXDN_VPOS_UNKNOWN 0xFFu        /* voice position: (sacch quarter << 2) | vch index */

typedef struct {
    nxdn_ks_t ks;
    bool  keyed;              /* cipher + key known */
    bool  efr;
    uint8_t cipher, key_id;
    bool  active;             /* keystream for the current superframe is valid */
    bool  iv_pending;
    uint64_t iv_next, iv_cur;
    uint8_t sf;               /* superframe inside the DES/AES session (0, 1) */
    uint8_t ksbuf[196];       /* keystream of the whole session: 8 frames x 196 bits (EHR) */
} nxdn_vcrypt_t;

/* select algorithm + key (from VCALL's Cipher Type / Key ID); key NULL = no key -> stays inactive */
void nxdn_vcrypt_setup(nxdn_vcrypt_t *v, uint8_t cipher, uint8_t key_id, const nxdn_key_t *key, bool efr);
void nxdn_vcrypt_iv(nxdn_vcrypt_t *v, uint64_t iv_seed);            /* VCALL_IV received / sent */
void nxdn_vcrypt_frame(nxdn_vcrypt_t *v, uint8_t quarter);          /* every voice frame, quarter 0..3 */
/* XOR the 49-bit (EHR) / 88-bit (EFR) voice coding data of VCH `pos`; returns false if no keystream */
bool nxdn_vcrypt_apply(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t *frame);
/* EHR: same on a 72-bit VCH (FEC decode, xor, FEC encode); channel errors in C0/C1 are corrected */
bool nxdn_vcrypt_apply72(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t vch72[9]);
/* EFR: same on a 144-bit VCH (full rate FEC decode, xor the 88 bits, re-encode) */
bool nxdn_vcrypt_apply144(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t vch144[18]);
void nxdn_vcrypt_reset(nxdn_vcrypt_t *v);

/* ---------------- encrypted voice TX (5.4.4, 6.4.1.2) ----------------
 * Sequencing of one encrypted (or clear) conventional voice call on the TX side:
 *  - VCALL carries Cipher Type / Key ID (octet 7);
 *  - DES / AES: the header frame's FACCH1 second half is VCALL_IV with IV(1); a session of 2 superframes
 *    uses one IV; superframe 2n+1 sends the next IV in its SACCH (VCALL_IV), which applies from
 *    superframe 2n+2 on. Even superframes carry VCALL in their SACCH. Scramble: no IV, VCALL every superframe.
 *  - the cipher is applied to the 49 (EHR) / 88 (EFR) bit voice data under the VCH FEC.
 * iv_state: the 64-bit IV generator state (non zero); its first output is IV(1). */
typedef struct {
    nxdn_vcrypt_t vc;
    uint64_t lfsr;            /* IV generator */
    uint64_t seed;            /* IV of the session the last VCALL_IV announced */
    uint8_t vcall[10], ivm[10];
    uint32_t frames;          /* voice frames sent (quarter = frames % 4) */
    bool enc, iv;             /* encrypting; DES/AES (IV sessions) */
} nxdn_vtx_t;

/* key NULL / NXDN_CIPHER_NONE / bad key = clear call. vcall: VCALL message (10 octets) of the call, its
 * cipher field is set here and a copy kept. Returns true when the call is encrypted. */
bool nxdn_vtx_start(nxdn_vtx_t *t, const nxdn_key_t *key, bool efr, uint64_t iv_state, uint8_t vcall[10]);
/* FACCH1 halves of the header frame: first = VCALL, second = VCALL_IV (DES/AES) or VCALL */
const uint8_t *nxdn_vtx_header2(const nxdn_vtx_t *t);
/* next voice frame: vch = 4 x 72 (EHR) or 2 x 144 (EFR) bit VCH, encrypted in place; *quarter = SACCH
 * quarter 0..3 of this frame; returns the SACCH message (9 octets used) of this superframe. */
const uint8_t *nxdn_vtx_frame(nxdn_vtx_t *t, uint8_t *vch, uint8_t *quarter);
void nxdn_vtx_wipe(nxdn_vtx_t *t);

/* ---------------- data (5.4.5) ---------------- */
/* one packet = one session: start with the IV from the DCALL header, then xor each DCALL(User Data)
 * octets 2..21 in order (20 octets per UDCH) */
void nxdn_dcrypt_start(nxdn_ks_t *ks, uint64_t iv_seed);
void nxdn_dcrypt_block(nxdn_ks_t *ks, uint8_t *data, size_t len);

/* VCALL_IV message (6.4.1.2): type 0x03, IV in octets 1..8 */
unsigned nxdn_msg_build_vcall_iv(uint8_t *m, uint64_t iv);
uint64_t nxdn_msg_iv(const uint8_t *m);           /* octets 1..8 */
/* Cipher Type / Key ID of a VCALL (octet 7) or DCALL header (octet 7) */
void nxdn_msg_set_cipher(uint8_t *m, uint8_t cipher, uint8_t key_id);

#endif
