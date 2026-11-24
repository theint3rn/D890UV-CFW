/* Security tests.
 *  - AES (FIPS-197 C.1/C.3), DES (classic 133457799BBCDFF1 vector), RC4, AES S-box from its definition
 *  - NXDN TS 1-D 7.2.3 IV generator outputs, 7.2.2 user data test vectors (scramble / DES / AES),
 *    checked at UDCH frame level through the receiver; 5.4.4 voice session layout rules
 *  - DMR TS 102 361-4 tables 6.15 / 6.16 authentication vectors, and the C_AHOY -> C_ACKU exchange
 *  - cfw DMR privacy: encrypted call with / without key, late entry, encrypted packet data
 */
#include <stdio.h>
#include "test.h"
#include "../common/cipher.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include "../nxdn/nxdn.h"
#include "../dmr/dmr.h"
#include "../dmr/dmr_t3.h"

static void hexb(const char *h, uint8_t *o)
{
    for (size_t i = 0; h[2 * i] && h[2 * i + 1]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        o[i] = (uint8_t)v;
    }
}

static void test_primitives(void)
{
    uint8_t k[32], pt[16], ct[16], exp[16];
    for (int i = 0; i < 32; i++) k[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++) pt[i] = (uint8_t)(0x11 * i);
    aes_ctx_t a;
    CHECK_EQ(aes_init(&a, k, 16), 0);
    aes_encrypt(&a, pt, ct);
    hexb("69c4e0d86a7b0430d8cdb78070b4c55a", exp);
    CHECK(!memcmp(ct, exp, 16));
    CHECK_EQ(aes_init(&a, k, 24), 0);
    aes_encrypt(&a, pt, ct);
    hexb("dda97ca4864cdfe06eaf70a0ec0d7191", exp);
    CHECK(!memcmp(ct, exp, 16));
    CHECK_EQ(aes_init(&a, k, 32), 0);
    aes_encrypt(&a, pt, ct);
    hexb("8ea2b7ca516745bfeafc49904b496089", exp);
    CHECK(!memcmp(ct, exp, 16));
    CHECK_EQ(aes_init(&a, k, 20), -1);
    /* AES S-box = affine(inverse(x)) (FIPS-197 5.1.1), via a 1-byte key schedule probe:
     * the first round key word of AES-128 with key 0 is S-box dependent; simpler: check SubBytes via
     * one known value of the inverse/affine map for every x through the key expansion of key = x. */
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        for (int y = 1; y < 256 && x; y++) {
            uint8_t p = 0, aa = (uint8_t)x, bb = (uint8_t)y;
            while (bb) { if (bb & 1) p ^= aa; aa = (uint8_t)((aa << 1) ^ ((aa & 0x80) ? 0x1B : 0)); bb >>= 1; }
            if (p == 1) { inv = (uint8_t)y; break; }
        }
        uint8_t s = inv;
        for (int r = 1; r < 5; r++) s ^= (uint8_t)((inv << r) | (inv >> (8 - r)));
        s ^= 0x63;
        /* key = 0..0 except last word x x x x: w4[0] = SBOX(x) ^ 1 (rcon) */
        uint8_t kk[16] = { 0 };
        kk[12] = kk[13] = kk[14] = kk[15] = (uint8_t)x;
        CHECK_EQ(aes_init(&a, kk, 16), 0);
        if (a.rk[16 + 3] != s) { CHECK_EQ(a.rk[16 + 3], s); break; }
    }
    des_ctx_t d;
    uint8_t dk[8], dp[8], dc[8], de[8];
    hexb("133457799BBCDFF1", dk); hexb("0123456789ABCDEF", dp); hexb("85E813540F0AB405", de);
    des_init(&d, dk);
    des_encrypt(&d, dp, dc);
    CHECK(!memcmp(dc, de, 8));
    rc4_ctx_t r;
    rc4_init(&r, (const uint8_t *)"Key", 3);
    const char *t = "Plaintext";
    uint8_t rc[9], re[9];
    for (int i = 0; i < 9; i++) rc[i] = (uint8_t)(t[i] ^ rc4_byte(&r));
    hexb("BBF316E8D940AF0AD3", re);
    CHECK(!memcmp(rc, re, 9));
}

/* ---------------- NXDN ---------------- */
static const char *IV64[8] = { "ABCDEF1234567890", "8B8DDEEB890F4CF1", "C83AB2073A8D80E8", "6C4663AF3D39244B",
                               "00A5754C8AC49E3F", "81C55D6EA55913C7", "C5522BA39D354F27", "C8065031EAF69931" };

static void keys_spec(nxdn_keyring_t *kr)
{
    memset(kr, 0, sizeof *kr);
    kr->k[0].cipher = NXDN_CIPHER_SCRAMBLE; kr->k[0].key_id = 1; kr->k[0].len = 2; kr->k[0].key[1] = 1;
    kr->k[1].cipher = NXDN_CIPHER_DES; kr->k[1].key_id = 2; kr->k[1].len = 8; hexb("ABCDEF0123456789", kr->k[1].key);
    kr->k[2].cipher = NXDN_CIPHER_AES; kr->k[2].key_id = 3; kr->k[2].len = 32;
    hexb("ABCDEF0123456789CDEF0123456789ABEF0123456789ABCD0123456789ABCDEF", kr->k[2].key);
    kr->n = 3;
}

