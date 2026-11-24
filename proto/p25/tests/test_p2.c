/* Phase 2 tests. Known-answer data: I-ISCH code words and the DUID decode
 * table as published in OP25 (p25p2_isch.cc, p25p2_duid.cc), scrambler
 * sequences computed with OP25's apps/tdma/lfsr.py algorithm (matrix seed
 * transform + segmented register, re-implemented in Python for the vectors). */
#include "tst.h"
#include "p25_p2.h"
#include "p25_fec.h"
#include <string.h>

static const uint64_t isch_tab[128] = {
    0x184229d461ULL, 0x18761451f6ULL, 0x181ae27e2fULL, 0x182edffbb8ULL, 0x18df8a7510ULL, 0x18ebb7f087ULL, 0x188741df5eULL, 0x18b37c5ac9ULL,
    0x1146a44f13ULL, 0x117299ca84ULL, 0x111e6fe55dULL, 0x112a5260caULL, 0x11db07ee62ULL, 0x11ef3a6bf5ULL, 0x1183cc442cULL, 0x11b7f1c1bbULL,
    0x1a4a2e239eULL, 0x1a7e13a609ULL, 0x1a12e589d0ULL, 0x1a26d80c47ULL, 0x1ad78d82efULL, 0x1ae3b00778ULL, 0x1a8f4628a1ULL, 0x1abb7bad36ULL,
    0x134ea3b8ecULL, 0x137a9e3d7bULL, 0x13166812a2ULL, 0x1322559735ULL, 0x13d300199dULL, 0x13e73d9c0aULL, 0x138bcbb3d3ULL, 0x13bff63644ULL,
    0x1442f705efULL, 0x1476ca8078ULL, 0x141a3cafa1ULL, 0x142e012a36ULL, 0x14df54a49eULL, 0x14eb692109ULL, 0x14879f0ed0ULL, 0x14b3a28b47ULL,
    0x1d467a9e9dULL, 0x1d72471b0aULL, 0x1d1eb134d3ULL, 0x1d2a8cb144ULL, 0x1ddbd93fecULL, 0x1defe4ba7bULL, 0x1d831295a2ULL, 0x1db72f1035ULL,
    0x164af0f210ULL, 0x167ecd7787ULL, 0x16123b585eULL, 0x162606ddc9ULL, 0x16d7535361ULL, 0x16e36ed6f6ULL, 0x168f98f92fULL, 0x16bba57cb8ULL,
    0x1f4e7d6962ULL, 0x1f7a40ecf5ULL, 0x1f16b6c32cULL, 0x1f228b46bbULL, 0x1fd3dec813ULL, 0x1fe7e34d84ULL, 0x1f8b15625dULL, 0x1fbf28e7caULL,
    0x084d62c339ULL, 0x08795f46aeULL, 0x0815a96977ULL, 0x082194ece0ULL, 0x08d0c16248ULL, 0x08e4fce7dfULL, 0x08880ac806ULL, 0x08bc374d91ULL,
    0x0149ef584bULL, 0x017dd2dddcULL, 0x011124f205ULL, 0x0125197792ULL, 0x01d44cf93aULL, 0x01e0717cadULL, 0x018c875374ULL, 0x01b8bad6e3ULL,
    0x0a456534c6ULL, 0x0a7158b151ULL, 0x0a1dae9e88ULL, 0x0a29931b1fULL, 0x0ad8c695b7ULL, 0x0aecfb1020ULL, 0x0a800d3ff9ULL, 0x0ab430ba6eULL,
    0x0341e8afb4ULL, 0x0375d52a23ULL, 0x03192305faULL, 0x032d1e806dULL, 0x03dc4b0ec5ULL, 0x03e8768b52ULL, 0x038480a48bULL, 0x03b0bd211cULL,
    0x044dbc12b7ULL, 0x0479819720ULL, 0x041577b8f9ULL, 0x04214a3d6eULL, 0x04d01fb3c6ULL, 0x04e4223651ULL, 0x0488d41988ULL, 0x04bce99c1fULL,
    0x0d493189c5ULL, 0x0d7d0c0c52ULL, 0x0d11fa238bULL, 0x0d25c7a61cULL, 0x0dd49228b4ULL, 0x0de0afad23ULL, 0x0d8c5982faULL, 0x0db864076dULL,
    0x0645bbe548ULL, 0x06718660dfULL, 0x061d704f06ULL, 0x06294dca91ULL, 0x06d8184439ULL, 0x06ec25c1aeULL, 0x0680d3ee77ULL, 0x06b4ee6be0ULL,
    0x0f41367e3aULL, 0x0f750bfbadULL, 0x0f19fdd474ULL, 0x0f2dc051e3ULL, 0x0fdc95df4bULL, 0x0fe8a85adcULL, 0x0f845e7505ULL, 0x0fb063f092ULL,
};

