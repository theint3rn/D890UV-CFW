/* DMR privacy and Tier III authentication.
 *
 * ETSI TS 102 361-1..-4 do not define any privacy algorithm ("Privacy is not defined in the present
 * document"); the DMR Association / vendor schemes (Basic/Enhanced Privacy, DMRA ARC4/AES) are not
 * public. So two things live here:
 *
 * 1. Tier III authentication (TS 102 361-4 6.4.8): RC4 keyed with challenge || K, keystream bytes
 *    256..258, optional PSN xor. Standard, interoperable, spec test vectors in the unit test.
 *
 * 2. "cfw privacy": an own, documented scheme for voice and packet data on every tier (direct mode,
 *    Tier II repeaters, Tier III payload channels). It only interoperates with other radios running
 *    this firmware. AES-256 in counter mode; the keystream is xored onto the 72-bit vocoder frames as
 *    sent (no AMBE knowledge needed; bit errors stay single bit errors).
 *      nonce block = IV(4) | superframe(2) | counter(2) | 'V' or 'D' | key id | 6 x 00
 *    Signalling (both carry alg, key id, IV, superframe number):
 *      - PI header burst (data type 0, CRC mask 0x6969) before the voice LC header / data header
 *        octets: C0|alg, key_id, IV(4), sf(2), 'C', 'F'
 *      - late entry: on odd superframes the embedded LC (bursts B..E) carries a privacy LC instead of the
 *        voice LC: FLCO DMR_PRIV_LC_FLCO, FID DMR_PRIV_LC_FID, alg<<4|key_id, IV(4), sf(2).
 *    The IV is 32 random bits per transmission (platform RNG); superframe numbers make each keystream
 *    block unique inside a transmission (max 65535 superframes = 6.5 h).
 *
 * Hooks for vendor schemes: dmr_priv_t has a keystream callback slot so DMRA/vendor algorithms can be
 * added when their specifications are available, without changing the receiver.
 */
#ifndef DMR_PRIV_H
#define DMR_PRIV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../common/cipher.h"

#ifndef DMR_PRIV_LC_FLCO
#define DMR_PRIV_LC_FLCO 0x3Eu      /* not assigned by ETSI for FID 0; configurable */
#endif
#ifndef DMR_PRIV_LC_FID
#define DMR_PRIV_LC_FID  0x00u
#endif

/* cfw privacy algorithms (alg field of the PI header / privacy LC). All use the same signalling and the same
 * keystream addressing (IV, superframe, byte offset, 'V'/'D', key id); they differ in the key size and the generator:
 *   CFW_AES256  AES-256-CTR, 32-byte key (cfw_cps key kind 3)
 *   CFW_AES128  AES-128-CTR, 16-byte key (kind 2; the stock CPS "AES Encryption Code" with a 32-hex-digit key)
 *   CFW_RC4     RC4-drop256 per superframe, key = K || IV || sf || kind || key id, K 5 bytes (40 bit, kind 1;
 *               the stock CPS "ARC4 Encryption Code"); max 16 key bytes
 *   CFW_BP      16-bit key (kind 0; the stock CPS "Encryption Code" / basic privacy), AES-128-CTR with the key
 *               16 | 'cfw-basic-priv' padding: as weak as any 16-bit scheme (65536 keys), only privacy against
 *               casual listeners.
 * None of these is the stock radio's (or the DMRA's / Motorola's / Hytera's) algorithm: the stock D890UV / D168UV
 * do their encryption inside the SCT3288 baseband (encrypted firmware image), so the stock schemes are not known
 * here and cfw only interoperates with cfw radios. */
enum { DMR_PRIV_NONE = 0, DMR_PRIV_CFW_AES256 = 1, DMR_PRIV_CFW_AES128 = 2, DMR_PRIV_CFW_RC4 = 3, DMR_PRIV_CFW_BP = 4 };

/* len: key bytes used (0 = the algorithm's default: 32 / 16 / 5 / 2) */
typedef struct { uint8_t alg, key_id; uint8_t key[32]; uint8_t len; } dmr_key_t;
#define DMR_MAX_KEYS 8
typedef struct { dmr_key_t k[DMR_MAX_KEYS]; uint8_t n; } dmr_keyring_t;
const dmr_key_t *dmr_keyring_find(const dmr_keyring_t *kr, uint8_t alg, uint8_t key_id);
/* late entry: the privacy LC carries only the low 4 bits of the key id */
const dmr_key_t *dmr_keyring_find_lc(const dmr_keyring_t *kr, uint8_t alg, uint8_t key_id4);
int dmr_priv_key_len(uint8_t alg);            /* default key length of an algorithm, -1 unknown */

typedef struct dmr_priv dmr_priv_t;
/* keystream provider: fill n bytes of keystream starting at byte offset `off` of superframe `sf` */
typedef void (*dmr_priv_ks_fn)(dmr_priv_t *p, uint8_t kind, uint16_t sf, uint32_t off, uint8_t *out, size_t n);

struct dmr_priv {
    uint8_t  alg, key_id;
    bool     keyed;               /* key present */
    bool     synced;              /* IV + superframe known */
    uint32_t iv;
    uint16_t sf;                  /* current voice superframe number */
    aes_ctx_t aes;
    uint8_t  rk[16], rklen;       /* RC4 key bytes */
    dmr_priv_ks_fn ks;            /* by alg (see above) */
};

/* information carried by the PI header / privacy LC */
typedef struct { uint8_t alg, key_id; uint32_t iv; uint16_t sf; } dmr_priv_info_t;

int  dmr_priv_setup(dmr_priv_t *p, const dmr_key_t *key);       /* -1: unknown alg / bad length / no key */
void dmr_priv_wipe(dmr_priv_t *p);
void dmr_priv_sync(dmr_priv_t *p, uint32_t iv, uint16_t sf);
/* xor one 72-bit vocoder frame: burst 0..5 (A..F) and frame 0..2 inside the burst, superframe p->sf */
void dmr_priv_voice(dmr_priv_t *p, uint8_t burst, uint8_t frame, uint8_t vf[9]);
/* xor packet data (user data octets, from offset 0 of the packet) */
void dmr_priv_data(dmr_priv_t *p, uint32_t off, uint8_t *buf, size_t n);

/* PI header / privacy LC coding; parse returns -1 if it is not a cfw privacy PDU */
void dmr_priv_pi_build(const dmr_priv_info_t *i, uint8_t pi10[10]);
int  dmr_priv_pi_parse(const uint8_t pi10[10], dmr_priv_info_t *i);
void dmr_priv_lc_build(const dmr_priv_info_t *i, uint8_t lc9[9]);
int  dmr_priv_lc_parse(const uint8_t lc9[9], dmr_priv_info_t *i);

/* ---------------- Tier III authentication (TS 102 361-4 6.4.8) ---------------- */
/* 24-bit response to `challenge` for the 128-bit key K; psn may be NULL (no PSN) */
uint32_t dmr_t3_auth_response(const uint8_t k[16], uint32_t challenge, const uint8_t *psn3);

#endif