typedef struct { int ev[20]; uint8_t data[24]; int ndata; uint8_t crypt; uint8_t vf[96][18]; uint8_t pos[96]; uint8_t vc[96]; int nv; } crec_t;
static void c_voice(void *u, const uint8_t *f, unsigned n, uint8_t pos, uint8_t crypt)
{
    crec_t *r = u;
    if (r->nv < 96) { memcpy(r->vf[r->nv], f, n / 8u); r->vc[r->nv] = crypt; r->pos[r->nv++] = pos; }
}
static void c_event(void *u, const nxdn_event_t *ev)
{
    crec_t *r = u;
    r->ev[ev->type]++;
    if (ev->type == NXDN_EV_UDCH && ev->len == 22 && (ev->data[0] & 0x3F) == NXDN_MSG_DCALL_DATA) {
        memcpy(r->data, ev->data + 2, 20);
        r->ndata++;
        r->crypt = ev->crypt;
    }
    if (ev->type == NXDN_EV_CRYPT) r->crypt = ev->crypt;
}

static void nxdn_iv_and_data(void)
{
    uint64_t st = 0xABCDEF1234567890ull;
    for (int i = 0; i < 8; i++) {
        uint8_t e[8];
        hexb(IV64[i], e);
        uint64_t o = nxdn_iv_next(&st), ev = 0;
        for (int b = 0; b < 8; b++) ev = (ev << 8) | e[b];
        CHECK(o == ev);
    }
    /* 7.2.2: "ABCDEFGHIJKLMNOPQRST" in the first DCALL (User Data), through a full UDCH frame */
    static const char *CT[3] = { "C143434245524730485A4D2C580E30D150545540",
                                 "FE04A2AB29D0CFAE27FAABCAB12DF741898BE218",
                                 "7C06FE6BF9BDA40E9ADBF432E471E71F1C6BE8BF" };
    nxdn_keyring_t kr;
    keys_spec(&kr);
    for (int c = 0; c < 3; c++) {
        nxdn_ks_t ks;
        uint8_t d[20], e[20];
        memcpy(d, "ABCDEFGHIJKLMNOPQRST", 20);
        CHECK_EQ(nxdn_ks_init(&ks, &kr.k[c]), 0);
        nxdn_dcrypt_start(&ks, 0xABCDEF1234567890ull);
        nxdn_dcrypt_block(&ks, d, 20);
        hexb(CT[c], e);
        CHECK(!memcmp(d, e, 20));

        /* the same through the receiver: DCALL header (cipher, key id, IV) + DCALL data */
        crec_t r;
        memset(&r, 0, sizeof r);
        nxdn_rx_t rx;
        nxdn_cb_t cb = { c_voice, c_event, &r };
        nxdn_rx_init(&rx, NULL, &cb);
        rx.keys = &kr;
        uint8_t info[23], frame[NXDN_FRAME_DIBITS], noise[100];
        for (int i = 0; i < 100; i++) noise[i] = (uint8_t)(trand() & 3u);
        nxdn_rx_dibits(&rx, noise, 100);
        memset(info, 0, sizeof info);
        info[0] = 7;                                  /* SR: RAN 7 */
        uint8_t *m = info + 1;
        m[0] = NXDN_MSG_DCALL_HDR; m[3] = 0x03; m[4] = 0xE9; m[5] = 0x01; m[6] = 0x2C;
        nxdn_msg_set_cipher(m, kr.k[c].cipher, kr.k[c].key_id);
        hexb("ABCDEF1234567890", m + 11);
        nxdn_tx_udch_frame(0x4F, info, frame);        /* RDCH 10, UDCH 01, no steal 11, outbound 1 */
        nxdn_rx_dibits(&rx, frame, sizeof frame);
        memset(info, 0, sizeof info);
        info[0] = 7;
        m[0] = NXDN_MSG_DCALL_DATA;
        memcpy(m + 2, e, 20);                         /* spec ciphertext on air */
        nxdn_tx_udch_frame(0x4F, info, frame);
        nxdn_rx_dibits(&rx, frame, sizeof frame);
        nxdn_rx_dibits(&rx, noise, 100);
        CHECK_EQ(r.ndata, 1);
        CHECK_EQ(r.crypt, NXDN_CRYPT_DECRYPTED);
        CHECK(!memcmp(r.data, "ABCDEFGHIJKLMNOPQRST", 20));
    }
}

/* 5.4.4.2: DES EHR session = 25 OFB outputs, the last 32 bits of VCH 4 / frame 4 / superframe 2 are
 * bits 0..31 of the 25th output; AES: 13th output. Scramble: P0..P783 per superframe. */
