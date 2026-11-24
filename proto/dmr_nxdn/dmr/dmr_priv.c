/* DMR cfw privacy (AES-256-CTR on vocoder frames / packet data) and Tier III authentication. */
#include "dmr_priv.h"
#include <string.h>

const dmr_key_t *dmr_keyring_find(const dmr_keyring_t *kr, uint8_t alg, uint8_t key_id)
{
    if (!kr) return NULL;
    for (unsigned i = 0; i < kr->n && i < DMR_MAX_KEYS; i++)
        if (kr->k[i].alg == alg && kr->k[i].key_id == key_id) return &kr->k[i];
    return NULL;
}

static void cfw_ks(dmr_priv_t *p, uint8_t kind, uint16_t sf, uint32_t off, uint8_t *out, size_t n)
{
    uint8_t in[16], blk[16];
    uint32_t ctr = off / 16u, pos = off % 16u;
    while (n) {
        memset(in, 0, sizeof in);
        in[0] = (uint8_t)(p->iv >> 24); in[1] = (uint8_t)(p->iv >> 16); in[2] = (uint8_t)(p->iv >> 8); in[3] = (uint8_t)p->iv;
        in[4] = (uint8_t)(sf >> 8); in[5] = (uint8_t)sf;
        in[6] = (uint8_t)(ctr >> 8); in[7] = (uint8_t)ctr;
        in[8] = kind;
        in[9] = p->key_id;
        aes_encrypt(&p->aes, in, blk);
        for (; pos < 16u && n; pos++, n--) *out++ = blk[pos];
        pos = 0;
        ctr++;
    }
    cipher_wipe(blk, sizeof blk);
}

int dmr_priv_setup(dmr_priv_t *p, const dmr_key_t *key)
{
    dmr_priv_wipe(p);
    if (!key || key->alg != DMR_PRIV_CFW_AES256) return -1;
    p->alg = key->alg;
    p->key_id = key->key_id;
    if (aes_init(&p->aes, key->key, 32) < 0) return -1;
    p->ks = cfw_ks;
    p->keyed = true;
    return 0;
}

void dmr_priv_wipe(dmr_priv_t *p) { cipher_wipe(p, sizeof *p); }

void dmr_priv_sync(dmr_priv_t *p, uint32_t iv, uint16_t sf)
{
    p->iv = iv;
    p->sf = sf;
    p->synced = true;
}

void dmr_priv_voice(dmr_priv_t *p, uint8_t burst, uint8_t frame, uint8_t vf[9])
{
    if (!p->keyed || !p->synced || !p->ks) return;
    uint8_t k[9];
    p->ks(p, 'V', p->sf, (uint32_t)(burst * 3u + frame) * 9u, k, 9);
    for (int i = 0; i < 9; i++) vf[i] ^= k[i];
}

void dmr_priv_data(dmr_priv_t *p, uint32_t off, uint8_t *buf, size_t n)
{
    if (!p->keyed || !p->synced || !p->ks) return;
    while (n) {
        uint8_t k[16];
        size_t m = n < sizeof k ? n : sizeof k;
        p->ks(p, 'D', 0, off, k, m);
        for (size_t i = 0; i < m; i++) buf[i] ^= k[i];
        buf += m; off += (uint32_t)m; n -= m;
    }
}

void dmr_priv_pi_build(const dmr_priv_info_t *i, uint8_t pi[10])
{
    pi[0] = (uint8_t)(0xC0u | (i->alg & 0x3Fu));
    pi[1] = i->key_id;
    pi[2] = (uint8_t)(i->iv >> 24); pi[3] = (uint8_t)(i->iv >> 16); pi[4] = (uint8_t)(i->iv >> 8); pi[5] = (uint8_t)i->iv;
    pi[6] = (uint8_t)(i->sf >> 8); pi[7] = (uint8_t)i->sf;
    pi[8] = 'C'; pi[9] = 'F';
}

int dmr_priv_pi_parse(const uint8_t pi[10], dmr_priv_info_t *i)
{
    if ((pi[0] & 0xC0u) != 0xC0u || pi[8] != 'C' || pi[9] != 'F') return -1;
    i->alg = pi[0] & 0x3Fu;
    i->key_id = pi[1];
    i->iv = ((uint32_t)pi[2] << 24) | ((uint32_t)pi[3] << 16) | ((uint32_t)pi[4] << 8) | pi[5];
    i->sf = (uint16_t)((pi[6] << 8) | pi[7]);
    return 0;
}

void dmr_priv_lc_build(const dmr_priv_info_t *i, uint8_t lc[9])
{
    lc[0] = DMR_PRIV_LC_FLCO & 0x3Fu;
    lc[1] = DMR_PRIV_LC_FID;
    lc[2] = (uint8_t)(((i->alg & 0xFu) << 4) | (i->key_id & 0xFu));
    lc[3] = (uint8_t)(i->iv >> 24); lc[4] = (uint8_t)(i->iv >> 16); lc[5] = (uint8_t)(i->iv >> 8); lc[6] = (uint8_t)i->iv;
    lc[7] = (uint8_t)(i->sf >> 8); lc[8] = (uint8_t)i->sf;
}

int dmr_priv_lc_parse(const uint8_t lc[9], dmr_priv_info_t *i)
{
    if ((lc[0] & 0x3Fu) != (DMR_PRIV_LC_FLCO & 0x3Fu) || lc[1] != DMR_PRIV_LC_FID) return -1;
    i->alg = lc[2] >> 4;
    i->key_id = lc[2] & 0xFu;
    i->iv = ((uint32_t)lc[3] << 24) | ((uint32_t)lc[4] << 16) | ((uint32_t)lc[5] << 8) | lc[6];
    i->sf = (uint16_t)((lc[7] << 8) | lc[8]);
    return 0;
}

uint32_t dmr_t3_auth_response(const uint8_t k[16], uint32_t challenge, const uint8_t *psn3)
{
    uint8_t key[19];
    key[0] = (uint8_t)(challenge >> 16); key[1] = (uint8_t)(challenge >> 8); key[2] = (uint8_t)challenge;
    memcpy(key + 3, k, 16);
    rc4_ctx_t rc;
    rc4_init(&rc, key, sizeof key);
    for (int i = 0; i < 256; i++) (void)rc4_byte(&rc);
    uint8_t r[3];
    for (int i = 0; i < 3; i++) r[i] = (uint8_t)(rc4_byte(&rc) ^ (psn3 ? psn3[i] : 0u));
    cipher_wipe(&rc, sizeof rc);
    cipher_wipe(key, sizeof key);
    return ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | r[2];
}
