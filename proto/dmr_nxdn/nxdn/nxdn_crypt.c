/* NXDN TS 1-D encryption: scramble / DES-OFB / AES-OFB, IV LFSR, voice and data sessions. */
#include "nxdn_crypt.h"
#include "../common/bits.h"
#include "../common/ambe_fec.h"
#include <string.h>

const nxdn_key_t *nxdn_keyring_find(const nxdn_keyring_t *kr, uint8_t cipher, uint8_t key_id)
{
    if (!kr) return NULL;
    for (unsigned i = 0; i < kr->n && i < NXDN_MAX_KEYS; i++)
        if (kr->k[i].cipher == cipher && kr->k[i].key_id == key_id) return &kr->k[i];
    return NULL;
}

/* figure 5.3-3: shift left, output b63, feedback from b63 b61 b45 b37 b26 b14 into b0
 * (orientation fixed by the IV test vectors of 7.2.3) */
uint64_t nxdn_iv_next(uint64_t *state)
{
    uint64_t s = *state, out = s;
    for (int i = 0; i < 64; i++) {
        uint64_t fb = ((s >> 63) ^ (s >> 61) ^ (s >> 45) ^ (s >> 37) ^ (s >> 26) ^ (s >> 14)) & 1u;
        s = (s << 1) | fb;
    }
    *state = s;
    return out;
}

int nxdn_ks_init(nxdn_ks_t *ks, const nxdn_key_t *key)
{
    memset(ks, 0, sizeof *ks);
    if (!key) return -1;
    ks->cipher = key->cipher;
    switch (key->cipher) {
    case NXDN_CIPHER_SCRAMBLE:
        ks->key15 = (uint16_t)(((key->key[0] << 8) | key->key[1]) & 0x7FFFu);
        if (!ks->key15) return -1;
        return 0;
    case NXDN_CIPHER_DES:
        if (key->len != 8) return -1;
        des_init(&ks->des, key->key);
        ks->blen = 8;
        return 0;
    case NXDN_CIPHER_AES:
        if (key->len != 32) return -1;
        ks->blen = 16;
        return aes_init(&ks->aes, key->key, 32);
    default:
        return -1;
    }
}

static void ks_block(nxdn_ks_t *ks)
{
    uint8_t o[16];
    if (ks->cipher == NXDN_CIPHER_DES) des_encrypt(&ks->des, ks->blk, o);
    else aes_encrypt(&ks->aes, ks->blk, o);
    memcpy(ks->blk, o, ks->blen);
    ks->bpos = 0;
}

void nxdn_ks_start(nxdn_ks_t *ks, uint64_t iv_seed)
{
    if (ks->cipher == NXDN_CIPHER_SCRAMBLE) { ks->pn = ks->key15; return; }
    uint64_t st = iv_seed;
    uint64_t a = nxdn_iv_next(&st);              /* first IV generator output = the seed */
    for (int i = 0; i < 8; i++) ks->blk[i] = (uint8_t)(a >> (56 - 8 * i));
    if (ks->blen == 16)                          /* 128-bit IV: buffer a63..a0 then LFSR b63..b0 */
        for (int i = 0; i < 8; i++) ks->blk[8 + i] = (uint8_t)(st >> (56 - 8 * i));
    ks_block(ks);                                /* e-iv/e-v: the first output only re-initialises */
    ks_block(ks);                                /* e-vi: second round output is the first keystream */
}

uint8_t nxdn_ks_bit(nxdn_ks_t *ks)
{
    if (ks->cipher == NXDN_CIPHER_SCRAMBLE) {
        /* figure 5.3-1, P(x) = X^15 + X + 1: output S0, feedback S0 ^ S1 into S14 */
        uint8_t out = (uint8_t)(ks->pn & 1u);
        uint16_t fb = (uint16_t)((ks->pn ^ (ks->pn >> 1)) & 1u);
        ks->pn = (uint16_t)((ks->pn >> 1) | (fb << 14));
        return out;
    }
    if (ks->bpos >= ks->blen * 8u) ks_block(ks);
    uint8_t b = (uint8_t)((ks->blk[ks->bpos >> 3] >> (7u - (ks->bpos & 7u))) & 1u);
    ks->bpos++;
    return b;
}