static void nxdn_voice_layout(void)
{
    nxdn_keyring_t kr;
    keys_spec(&kr);
    for (int c = 0; c < 3; c++) {
        nxdn_vcrypt_t v;
        nxdn_vcrypt_setup(&v, kr.k[c].cipher, kr.k[c].key_id, &kr.k[c], false);
        CHECK(v.keyed);
        nxdn_vcrypt_iv(&v, 0xABCDEF1234567890ull);
        nxdn_vcrypt_frame(&v, 0);
        CHECK(v.active);
        /* reference keystream */
        nxdn_ks_t ks;
        nxdn_ks_init(&ks, &kr.k[c]);
        nxdn_ks_start(&ks, 0xABCDEF1234567890ull);
        unsigned total = c == 0 ? 784u : 1568u;
        uint8_t ref[196];
        memset(ref, 0, sizeof ref);
        nxdn_ks_xor_bits(&ks, ref, total);
        if (c == 1) {   /* 25th DES output starts at bit 24*64 = 1536 */
            nxdn_ks_t k2;
            nxdn_ks_init(&k2, &kr.k[1]);
            nxdn_ks_start(&k2, 0xABCDEF1234567890ull);
            nxdn_ks_skip(&k2, 24 * 64);
            CHECK_EQ(k2.bpos, 64);                    /* exactly 24 blocks consumed */
        }
        if (c == 2) {
            nxdn_ks_t k2;
            nxdn_ks_init(&k2, &kr.k[2]);
            nxdn_ks_start(&k2, 0xABCDEF1234567890ull);
            nxdn_ks_skip(&k2, 12 * 128);
            CHECK_EQ(k2.bpos, 128);
        }
        /* walk the session as the receiver does and compare every VCH mask */
        int bad = 0;
        for (unsigned sf = 0; sf < (c == 0 ? 1u : 2u); sf++)
            for (unsigned q = 0; q < 4; q++) {
                if (sf || q) nxdn_vcrypt_frame(&v, (uint8_t)q);
                for (unsigned vch = 0; vch < 4; vch++) {
                    uint8_t f[7] = { 0 };
                    CHECK(nxdn_vcrypt_apply(&v, (uint8_t)((q << 2) | vch), f));
                    unsigned off = ((sf * 4 + q) * 4 + vch) * 49;
                    for (unsigned i = 0; i < 49; i++)
                        if ((unsigned)bits_get(f, i) != (unsigned)bits_get(ref, off + i)) bad++;
                }
            }
        CHECK_EQ(bad, 0);
        /* without a new IV the DES/AES session ends after 2 superframes; scramble restarts */
        nxdn_vcrypt_frame(&v, 0);
        CHECK_EQ(v.active, c == 0);
    }
}

/* ---------------- AMBE+2 3600x2450 frame FEC ---------------- */
#include "../common/ambe_fec.h"
#include "nxdn_ts1d_vectors.h"
static const uint8_t TONE49[7] = { 0xFE, 0xE2, 0x12, 0x12, 0x12, 0x10, 0x00 };   /* TS 1-A 7.2.1.1 */

static void ambe_fec(void)
{
    uint8_t o[9], e[9], d[7];
    ambe72_encode(TONE49, o);
    hexb("CEA8FE83ACC458200A", e);                       /* TS 1-A 7.2.1.1 */
    CHECK(!memcmp(o, e, 9));
    hexb("B9E881526173002A6B", e);                       /* TS 1-A 7.2.1.2 silence */
    CHECK_EQ(ambe72_decode(e, d), 0);
    uint8_t sil[7];
    hexb("F801A99F8CE080", sil);
    CHECK(!memcmp(d, sil, 7));
    ambe72_encode(sil, o);
    CHECK(!memcmp(o, e, 9));
    /* Golay(23,12): every pattern of up to 3 errors on random words is corrected */
    for (int t = 0; t < 2000; t++) {
        uint16_t dd = (uint16_t)(trand() & 0xFFFu), out;
        uint32_t cw = golay23_encode(dd);
        int ne = (int)(trand() % 4u);
        uint32_t err = 0;
        while (popcount32(err) < ne) err |= 1u << (trand() % 23u);
        CHECK_EQ(golay23_decode(cw ^ err, &out), ne);
        CHECK_EQ(out, dd);
    }
    int minw = 99;
    for (unsigned x = 1; x < 4096; x++) { int w = popcount32(golay23_encode((uint16_t)x)); if (w < minw) minw = w; }
    CHECK_EQ(minw, 7);
    /* frame: up to 3 errors in C0 and 3 in C1 corrected; random round trips */
    for (int t = 0; t < 300; t++) {
        uint8_t p[7], q[7];
        for (int i = 0; i < 7; i++) p[i] = (uint8_t)trand();
        p[6] &= 0x80;
        ambe72_encode(p, o);
        CHECK_EQ(ambe72_decode(o, q), 0);
        CHECK(!memcmp(p, q, 7));
        o[0] ^= 0x80;                         /* C0 bit 23 (dibit 0, first bit) */
        o[0] ^= 0x40;                         /* C0 bit 5 (dibit 0, second bit) */
        o[0] ^= 0x20;                         /* C1 bit 10 (dibit 1, first bit) */
        CHECK_EQ(ambe72_decode(o, q), 3);
        CHECK(!memcmp(p, q, 7));
    }
}

/* TS 1-D 7.2.1: the tone pattern encrypted per VCH position, then FEC encoded, bit exact */
static void nxdn_voice_vectors(void)
{
    nxdn_keyring_t kr;
    keys_spec(&kr);
    const char **V[3] = { VEC_SCR, VEC_DES, VEC_AES };
    const int N[3] = { 16, 32, 32 };
    for (int c = 0; c < 3; c++) {
        nxdn_vcrypt_t tx, rxv;
        nxdn_vcrypt_setup(&tx, kr.k[c].cipher, kr.k[c].key_id, &kr.k[c], false);
        nxdn_vcrypt_setup(&rxv, kr.k[c].cipher, kr.k[c].key_id, &kr.k[c], false);
        nxdn_vcrypt_iv(&tx, 0xABCDEF1234567890ull);
        nxdn_vcrypt_iv(&rxv, 0xABCDEF1234567890ull);
        int ok = 0, back = 0;
        for (int i = 0; i < N[c]; i++) {
            uint8_t q = (uint8_t)((i / 4) % 4), pos = (uint8_t)((q << 2) | (i % 4)), o[9], e[9], d[7];
            if (i % 4 == 0) { nxdn_vcrypt_frame(&tx, q); nxdn_vcrypt_frame(&rxv, q); }
            ambe72_encode(TONE49, o);
            CHECK(nxdn_vcrypt_apply72(&tx, pos, o));
            hexb(V[c][i], e);
            ok += !memcmp(o, e, 9);
            CHECK(nxdn_vcrypt_apply72(&rxv, pos, e));      /* decrypt the spec word */
            ambe72_decode(e, d);
            back += !memcmp(d, TONE49, 7);
        }
        CHECK_EQ(ok, N[c]);
        CHECK_EQ(back, N[c]);
    }
}

