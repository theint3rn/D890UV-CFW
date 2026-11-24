/* Encryption tests: cipher known-answer vectors (FIPS-197, classic DES,
 * openssl OFB output, independent Python ARC4), keystream layout, MI LFSR,
 * and encrypted Phase 1 / Phase 2 calls end to end. */
#include "tst.h"
#include "p25_crypt.h"
#include "p25_p1.h"
#include "p25_p2.h"
#include "p25_fec.h"
#include <string.h>

static void hex(const char *s, uint8_t *o)
{
    for (int i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        o[i] = (uint8_t)v;
    }
}

static const uint8_t aes_key[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                     21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

static void t_ciphers(void)
{
    uint8_t in[16], out[16], exp[48];
    hex("00112233445566778899aabbccddeeff", in);
    hex("8ea2b7ca516745bfeafc49904b496089", exp);           /* FIPS-197 C.3 */
    p25_aes256_ecb(aes_key, in, out);
    CHECK(memcmp(out, exp, 16) == 0);
    /* AES-256-OFB (openssl) */
    hex("0011223344556677ff00112233445566", in);
    hex("80754120560e0acfc5f7168cde5045872ddaa28c88982efc63f9bf1ede5607eb482eeee73c5dbd84d187ff4ff691f294", exp);
    for (int b = 0; b < 3; b++) {
        p25_aes256_ecb(aes_key, in, in);
        CHECK(memcmp(in, exp + 16 * b, 16) == 0);
    }
    /* DES */
    uint8_t k8[8], d[8];
    hex("133457799bbcdff1", k8);
    hex("0123456789abcdef", in);
    p25_des_ecb(k8, in, d);
    hex("85e813540f0ab405", exp);
    CHECK(memcmp(d, exp, 8) == 0);
}

static void t_keystreams(void)
{
    static p25_keyring_t r;
    static p25_crypt_t c;
    p25_keyring_init(&r);
    uint8_t k[32], mi[9] = { 0 }, exp[32];
    /* DES-OFB: IV = MI, matches openssl -des-ofb */
    hex("0123456789abcdef", k);
    CHECK(p25_keyring_add(&r, P25_ALG_DES_OFB, 0x0010, k, 8));
    hex("1122334455667788", mi);
    CHECK(p25_crypt_prepare(&c, &r, P25_ALG_DES_OFB, 0x0010, mi, false));
    hex("b4cc3fd9d8d95214f11450c7beb46a74e3b89e967cecf2a65d2c1dd1aa287e99", exp);
    CHECK(memcmp(c.ks, exp, 32) == 0);
    /* AES: IV = MI || next(MI); first block = E(IV) */
    CHECK(p25_keyring_add(&r, P25_ALG_AES256, 0x0020, aes_key, 32));
    CHECK(p25_crypt_prepare(&c, &r, P25_ALG_AES256, 0x0020, mi, false));
    uint8_t iv[16], m2[9], blk[16];
    p25_mi_to_iv128(mi, iv);
    CHECK(memcmp(iv, mi, 8) == 0);
    memcpy(m2, mi, 9);
    p25_mi_next(m2);
    CHECK(memcmp(iv + 8, m2, 8) == 0);
    p25_aes256_ecb(aes_key, iv, blk);
    CHECK(memcmp(c.ks, blk, 16) == 0);
    /* ADP: ARC4 with key(5) || MI(8) (reference: independent Python ARC4) */
    hex("0102030405", k);
    CHECK(p25_keyring_add(&r, P25_ALG_ADP, 0x0030, k, 5));
    hex("a1b2c3d4e5f6071800", mi);
    CHECK(p25_crypt_prepare(&c, &r, P25_ALG_ADP, 0x0030, mi, false));
    hex("ecb7c4fa71ca3e519d5e2d0bb03ba7d0", exp);
    CHECK(memcmp(c.ks, exp, 16) == 0);
    /* first LDU1 IMBE frame of ADP uses keystream bytes 267..277 */
    uint8_t f[11] = { 0 };
    p25_crypt_imbe(&c, false, 0, f);
    hex("7567ba4397262ebd960af4", exp);
    CHECK(memcmp(f, exp, 11) == 0);
    /* missing key / unknown algorithm */
    CHECK(!p25_crypt_prepare(&c, &r, P25_ALG_AES256, 0x9999, mi, false));
    CHECK(!c.ready);
    CHECK(!p25_keyring_add(&r, 0x83, 1, k, 5));
    CHECK(!p25_keyring_add(&r, P25_ALG_ADP, 1, k, 6));
    /* short key is left-padded */
    CHECK(p25_keyring_add(&r, P25_ALG_AES256, 0x0021, k, 5));
    CHECK_EQ(p25_keyring_find(&r, P25_ALG_AES256, 0x0021)->key[27], 0x01);

    /* layout: AES Phase 1 offsets 27.., LDU2 sub 8 ends at 228; no overlap */
    CHECK(p25_crypt_prepare(&c, &r, P25_ALG_AES256, 0x0020, mi, false));
    uint8_t used[P25_KS_MAX] = { 0 };
    int bad = 0;
    for (int l = 0; l < 2; l++)
        for (unsigned s = 0; s < 9; s++) {
            memset(f, 0, 11);
            p25_crypt_imbe(&c, l, s, f);
            unsigned off = (l ? 101u : 0u) + 11u * s + (s < 8 ? 0u : 2u) + 27u;
            if (memcmp(f, c.ks + off, 11)) bad++;
            for (unsigned i = 0; i < 11; i++) if (used[off + i]++) bad++;
        }
    CHECK_EQ(bad, 0);
    uint8_t a[7] = { 0 };
    p25_crypt_ambe(&c, 4, 1, a);                  /* last Phase 2 frame: 16 + 7*17 = 135 */
    CHECK_EQ(a[0], c.ks[135]);
    CHECK_EQ(a[6], c.ks[141] & 0x80);
    p25_keyring_wipe(&r);
    CHECK_EQ(r.n, 0);
}

/* ------------------------------------------------- encrypted P1 call */
typedef struct { uint8_t v[64][11]; int n; int nocrypt_ev; uint8_t last_crypt; } vsink_t;
static void vs_voice(void *ctx, const uint8_t f[11], int e) { vsink_t *s = ctx; (void)e; if (s->n < 64) memcpy(s->v[s->n], f, 11); s->n++; }
static void vs_event(void *ctx, const p25_p1_event_t *ev)
{
    vsink_t *s = ctx;
    if (ev->type == P25_P1_EV_LDU1 || ev->type == P25_P1_EV_LDU2) { s->last_crypt = ev->crypt; if (ev->crypt == P25_VC_NOKEY) s->nocrypt_ev++; }
}

static uint8_t call[P25_HDU_AIR + 6 * P25_LDU_AIR + P25_TDU_AIR];

static void t_p1_call(uint8_t algid, uint16_t kid, const uint8_t *key, size_t klen)
{
    static p25_keyring_t r;
    static p25_crypt_t ctx, rxc;
    static p25_p1_voice_tx_t tx;
    static p25_p1_rx_t rx;
    p25_keyring_init(&r);
    CHECK(p25_keyring_add(&r, algid, kid, key, klen));
    uint8_t mi[9] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0 };
    p25_lc_t lc;
    p25_lc_group_voice(&lc, 0, 1000, 7777);
    size_t n = p25_p1_voice_tx_start_enc(&tx, call, 0x293, &lc, 0, &ctx, &r, algid, kid, mi);
    CHECK_EQ(n, P25_HDU_AIR);
    static uint8_t frames[54][11];
    for (int i = 0; i < 54; i++) {
        for (int b = 0; b < 11; b++) frames[i][b] = (uint8_t)rnd();
        frames[i][10] &= 0xFE;
        n += p25_p1_voice_tx_push(&tx, call + n, frames[i]);
    }
    size_t ldu1_2 = P25_HDU_AIR + 2 * P25_LDU_AIR;    /* start of the 2nd superframe */
    n += p25_p1_voice_tx_end(&tx, call + n, NULL);
    CHECK_EQ(n, P25_HDU_AIR + 6 * P25_LDU_AIR + P25_TDU_AIR);

    /* no decryption configured: encrypted voice is muted, not passed as noise */
    vsink_t s;
    p25_p1_cb_t cb = { vs_voice, vs_event };
    memset(&s, 0, sizeof s);
    p25_p1_rx_init(&rx, P25_NAC_ANY, &cb, &s);
    p25_p1_rx_push(&rx, call, n, 0);
    CHECK_EQ(s.n, 0);
    /* on air the voice is not the clear frames: decrypt with a wrong key */
    static p25_keyring_t wrong;
    p25_keyring_init(&wrong);
    uint8_t wk[32];
    memcpy(wk, key, klen);
    wk[0] ^= 0x80;
    p25_keyring_add(&wrong, algid, kid, wk, klen);
    memset(&s, 0, sizeof s);
    p25_p1_rx_init(&rx, P25_NAC_ANY, &cb, &s);
    p25_p1_rx_set_crypt(&rx, &wrong, &rxc);
    p25_p1_rx_push(&rx, call, n, 0);
    CHECK_EQ(s.n, 54);
    int same = 0;
    for (int i = 0; i < 54; i++) if (!memcmp(s.v[i], frames[i], 11)) same++;
    CHECK_EQ(same, 0);

    /* with the key: decrypted */
    memset(&s, 0, sizeof s);
    p25_p1_rx_init(&rx, P25_NAC_ANY, &cb, &s);
    p25_p1_rx_set_crypt(&rx, &r, &rxc);
    p25_p1_rx_push(&rx, call, n, 0);
    CHECK_EQ(s.n, 54);
    int bad = 0;
    for (int i = 0; i < 54; i++) if (memcmp(s.v[i], frames[i], 11)) bad++;
    CHECK_EQ(bad, 0);
    CHECK_EQ(s.last_crypt, P25_VC_DECRYPTED);

    /* key missing: everything muted */
    static p25_keyring_t empty;
    p25_keyring_init(&empty);
    memset(&s, 0, sizeof s);
    p25_p1_rx_init(&rx, P25_NAC_ANY, &cb, &s);
    p25_p1_rx_set_crypt(&rx, &empty, &rxc);
    p25_p1_rx_push(&rx, call, n, 0);
    CHECK_EQ(s.n, 0);
    CHECK(s.nocrypt_ev >= 4);

    /* late entry at the 2nd superframe: its LDU1 passes undecrypted (the LC
     * only says "encrypted" at the end of the LDU1), its LDU2 is muted (waiting
     * for the MI), the 3rd superframe decrypts correctly */
    memset(&s, 0, sizeof s);
    p25_p1_rx_init(&rx, P25_NAC_ANY, &cb, &s);
    p25_p1_rx_set_crypt(&rx, &r, &rxc);
    p25_p1_rx_push(&rx, call + ldu1_2, n - ldu1_2, 0);
    CHECK_EQ(s.n, 9 + 18);
    bad = 0;
    for (int i = 0; i < 18; i++) if (memcmp(s.v[9 + i], frames[36 + i], 11)) bad++;
    CHECK_EQ(bad, 0);
}