void nxdn_ks_xor_bits(nxdn_ks_t *ks, uint8_t *buf, unsigned nbits)
{
    for (unsigned i = 0; i < nbits; i++) if (nxdn_ks_bit(ks)) buf[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u));
}

void nxdn_ks_skip(nxdn_ks_t *ks, unsigned nbits)
{
    while (nbits--) (void)nxdn_ks_bit(ks);
}

void nxdn_ks_wipe(nxdn_ks_t *ks) { cipher_wipe(ks, sizeof *ks); }

/* ---------------- voice ---------------- */
static unsigned vbits(const nxdn_vcrypt_t *v) { return v->efr ? 88u : 49u; }
static unsigned vper(const nxdn_vcrypt_t *v) { return v->efr ? 2u : 4u; }

void nxdn_vcrypt_reset(nxdn_vcrypt_t *v)
{
    v->active = false;
    v->iv_pending = false;
    v->sf = 0;
}

void nxdn_vcrypt_setup(nxdn_vcrypt_t *v, uint8_t cipher, uint8_t key_id, const nxdn_key_t *key, bool efr)
{
    nxdn_ks_wipe(&v->ks);
    memset(v, 0, sizeof *v);
    v->cipher = cipher;
    v->key_id = key_id;
    v->efr = efr;
    v->keyed = cipher != NXDN_CIPHER_NONE && key && key->cipher == cipher && nxdn_ks_init(&v->ks, key) == 0;
}

void nxdn_vcrypt_iv(nxdn_vcrypt_t *v, uint64_t iv_seed)
{
    v->iv_next = iv_seed;
    v->iv_pending = true;
}

static void fill_session(nxdn_vcrypt_t *v, unsigned superframes)
{
    unsigned n = superframes * 4u * vper(v) * vbits(v);
    memset(v->ksbuf, 0, sizeof v->ksbuf);
    for (unsigned i = 0; i < n && i < sizeof v->ksbuf * 8u; i++)
        if (nxdn_ks_bit(&v->ks)) v->ksbuf[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
}

void nxdn_vcrypt_frame(nxdn_vcrypt_t *v, uint8_t quarter)
{
    if (!v->keyed || quarter != 0) return;
    if (v->cipher == NXDN_CIPHER_SCRAMBLE) {
        /* 5.4.4.1: one session = one superframe, PN restarted from the key */
        nxdn_ks_start(&v->ks, 0);
        fill_session(v, 1);
        v->sf = 0;
        v->active = true;
        return;
    }
    if (v->iv_pending) {            /* 5.4.4.2 (4): the IV applies from the next superframe on, for 2 superframes */
        v->iv_pending = false;
        v->iv_cur = v->iv_next;
        nxdn_ks_start(&v->ks, v->iv_cur);
        fill_session(v, 2);
        v->sf = 0;
        v->active = true;
    } else if (v->active && ++v->sf > 1) {
        v->active = false;          /* session over and no new IV received */
    }
}

bool nxdn_vcrypt_apply(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t *frame)
{
    if (!v->keyed || !v->active || pos == NXDN_VPOS_UNKNOWN) return false;
    unsigned q = pos >> 2, vch = pos & 3u;
    if (vch >= vper(v)) return false;
    unsigned off = ((v->sf * 4u + q) * vper(v) + vch) * vbits(v);
    for (unsigned i = 0; i < vbits(v); i++) {
        unsigned k = off + i;
        if ((v->ksbuf[k >> 3] >> (7u - (k & 7u))) & 1u) frame[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u));
    }
    return true;
}

bool nxdn_vcrypt_apply72(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t vch72[9])
{
    uint8_t d[7];
    if (v->efr || !v->keyed || !v->active || pos == NXDN_VPOS_UNKNOWN) return false;
    (void)ambe72_decode(vch72, d);
    if (!nxdn_vcrypt_apply(v, pos, d)) return false;
    ambe72_encode(d, vch72);
    return true;
}