/* TS 1-A 7.2.2 EFR tone / silence patterns (88 <-> 144 bits); tone #2 = #1 with LSB set (TS 1-D 7.2.1) */
static const char *EFR_DATA[4] = { "09B0880CC621F680A82600", "09B0880CC621F680A82601",
                                   "040CFD7BFB7DF27B3D9E44", "040CFD7BFB7DF27B3D9E45" };
static const char *EFR_FEC[4] = { "38928490D433C0BE1B91844FF058A589D839", "38928490D433C0BE1B91844FF058A589D83B",
                                  "6C42E85DE2E8269363D981F9BE23B18AE004", "6C42E85DE2E8269363D981F9BE23B18AE006" };

static void efr_fec(void)
{
    uint8_t d[11], e[18], o[18], q[11];
    for (int i = 0; i < 4; i++) {
        hexb(EFR_DATA[i], d);
        hexb(EFR_FEC[i], e);
        ambe144_encode(d, o);
        CHECK(!memcmp(o, e, 18));
        CHECK_EQ(ambe144_decode(e, q), 0);
        CHECK(!memcmp(q, d, 11));
    }
    /* Hamming(15,11): distance 3, single errors corrected */
    for (unsigned x = 0; x < 2048; x += 7) {
        uint16_t cw = hamming1511_encode((uint16_t)x), out;
        CHECK_EQ(hamming1511_decode(cw, &out), 0);
        CHECK_EQ(out, x);
        unsigned b = trand() % 15u;
        CHECK_EQ(hamming1511_decode((uint16_t)(cw ^ (1u << b)), &out), 1);
        CHECK_EQ(out, x);
    }
    /* frame round trips; 3 errors in c0, 3 in c1, 1 in each Hamming word corrected */
    int fixed = 0;
    for (int t = 0; t < 300; t++) {
        for (int i = 0; i < 11; i++) d[i] = (uint8_t)trand();
        ambe144_encode(d, o);
        CHECK_EQ(ambe144_decode(o, q), 0);
        CHECK(!memcmp(q, d, 11));
        uint8_t x[18];
        memcpy(x, o, 18);
        x[0] ^= 0xE0;                   /* air bits 0..2 -> vector bits 0 (c0), 24 (c1), 48 (c2) */
        x[0] ^= 0x10;                   /* air bit 3 -> vector bit 72 (c3) */
        x[0] ^= 0x08;                   /* air bit 4 -> vector bit 96 (c4, Hamming) */
        int n = ambe144_decode(x, q);
        fixed += n == 5 && !memcmp(q, d, 11);
    }
    CHECK_EQ(fixed, 300);
}

static void nxdn_efr_vectors(void)
{
    nxdn_keyring_t kr;
    keys_spec(&kr);
    const char **V[3] = { VEC_EFR_SCR, VEC_EFR_DES, VEC_EFR_AES };
    const int N[3] = { 8, 16, 16 };
    uint8_t tone[2][11];
    hexb(EFR_DATA[0], tone[0]);
    hexb(EFR_DATA[1], tone[1]);
    for (int c = 0; c < 3; c++) {
        nxdn_vcrypt_t tx, rxv;
        nxdn_vcrypt_setup(&tx, kr.k[c].cipher, kr.k[c].key_id, &kr.k[c], true);
        nxdn_vcrypt_setup(&rxv, kr.k[c].cipher, kr.k[c].key_id, &kr.k[c], true);
        nxdn_vcrypt_iv(&tx, 0xABCDEF1234567890ull);
        nxdn_vcrypt_iv(&rxv, 0xABCDEF1234567890ull);
        int ok = 0, back = 0;
        for (int i = 0; i < N[c]; i++) {
            uint8_t q = (uint8_t)((i / 2) % 4), pos = (uint8_t)((q << 2) | (i % 2)), o[18], e[18], d[11];
            if (i % 2 == 0) { nxdn_vcrypt_frame(&tx, q); nxdn_vcrypt_frame(&rxv, q); }
            ambe144_encode(tone[i & 1], o);
            CHECK(nxdn_vcrypt_apply144(&tx, pos, o));
            hexb(V[c][i], e);
            ok += !memcmp(o, e, 18);
            CHECK(nxdn_vcrypt_apply144(&rxv, pos, e));
            ambe144_decode(e, d);
            back += !memcmp(d, tone[i & 1], 11);
        }
        CHECK_EQ(ok, N[c]);
        CHECK_EQ(back, N[c]);
    }
}

/* TX sequencing helper (nxdn_vtx_*) against the TS 1-D 7.2.1 vectors: IV generator state ABCDEF1234567890
 * gives IV(1) = the vector IV; VCH words bit exact; superframe 1 SACCH = VCALL_IV with IV64[1]; VCALL
 * carries the cipher type / key ID. Then a long call with every session's IV through the receiver. */