static const int8_t duid_tab[256] = {
    0,0,0,-1,0,-1,-1,1,0,-1,-1,4,-1,8,2,-1,  0,-1,-1,1,-1,1,1,1,-1,3,9,-1,5,-1,-1,1,
    0,-1,-1,10,-1,6,2,-1,-1,3,2,-1,2,-1,2,2,  -1,3,7,-1,11,-1,-1,1,3,3,-1,3,-1,3,2,-1,
    0,-1,-1,4,-1,6,12,-1,-1,4,4,4,5,-1,-1,4,  -1,13,7,-1,5,-1,-1,1,5,-1,-1,4,5,5,5,-1,
    -1,6,7,-1,6,6,-1,6,14,-1,-1,4,-1,6,2,-1,  7,-1,7,7,-1,6,7,-1,-1,3,7,-1,5,-1,-1,15,
    0,-1,-1,10,-1,8,12,-1,-1,8,9,-1,8,8,-1,8,  -1,13,9,-1,11,-1,-1,1,9,-1,9,9,-1,8,9,-1,
    -1,10,10,10,11,-1,-1,10,14,-1,-1,10,-1,8,2,-1,  11,-1,-1,10,11,11,11,-1,-1,3,9,-1,11,-1,-1,15,
    -1,13,12,-1,12,-1,12,12,14,-1,-1,4,-1,8,12,-1,  13,13,-1,13,-1,13,12,-1,-1,13,9,-1,5,-1,-1,15,
    14,-1,-1,10,-1,6,12,-1,14,14,14,-1,14,-1,-1,15,  -1,13,7,-1,11,-1,-1,15,14,-1,-1,15,-1,15,15,15,
};

static void t_isch_duid(void)
{
    int bad = 0;
    for (unsigned v = 0; v < 128; v++) if (p25_p2_isch_encode(v) != isch_tab[v]) bad++;
    CHECK_EQ(bad, 0);
    for (int t = 0; t < 300; t++) {
        unsigned v = rndn(128);
        uint64_t e = 0;
        while ((int)p25_popcount64(e) < t % 8) e |= 1ULL << rndn(40);
        if (p25_p2_isch_decode(isch_tab[v] ^ e, NULL) != (int)v) bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(p25_p2_isch_decode(P25_P2_SYNC ^ 0x10001ULL, NULL), -2);
    for (int v = 0; v < 256; v++) if (p25_p2_duid_decode((uint8_t)v) != duid_tab[v]) bad++;
    CHECK_EQ(bad, 0);
    CHECK_EQ(p25_p2_isch_for_slot(4, 1), p25_p2_isch_value(0, 1, 0, 1));
    CHECK_EQ(p25_p2_isch_for_slot(3, 0), -1);
    CHECK_EQ(p25_p2_slot_channel(10), 1);
    CHECK_EQ(p25_p2_slot_channel(11), 0);
    CHECK_EQ(p25_p2_slot_channel(7), 1);
}

static void t_scrambler(void)
{
    static p25_p2_scrambler_t s;
    static const uint8_t a_first[12] = { 0x00, 0x00, 0x10, 0x18, 0x29, 0x36, 0x2d, 0x3d, 0x7a, 0x7c, 0xc9, 0x8f };
    static const uint8_t a_last[4] = { 0xcc, 0xab, 0xbd, 0x73 };
    static const uint8_t b_first[12] = { 0xbe, 0xe0, 0x03, 0xc1, 0x5a, 0x33, 0x60, 0x8d, 0x0b, 0xb5, 0x35, 0x0e };
    static const uint8_t b_last[4] = { 0x27, 0x59, 0x83, 0x70 };
    p25_p2_scrambler_init(&s, 0x1, 0x18, 0x293);
    CHECK(memcmp(s.bits, a_first, 12) == 0);
    CHECK(memcmp(s.bits + 536, a_last, 4) == 0);
    p25_p2_scrambler_init(&s, 0xBEE00, 0x3C1, 0x5A3);
    CHECK(memcmp(s.bits, b_first, 12) == 0);
    CHECK(memcmp(s.bits + 536, b_last, 4) == 0);
    /* slot 0 position 10 -> first mask dibit */
    CHECK_EQ(p25_p2_scramble_dibit(&s, 0, 10), 0xbe >> 6);
}

static void rand_ambe(uint8_t f[7])
{
    for (int i = 0; i < 7; i++) f[i] = (uint8_t)rnd();
    f[6] &= 0x80;   /* 49 bits */
}

static void t_ambe(void)
{
    uint8_t f[7], g[7], d[36];
    /* PN check against OP25's pr_n[0] (u0 = 0) */
    memset(f, 0, 7);
    p25_ambe_encode(f, d);
    p25_ambe_decode(d, g);
    CHECK(memcmp(f, g, 7) == 0);
    int bad = 0;
    for (int t = 0; t < 300; t++) {
        rand_ambe(f);
        p25_ambe_encode(f, d);
        /* 1 random dibit error -> at most 2 bits, the Golay words fix them
         * unless it hits the unprotected c2/c3 bits: compare u0/u1 only then */
        unsigned p = rndn(36);
        d[p] ^= (uint8_t)(1 + rndn(3));
        int e = p25_ambe_decode(d, g);
        if (e < 0) bad++;
        if (p25_bits_get(f, 0, 24) != p25_bits_get(g, 0, 24)) bad++;
    }
    CHECK_EQ(bad, 0);
}

static void t_ess(void)
{
    p25_p2_ess_t e = { 0x84, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8, 9 } }, o;
    uint8_t b[16], a[28];
    p25_p2_ess_encode(&e, b, a);
    b[3] ^= 0x21; a[7] ^= 3; a[20] ^= 0x3F;
    CHECK(p25_p2_ess_decode(b, a, &o) >= 0);
    CHECK_EQ(o.algid, 0x84);
    CHECK_EQ(o.kid, 0x1234);
    CHECK(memcmp(o.mi, e.mi, 9) == 0);
}