bool nxdn_vcrypt_apply144(const nxdn_vcrypt_t *v, uint8_t pos, uint8_t vch144[18])
{
    uint8_t d[11];
    if (!v->efr || !v->keyed || !v->active || pos == NXDN_VPOS_UNKNOWN) return false;
    (void)ambe144_decode(vch144, d);
    if (!nxdn_vcrypt_apply(v, pos, d)) return false;
    ambe144_encode(d, vch144);
    return true;
}

/* ---------------- encrypted voice TX ---------------- */
bool nxdn_vtx_start(nxdn_vtx_t *t, const nxdn_key_t *key, bool efr, uint64_t iv_state, uint8_t vcall[10])
{
    nxdn_vtx_wipe(t);
    uint8_t cipher = key ? key->cipher : NXDN_CIPHER_NONE;
    nxdn_vcrypt_setup(&t->vc, cipher, key ? key->key_id : 0, key, efr);
    t->enc = t->vc.keyed;
    if (!t->enc) cipher = NXDN_CIPHER_NONE;
    nxdn_msg_set_cipher(vcall, cipher, t->enc ? key->key_id : 0);
    memcpy(t->vcall, vcall, 10);
    t->iv = t->enc && cipher != NXDN_CIPHER_SCRAMBLE;
    if (t->iv) {
        t->lfsr = iv_state ? iv_state : 1u;
        t->seed = nxdn_iv_next(&t->lfsr);
        nxdn_msg_build_vcall_iv(t->ivm, t->seed);
        nxdn_vcrypt_iv(&t->vc, t->seed);          /* used from the first voice superframe on */
    }
    return t->enc;
}

const uint8_t *nxdn_vtx_header2(const nxdn_vtx_t *t) { return t->iv ? t->ivm : t->vcall; }

const uint8_t *nxdn_vtx_frame(nxdn_vtx_t *t, uint8_t *vch, uint8_t *quarter)
{
    uint8_t q = (uint8_t)(t->frames & 3u);
    bool odd = (t->frames >> 2) & 1u;
    if (q == 0 && t->iv && odd) {                 /* announce the next session's IV */
        t->seed = nxdn_iv_next(&t->lfsr);
        nxdn_msg_build_vcall_iv(t->ivm, t->seed);
    }
    if (t->enc) {
        nxdn_vcrypt_frame(&t->vc, q);
        unsigned per = t->vc.efr ? 2u : 4u;
        for (unsigned i = 0; i < per; i++) {
            uint8_t pos = (uint8_t)((q << 2) | i);
            if (t->vc.efr) (void)nxdn_vcrypt_apply144(&t->vc, pos, vch + 18 * i);
            else (void)nxdn_vcrypt_apply72(&t->vc, pos, vch + 9 * i);
        }
    }
    if (q == 3 && t->iv && odd) nxdn_vcrypt_iv(&t->vc, t->seed);
    t->frames++;
    *quarter = q;
    return t->iv && odd ? t->ivm : t->vcall;
}

void nxdn_vtx_wipe(nxdn_vtx_t *t)
{
    cipher_wipe(t, sizeof *t);
}

/* ---------------- data ---------------- */
void nxdn_dcrypt_start(nxdn_ks_t *ks, uint64_t iv_seed) { nxdn_ks_start(ks, iv_seed); }
void nxdn_dcrypt_block(nxdn_ks_t *ks, uint8_t *data, size_t len) { nxdn_ks_xor_bits(ks, data, (unsigned)len * 8u); }

unsigned nxdn_msg_build_vcall_iv(uint8_t *m, uint64_t iv)
{
    memset(m, 0, 10);
    m[0] = 0x03;
    for (int i = 0; i < 8; i++) m[1 + i] = (uint8_t)(iv >> (56 - 8 * i));
    return 9;
}

uint64_t nxdn_msg_iv(const uint8_t *m)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | m[1 + i];
    return v;
}

void nxdn_msg_set_cipher(uint8_t *m, uint8_t cipher, uint8_t key_id)
{
    m[7] = (uint8_t)(((cipher & 3u) << 6) | (key_id & 0x3Fu));
}