static void nxdn_vtx_vectors(void)
{
    nxdn_keyring_t kr;
    keys_spec(&kr);
    for (int efr = 0; efr < 2; efr++) {
        const char **V[3] = { efr ? VEC_EFR_SCR : VEC_SCR, efr ? VEC_EFR_DES : VEC_DES, efr ? VEC_EFR_AES : VEC_AES };
        const int per = efr ? 2 : 4, sz = efr ? 18 : 9;
        for (int c = 0; c < 3; c++) {
            const int N = (c == 0 ? 16 : 32) / (efr ? 2 : 1);
            uint8_t vcall[10], tone[2][11], ivx[8];
            nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
            nxdn_vtx_t t;
            CHECK(nxdn_vtx_start(&t, &kr.k[c], efr, 0xABCDEF1234567890ull, vcall));
            CHECK_EQ(vcall[7], (kr.k[c].cipher << 6) | kr.k[c].key_id);
            const uint8_t *h2 = nxdn_vtx_header2(&t);
            if (c == 0) CHECK(!memcmp(h2, vcall, 10));
            else { hexb(IV64[0], ivx); CHECK(h2[0] == 0x03 && !memcmp(h2 + 1, ivx, 8)); }
            hexb(EFR_DATA[0], tone[0]);
            hexb(EFR_DATA[1], tone[1]);
            int ok = 0;
            for (int f = 0; f < N / per; f++) {
                uint8_t vch[36], e[18], q;
                for (int k = 0; k < per; k++) {
                    if (efr) ambe144_encode(tone[k & 1], vch + 18 * k);
                    else ambe72_encode(TONE49, vch + 9 * k);
                }
                const uint8_t *m = nxdn_vtx_frame(&t, vch, &q);
                CHECK_EQ(q, f % 4);
                if (c > 0 && f / 4 == 1) { hexb(IV64[1], ivx); CHECK(m[0] == 0x03 && !memcmp(m + 1, ivx, 8)); }
                else CHECK(!memcmp(m, vcall, 8));
                for (int k = 0; k < per; k++) { hexb(V[c][f * per + k], e); ok += !memcmp(vch + sz * k, e, (size_t)sz); }
            }
            CHECK_EQ(ok, N);
            nxdn_vtx_wipe(&t);
        }
        /* clear call: no cipher field, VCALL in both header halves and every SACCH, VCH untouched */
        uint8_t vcall[10], vch[36], orig[36], q;
        nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
        nxdn_vtx_t t;
        CHECK(!nxdn_vtx_start(&t, NULL, efr, 1, vcall));
        CHECK_EQ(vcall[7], 0);
        CHECK(nxdn_vtx_header2(&t) == t.vcall);
        for (int i = 0; i < 36; i++) orig[i] = vch[i] = (uint8_t)trand();
        for (int f = 0; f < 8; f++) CHECK(!memcmp(nxdn_vtx_frame(&t, vch, &q), vcall, 8));
        CHECK(!memcmp(vch, orig, 36));
    }
}

/* long encrypted call built with nxdn_vtx_* through the receiver: 6 superframes = 3 IV sessions. Every RAN:
 * some header frames hold a near-FSW (<= 2 bit errors) that must not cost the header (VCALL + VCALL_IV). */
static void nxdn_vtx_call(int cidx, bool rx_has_key, bool efr, int ran)
{
    const int per = efr ? 2 : 4, sz = efr ? 18 : 9, NSF = 6;
    nxdn_keyring_t kr, none;
    keys_spec(&kr);
    memset(&none, 0, sizeof none);
    crec_t r;
    memset(&r, 0, sizeof r);
    nxdn_rx_t rx;
    nxdn_cb_t cb = { c_voice, c_event, &r };
    nxdn_rx_cfg_t cfg = { 2, efr, false, ran ? ran : -1 };
    nxdn_rx_init(&rx, &cfg, &cb);
    rx.keys = rx_has_key ? &kr : &none;
    uint8_t vcall[10], sacch[9], info[4], frame[NXDN_FRAME_DIBITS], noise[150];
    nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
    nxdn_vtx_t t;
    CHECK(nxdn_vtx_start(&t, &kr.k[cidx], efr, 0x0123456789ABCDEFull ^ (uint64_t)trand(), vcall));
    for (int i = 0; i < 150; i++) noise[i] = (uint8_t)(trand() & 3u);
    nxdn_rx_dibits(&rx, noise, 150);
    memset(sacch, 0, sizeof sacch);
    memcpy(sacch, vcall, 8);
    nxdn_sacch_quarter(sacch, 0, (uint8_t)ran, false, info);
    nxdn_tx_voice_frame(0x40, info, vcall, nxdn_vtx_header2(&t), NULL, efr, frame);
    nxdn_rx_dibits(&rx, frame, sizeof frame);
    static uint8_t plain[96][18];
    int np = 0;
    for (int f = 0; f < NSF * 4; f++) {
        uint8_t vch[36], q;
        for (int k = 0; k < per; k++) {
            uint8_t pv[11];
            for (int i = 0; i < 11; i++) pv[i] = (uint8_t)trand();
            if (efr) ambe144_encode(pv, plain[np]);
            else { pv[6] &= 0x80; ambe72_encode(pv, plain[np]); }
            memcpy(vch + sz * k, plain[np++], (size_t)sz);
        }
        const uint8_t *m = nxdn_vtx_frame(&t, vch, &q);
        memset(sacch, 0, sizeof sacch);
        memcpy(sacch, m, 9);
        nxdn_sacch_quarter(sacch, q, (uint8_t)ran, true, info);
        nxdn_tx_voice_frame(0x56, info, NULL, NULL, vch, efr, frame);
        nxdn_rx_dibits(&rx, frame, sizeof frame);
    }
    nxdn_rx_dibits(&rx, noise, 150);
    nxdn_vtx_wipe(&t);
    /* header frame decoded: VCALL + VCALL_IV halves (scramble: VCALL twice, the repeat is not reported) */
    CHECK_EQ(r.ev[NXDN_EV_FACCH1], cidx ? 2 : 1);
    CHECK_EQ(r.nv, NSF * 4 * per);
    int ok = 0;
    for (int i = 0; i < r.nv && i < np; i++) ok += !memcmp(r.vf[i], plain[i], (size_t)sz);
    CHECK_EQ(ok, rx_has_key ? NSF * 4 * per : 0);
    CHECK_EQ(r.crypt, rx_has_key ? NXDN_CRYPT_DECRYPTED : NXDN_CRYPT_ENCRYPTED);
}