/* ------------------------------------------------------- superframe loop */
typedef struct {
    uint8_t voice[2][128][7];
    int nv[2];
    int n_mac, n_mac_bad, n_ess, n_sync, n_msgs;
    p25_mac_pdu_t last_ptt, last_end;
    p25_p2_ess_t ess;
    uint16_t grant_ch, grant_grp;
    uint32_t wacn;
} p2sink_t;

static void p2_voice(void *ctx, unsigned ch, const uint8_t f[7], int errs)
{
    p2sink_t *s = ctx;
    (void)errs;
    if (s->nv[ch] < 128) memcpy(s->voice[ch][s->nv[ch]], f, 7);
    s->nv[ch]++;
}

static void p2_msg(void *ctx, uint8_t mco, const uint8_t *msg, unsigned len, const p25_tsbk_t *t)
{
    p2sink_t *s = ctx;
    (void)msg; (void)len;
    s->n_msgs++;
    if (mco == 0x42 && t && t->known) { s->grant_ch = t->u.grant_updt.ch[1]; s->grant_grp = t->u.grant_updt.grp[1]; }
    if (mco == 0x7B && t && t->known) s->wacn = t->u.net.wacn;
}

static void p2_event(void *ctx, const p25_p2_event_t *ev)
{
    p2sink_t *s = ctx;
    switch (ev->type) {
    case P25_P2_EV_SYNC: s->n_sync++; break;
    case P25_P2_EV_MAC:
        s->n_mac++;
        if (ev->mac.opcode == P25_MAC_PTT) s->last_ptt = ev->mac;
        if (ev->mac.opcode == P25_MAC_END_PTT) s->last_end = ev->mac;
        p25_mac_messages(&ev->mac, p2_msg, s);
        break;
    case P25_P2_EV_MAC_BAD: s->n_mac_bad++; break;
    case P25_P2_EV_ESS: s->n_ess++; s->ess = ev->ess; break;
    default: break;
    }
}

static uint8_t p2stream[P25_P2_SLOT * 12 * 4 + 200];