/* ------------------------------------------------- encrypted P2 call */
typedef struct { uint8_t v[64][7]; int n; } asink_t;
static void as_voice(void *ctx, unsigned ch, const uint8_t f[7], int e)
{
    asink_t *s = ctx; (void)e;
    if (ch == 0 && s->n < 64) memcpy(s->v[s->n++], f, 7);
}

static void t_p2_call(void)
{
    static p25_keyring_t r;
    static p25_crypt_t txc, rxc;
    static p25_p2_scrambler_t scr;
    static p25_p2_rx_t rx;
    static uint8_t air[P25_P2_SLOT * 12 * 3];
    p25_keyring_init(&r);
    CHECK(p25_keyring_add(&r, P25_ALG_AES256, 0x0042, aes_key, 32));
    p25_p2_scrambler_init(&scr, 0xBEE00, 0x3C1, 0x5A3);
    uint8_t mi[9] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x01, 0x02, 0x03, 0x04, 0 };
    static uint8_t frames[36][7];
    for (int i = 0; i < 36; i++) { for (int b = 0; b < 7; b++) frames[i][b] = (uint8_t)rnd(); frames[i][6] &= 0x80; }

    size_t n = 0;
    int fi = 0;
    for (int sf = 0; sf < 2; sf++) {
        CHECK(p25_crypt_prepare(&txc, &r, P25_ALG_AES256, 0x0042, mi, true));
        uint8_t next[9];
        memcpy(next, mi, 9);
        p25_mi_next(next);
        p25_p2_ess_t ess = { P25_ALG_AES256, 0x0042, { 0 } };
        memcpy(ess.mi, next, 9);                   /* ESS carries the next MI */
        uint8_t essb[16], essa[28];
        p25_p2_ess_encode(&ess, essb, essa);
        int vb = 0;
        for (unsigned slot = 0; slot < 12; slot++) {
            uint8_t *o = air + n;
            int isch = p25_p2_isch_for_slot(slot, (unsigned)sf);
            unsigned ch = p25_p2_slot_channel(slot);
            p25_mac_pdu_t m;
            if (ch == 0 && slot < 10) {
                uint8_t f[4][7];
                int nv = vb < 4 ? 4 : 2;
                for (int i = 0; i < nv; i++) {
                    memcpy(f[i], frames[fi + i], 7);
                    p25_crypt_ambe(&txc, (unsigned)vb, (unsigned)i, f[i]);
                }
                if (vb < 4) p25_p2_build_4v(o, slot, isch, &scr, (const uint8_t(*)[7])f, essb + 4 * vb);
                else p25_p2_build_2v(o, slot, isch, &scr, (const uint8_t(*)[7])f, essa);
                fi += nv;
                vb++;
            } else if (ch == 0) {                  /* SACCH during the call: MAC_ACTIVE */
                p25_mac_idle(&m, true);
                m.pdu[0] = (uint8_t)(P25_MAC_ACTIVE << 5);
                p25_p2_build_mac(o, slot, isch, &scr, &m);
            } else {
                p25_mac_idle(&m, slot == 10);
                p25_p2_build_mac(o, slot, isch, &scr, &m);
            }
            n += P25_P2_SLOT;
        }
        memcpy(mi, next, 9);
    }
    /* receive: channel 0, decryption on; first MI from a MAC_PTT event */
    asink_t s;
    memset(&s, 0, sizeof s);
    p25_p2_cb_t cb = { as_voice, NULL };
    p25_p2_rx_init(&rx, &cb, &s);
    rx.channel_filter = 0;
    p25_p2_rx_set_scrambler(&rx, &scr);
    p25_p2_rx_set_crypt(&rx, &r, &rxc);
    /* PTT with ALGID/KID/MI as it would arrive before the call */
    p25_mac_pdu_t ptt;
    p25_mac_ptt(&ptt, false, 0x1234, 1000);
    uint8_t mi0[9] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x01, 0x02, 0x03, 0x04, 0 };
    memcpy(ptt.pdu + 1, mi0, 9);
    ptt.pdu[10] = P25_ALG_AES256;
    ptt.pdu[11] = 0x00; ptt.pdu[12] = 0x42;
    uint8_t slotbuf[P25_P2_SLOT];
    p25_p2_build_mac(slotbuf, 11, -1, &scr, &ptt);
    p25_p2_rx_slot(&rx, slotbuf, 11);
    CHECK_EQ(rx.vc, P25_VC_DECRYPTED);
    /* superframe 0 slots handed directly (known slot ids), then superframe 1 */
    for (unsigned k = 0; k < 24; k++) p25_p2_rx_slot(&rx, air + k * P25_P2_SLOT, k % 12);
    CHECK_EQ(s.n, 36);
    int bad = 0;
    for (int i = 0; i < 36 && i < s.n; i++) if (memcmp(s.v[i], frames[i], 7)) bad++;
    CHECK_EQ(bad, 0);
}

void test_crypt(void)
{
    t_ciphers();
    t_keystreams();
    uint8_t des[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    uint8_t adp[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    t_p1_call(P25_ALG_AES256, 0x0101, aes_key, 32);
    t_p1_call(P25_ALG_DES_OFB, 0x0102, des, 8);
    t_p1_call(P25_ALG_ADP, 0x0103, adp, 5);
    t_p2_call();
}