/* full encrypted conventional voice call through TX frames and the receiver: FACCH1 VCALL + VCALL_IV,
 * SACCH alternating VCALL / VCALL_IV. The receiver hands out plain 72-bit frames. */
static void nxdn_voice_call(int cidx, bool rx_has_key, bool efr)
{
    const int per = efr ? 2 : 4, sz = efr ? 18 : 9;
    nxdn_keyring_t kr, none;
    keys_spec(&kr);
    memset(&none, 0, sizeof none);
    const nxdn_key_t *key = &kr.k[cidx];
    crec_t r;
    memset(&r, 0, sizeof r);
    nxdn_rx_t rx;
    nxdn_cb_t cb = { c_voice, c_event, &r };
    nxdn_rx_cfg_t cfg = { 2, efr, false, -1 };
    nxdn_rx_init(&rx, &cfg, &cb);
    rx.keys = rx_has_key ? &kr : &none;

    nxdn_vcrypt_t tx;
    nxdn_vcrypt_setup(&tx, key->cipher, key->key_id, key, efr);
    uint64_t lfsr = 0x1122334455667788ull;
    uint64_t seed = nxdn_iv_next(&lfsr);
    uint8_t vcall[10], ivm[10], sacch[9], info[4], frame[NXDN_FRAME_DIBITS];
    nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
    nxdn_msg_set_cipher(vcall, key->cipher, key->key_id);
    nxdn_msg_build_vcall_iv(ivm, seed);
    nxdn_vcrypt_iv(&tx, seed);
    uint8_t noise[150];
    for (int i = 0; i < 150; i++) noise[i] = (uint8_t)(trand() & 3u);
    nxdn_rx_dibits(&rx, noise, 150);
    memset(sacch, 0, sizeof sacch);
    memcpy(sacch, vcall, 8);
    nxdn_sacch_quarter(sacch, 0, 0, false, info);
    nxdn_tx_voice_frame(0x40, info, vcall, ivm, NULL, efr, frame);
    nxdn_rx_dibits(&rx, frame, sizeof frame);
    static uint8_t plain[64][18];
    int np = 0;
    for (int sfn = 0; sfn < 4; sfn++) {
        uint8_t msg[9];
        memset(msg, 0, sizeof msg);
        if (sfn & 1) {          /* 2nd superframe of a session: next IV */
            seed = nxdn_iv_next(&lfsr);
            nxdn_msg_build_vcall_iv(ivm, seed);
            memcpy(msg, ivm, 9);
        } else memcpy(msg, vcall, 8);
        for (unsigned q = 0; q < 4; q++) {
            nxdn_vcrypt_frame(&tx, (uint8_t)q);
            uint8_t vch[36];
            for (int k = 0; k < per; k++) {
                uint8_t pv[11];
                uint8_t pos = (uint8_t)((q << 2) | (unsigned)k);
                for (int i = 0; i < 11; i++) pv[i] = (uint8_t)trand();
                if (efr) {
                    ambe144_encode(pv, plain[np++]);
                    memcpy(vch + 18 * k, plain[np - 1], 18);
                    CHECK(nxdn_vcrypt_apply144(&tx, pos, vch + 18 * k));
                } else {
                    pv[6] &= 0x80;
                    ambe72_encode(pv, plain[np++]);
                    memcpy(vch + 9 * k, plain[np - 1], 9);
                    CHECK(nxdn_vcrypt_apply72(&tx, pos, vch + 9 * k));
                }
            }
            nxdn_sacch_quarter(msg, q, 0, true, info);
            nxdn_tx_voice_frame(0x56, info, NULL, NULL, vch, efr, frame);
            if (sfn == 2 && q == 1) frame[100] ^= 1u;      /* a channel bit error inside an encrypted VCH */
            nxdn_rx_dibits(&rx, frame, sizeof frame);
            if (q == 3 && (sfn & 1)) nxdn_vcrypt_iv(&tx, seed);
        }
    }
    nxdn_rx_dibits(&rx, noise, 150);
    CHECK_EQ(r.nv, 16 * per);
    CHECK_EQ(r.crypt, rx_has_key ? NXDN_CRYPT_DECRYPTED : NXDN_CRYPT_ENCRYPTED);
    int ok = 0, dec = 0;
    for (int i = 0; i < r.nv && i < np; i++) {
        CHECK_EQ(r.pos[i], ((i / per) % 4) << 2 | (i % per));
        ok += !memcmp(r.vf[i], plain[i], (size_t)sz);
        dec += r.vc[i] == NXDN_CRYPT_DECRYPTED;
    }
    CHECK_EQ(ok, rx_has_key ? 16 * per : 0);    /* the bit error in a protected bit was corrected by the FEC */
    CHECK_EQ(dec, rx_has_key ? 16 * per : 0);
}