static void t_superframe(int nerr)
{
    static p25_p2_scrambler_t scr;
    p25_p2_scrambler_init(&scr, 0xBEE00, 0x3C1, 0x5A3);
    p25_p2_ess_t ess = { 0x80, 0, { 0 } };
    ess.mi[0] = 0x5A;
    uint8_t essb[16], essa[28];
    p25_p2_ess_encode(&ess, essb, essa);
    static uint8_t frames[3 * 18][7];
    for (int i = 0; i < 3 * 18; i++) rand_ambe(frames[i]);

    size_t n = 0;
    for (int i = 0; i < 77; i++) p2stream[n++] = (uint8_t)rndn(4);
    int fi = 0;
    for (int sf = 0; sf < 3; sf++) {
        int vb = 0;
        for (unsigned slot = 0; slot < 12; slot++) {
            uint8_t *o = p2stream + n;
            int isch = p25_p2_isch_for_slot(slot, (unsigned)sf);
            unsigned ch = p25_p2_slot_channel(slot);
            p25_mac_pdu_t m;
            if (ch == 0 && slot < 10) {
                if (vb < 4) {
                    p25_p2_build_4v(o, slot, isch, &scr, (const uint8_t(*)[7])frames[fi], essb + 4 * vb);
                    fi += 4;
                } else {
                    p25_p2_build_2v(o, slot, isch, &scr, (const uint8_t(*)[7])frames[fi], essa);
                    fi += 2;
                }
                vb++;
            } else if (ch == 0) {               /* slot 11: SACCH of channel 0 */
                if (sf < 2) p25_mac_ptt(&m, true, 0x0ABCDE, 0x1234);
                else p25_mac_end_ptt(&m, true, 0x5A3, 0x0ABCDE, 0x1234);
                p25_p2_build_mac(o, slot, isch, &scr, &m);
            } else {                            /* channel 1: idle FACCH with messages */
                p25_mac_idle(&m, slot == 10);
                /* GRP_V_CH_GRANT_UPDT (0x42) + NET_STS_BCST (0x7B) */
                uint8_t *p = m.pdu + 1;
                p[0] = 0x42; p[1] = 0x10; p[2] = 0x05; p[3] = 0x00; p[4] = 0x64;
                p[5] = 0x10; p[6] = 0x07; p[7] = 0x01; p[8] = 0x2C;
                if (slot == 10) {
                    uint8_t net[11] = { 0x7B, 0x01, 0xBE, 0xE0, 0x03, 0xC1, 0x10, 0x01, 0x10, 0x01, 0x70 };
                    memcpy(p + 9, net, 11);
                }
                p25_p2_build_mac(o, slot, isch, slot == 10 ? NULL : &scr, &m);
            }
            n += P25_P2_SLOT;
        }
    }
    for (int i = 0; i < 50; i++) p2stream[n++] = (uint8_t)rndn(4);
    /* errors outside the ISCH/DUID fields */
    for (int e = 0; e < nerr; e++) {
        size_t p = rndn((uint32_t)n);
        size_t r = (p + 180 - 77 % 180) % 180;
        if (r < 21 || r == 57 || r == 142 || r == 179) continue;
        p2stream[p] ^= (uint8_t)(1 + rndn(3));
    }

    p2sink_t s;
    memset(&s, 0, sizeof s);
    p25_p2_cb_t cb = { p2_voice, p2_event };
    static p25_p2_rx_t rx;
    p25_p2_rx_init(&rx, &cb, &s);
    p25_p2_rx_set_scrambler(&rx, &scr);
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rndn(64);
        if (i + k > n) k = n - i;
        p25_p2_rx_push(&rx, p2stream + i, k, -70);
        i += k;
    }
    /* sync is found at slot 2 (first S-ISCH); voice of superframe 0 slots 0/1 is
     * before that, slot ids become known at slot 4 */
    CHECK(s.n_sync >= 1);
    int first = 18 - (s.nv[0] - 2 * 18);         /* frames missed in superframe 0 */
    CHECK(s.nv[0] >= 2 * 18 + 6);
    int vbad = 0;
    for (int i = 0; i < s.nv[0] && i < 128; i++)
        if (p25_bits_get(s.voice[0][i], 0, 24) != p25_bits_get(frames[first + i], 0, 24)) vbad++;
    CHECK_EQ(vbad, 0);
    CHECK_EQ(s.nv[1], 0);
    CHECK(s.n_ess >= 2);
    CHECK_EQ(s.ess.mi[0], 0x5A);
    CHECK_EQ(s.ess.algid, 0x80);
    CHECK_EQ(s.last_ptt.src, 0x0ABCDE);
    CHECK_EQ(s.last_ptt.grp, 0x1234);
    CHECK_EQ(s.last_ptt.algid, 0x80);
    CHECK_EQ(s.last_end.colour, 0x5A3);
    CHECK_EQ(s.last_end.grp, 0x1234);
    CHECK_EQ(s.grant_ch, 0x1007);
    CHECK_EQ(s.grant_grp, 0x012C);
    CHECK_EQ(s.wacn, 0xBEE00);
    CHECK(s.n_mac >= 3 * 6 - 3);
    if (!nerr) CHECK_EQ(s.n_mac_bad, 0);
}

void test_p2(void)
{
    t_isch_duid();
    t_scrambler();
    t_ambe();
    t_ess();
    t_superframe(0);
    t_superframe(40);
}