/* ---------------- DMR ---------------- */
typedef struct { uint16_t l; int tunes; dmr_csbk_t tx[4]; int ntx; int ev[16]; } trec_t;
static void t_tune(void *u, uint16_t l, uint8_t s, bool c) { trec_t *r = u; (void)s; (void)c; r->l = l; r->tunes++; }
static void t_tx(void *u, const dmr_csbk_t *c, uint8_t s) { trec_t *r = u; (void)s; if (r->ntx < 4) r->tx[r->ntx++] = *c; }
static void t_ev(void *u, const t3_event_t *e) { trec_t *r = u; r->ev[e->type]++; }

static void dmr_auth(void)
{
    uint8_t k[16], psn[3] = { 1, 2, 3 };
    for (int i = 0; i < 16; i++) k[i] = (uint8_t)(i + 1);
    CHECK_EQ(dmr_t3_auth_response(k, 0x7A17C0, NULL), 0xE8CA1D);     /* table 6.15 */
    CHECK_EQ(dmr_t3_auth_response(k, 0x7A17C0, psn), 0xE9C81E);      /* table 6.16 */

    trec_t r;
    memset(&r, 0, sizeof r);
    dmr_t3_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.my_id = 4242;
    cfg.hunt[0] = 1; cfg.n_hunt = 1;
    cfg.syscode_filter = -1;
    cfg.auth = true;
    memcpy(cfg.auth_key, k, 16);
    cfg.use_psn = true;
    memcpy(cfg.psn, psn, 3);
    dmr_t3_cb_t cb = { t_tune, t_tx, t_ev, &r };
    dmr_t3_t t;
    dmr_t3_init(&t, &cfg, &cb);
    dmr_t3_start(&t);
    dmr_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = DMR_EV_CSBK;
    t3_aloha_t a;
    memset(&a, 0, sizeof a);
    a.syscode = 0x1234;
    dmr_t3_build_aloha(&ev.csbk, &a);
    dmr_t3_on_rx(&t, &ev);
    uint8_t d[8] = { 0, T3_SK_REGISTRATION, 0, 0x10, 0x92, 0x7A, 0x17, 0xC0 };   /* target 4242, challenge */
    dmr_csbk_set(&ev.csbk, T3_C_AHOY, 0, d);
    dmr_t3_on_rx(&t, &ev);
    CHECK_EQ(r.ntx, 1);
    CHECK_EQ(r.tx[0].opcode, T3_C_ACKU);
    t3_ack_t ack;
    dmr_t3_parse_ack(&r.tx[0], &ack);
    CHECK_EQ(ack.reason, T3_REASON_AUTH_RESP);
    CHECK_EQ(ack.target, 0xE9C81E);
    CHECK_EQ(ack.source, 4242);
    CHECK_EQ(r.ev[T3_EV_AUTH], 1);
}

typedef struct { uint8_t f[200][9]; uint8_t c[200]; int n; int ev[20]; dmr_event_t last[20]; uint8_t data[64]; } drec_t;
static void d_voice(void *u, uint8_t slot, const uint8_t f[9], uint8_t idx, uint8_t crypt)
{
    drec_t *r = u;
    (void)slot; (void)idx;
    if (r->n < 200) { memcpy(r->f[r->n], f, 9); r->c[r->n++] = crypt; }
}
static void d_event(void *u, const dmr_event_t *ev)
{
    drec_t *r = u;
    r->ev[ev->type]++;
    r->last[ev->type] = *ev;
    if (ev->type == DMR_EV_DATA_PACKET && ev->data && ev->len <= 64) memcpy(r->data, ev->data, ev->len);
}

static uint8_t g_s[60000];
static unsigned g_sn;
static void sput(const uint8_t *d, unsigned n) { memcpy(g_s + g_sn, d, n); g_sn += n; }
static void snoise(unsigned n) { for (unsigned i = 0; i < n; i++) g_s[g_sn++] = (uint8_t)(trand() & 3u); }

static void dmr_privacy(void)
{
    dmr_keyring_t kr;
    memset(&kr, 0, sizeof kr);
    kr.k[0].alg = DMR_PRIV_CFW_AES256; kr.k[0].key_id = 7;
    for (int i = 0; i < 32; i++) kr.k[0].key[i] = (uint8_t)(0xA0 + i);
    kr.n = 1;
    dmr_priv_t tp;
    CHECK_EQ(dmr_priv_setup(&tp, &kr.k[0]), 0);
    dmr_priv_sync(&tp, 0xDEADBEEF, 0);
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, 91, 2001, 0);
    dmr_voice_tx_t vt;
    dmr_voice_tx_init(&vt, &lc, 1, DMR_SYNC_TS1_VOICE);
    dmr_voice_tx_set_priv(&vt, &tp);
    uint8_t burst[132], plain[72][9];
    int np = 0;
    g_sn = 0;
    snoise(500);
    unsigned late_start = 0;
    dmr_tx_pi_header(&tp, 1, DMR_SYNC_TS1_DATA, burst);
    snoise(12); sput(burst, 132); snoise(144);
    dmr_tx_voice_header(&vt.lc, 1, DMR_SYNC_TS1_DATA, burst);
    snoise(12); sput(burst, 132); snoise(144);
    for (int b = 0; b < 24; b++) {         /* 4 superframes */
        uint8_t fr[27];
        for (int i = 0; i < 27; i++) fr[i] = (uint8_t)trand();
        memcpy(plain[np++], fr, 9); memcpy(plain[np++], fr + 9, 9); memcpy(plain[np++], fr + 18, 9);
        dmr_voice_tx_burst(&vt, fr, burst);
        if (b == 6) late_start = g_sn;     /* superframe 1 starts here */
        snoise(12); sput(burst, 132); snoise(144);
    }
    dmr_tx_terminator(&vt.lc, 1, DMR_SYNC_TS1_DATA, burst);
    snoise(12); sput(burst, 132); snoise(144 * 3);

    for (int mode = 0; mode < 3; mode++) {      /* 0: key, from the start; 1: no key; 2: key, late entry */
        drec_t r;
        memset(&r, 0, sizeof r);
        dmr_rx_t rx;
        dmr_rx_cfg_t cfg = { 4, -1, false };
        dmr_cb_t cb = { d_voice, d_event, &r };
        dmr_rx_init(&rx, &cfg, &cb);
        dmr_keyring_t empty;
        memset(&empty, 0, sizeof empty);
        rx.keys = mode == 1 ? &empty : &kr;
        unsigned from = mode == 2 ? late_start - 12 : 0;
        dmr_rx_dibits(&rx, g_s + from, g_sn - from);
        int dec = 0, enc = 0, same = 0;
        int first = mode == 2 ? 18 : 0;        /* late entry starts at frame 18 */
        for (int i = 0; i < r.n; i++) {
            if (r.c[i] == DMR_CRYPT_DECRYPTED) dec++;
            if (r.c[i] == DMR_CRYPT_ENCRYPTED) enc++;
            if (i + first < np && !memcmp(r.f[i], plain[i + first], 9)) same++;
        }
        if (mode == 0) {
            CHECK_EQ(r.n, 72); CHECK_EQ(dec, 72); CHECK_EQ(same, 72);
            CHECK(r.ev[DMR_EV_PRIVACY] >= 1);
            CHECK_EQ(r.last[DMR_EV_PRIVACY].priv.iv, 0xDEADBEEF);
            CHECK_EQ(r.last[DMR_EV_VOICE_START].lc.svc_opts & 0x40, 0x40);
        } else if (mode == 1) {
            CHECK_EQ(r.n, 72); CHECK_EQ(enc, 72); CHECK(same < 2);
            CHECK_EQ(r.last[DMR_EV_PRIVACY].crypt, DMR_CRYPT_ENCRYPTED);
        } else {
            /* superframe 1 (odd) carries the privacy LC: complete at burst E, burst F onwards decrypted */
            /* the 15 frames before it cannot be recognised as encrypted (no header seen): marked clear */
            CHECK_EQ(r.n, 54);
            CHECK_EQ(dec, 54 - 15);
            CHECK_EQ(same, 54 - 15);
            CHECK_EQ(enc, 0);
        }
    }

    /* encrypted unconfirmed packet data */
    static uint8_t bursts[8][132];
    const char *msg = "secret status report 42";
    dmr_priv_sync(&tp, 0x01020304, 0);
    int nb = dmr_tx_data_unconfirmed(false, 100, 2001, 4, (const uint8_t *)msg, strlen(msg), &tp, 1,
                                     DMR_SYNC_MS_DATA, bursts, 8);
    CHECK_EQ(nb, 1 + 1 + 3);
    drec_t r;
    memset(&r, 0, sizeof r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, -1, false };
    dmr_cb_t cb = { d_voice, d_event, &r };
    dmr_rx_init(&rx, &cfg, &cb);
    rx.keys = &kr;
    g_sn = 0;
    snoise(300);
    for (int i = 0; i < nb; i++) { snoise(12); sput(bursts[i], 132); snoise(144); }
    snoise(300);
    dmr_rx_dibits(&rx, g_s, g_sn);
    CHECK_EQ(r.ev[DMR_EV_DATA_PACKET], 1);
    CHECK_EQ(r.last[DMR_EV_DATA_PACKET].crc_ok, 1);
    CHECK_EQ(r.last[DMR_EV_DATA_PACKET].crypt, DMR_CRYPT_DECRYPTED);
    CHECK(!memcmp(r.data, msg, strlen(msg)));
    /* the ciphertext on air is not the plaintext */
    uint8_t info[12], bits[196];
    dmr_burst_info_bits(bursts[2], bits);
    CHECK(bptc196_decode(bits, info) >= 0);
    CHECK(memcmp(info, msg, 12) != 0);
    dmr_priv_wipe(&tp);
}

void test_crypt(void)
{
    test_primitives();
    nxdn_iv_and_data();
    nxdn_voice_layout();
    ambe_fec();
    nxdn_voice_vectors();
    efr_fec();
    nxdn_efr_vectors();
    for (int c = 0; c < 3; c++)
        for (int efr = 0; efr < 2; efr++) { nxdn_voice_call(c, true, efr); nxdn_voice_call(c, false, efr); }
    nxdn_vtx_vectors();
    for (int c = 0; c < 3; c++)
        for (int efr = 0; efr < 2; efr++) { nxdn_vtx_call(c, true, efr, 0); nxdn_vtx_call(c, false, efr, 0); }
    for (int ran = 1; ran < 64; ran++)
        for (int efr = 0; efr < 2; efr++) nxdn_vtx_call(1, true, efr, ran);
    dmr_auth();
    dmr_privacy();
}
