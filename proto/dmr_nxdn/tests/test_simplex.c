/* Simplex / direct mode: DMR direct (MS syncs), TDMA direct (TS1/TS2), repeater-out (BS + CACH),
 * reverse channel bursts, MS vs BS sync classification; NXDN RDCH direct with preamble and RAN. */
#include "test.h"
#include "../dmr/dmr.h"
#include "../nxdn/nxdn.h"
#include "../common/bits.h"
#include "../common/fec.h"

typedef struct {
    int ev[20]; dmr_event_t last[20];
    uint8_t f[200][9]; uint8_t fslot[200]; int n;
    uint8_t rc_cmd; int rc_ok;
} srec_t;
static void s_voice(void *u, uint8_t slot, const uint8_t f[9], uint8_t idx, uint8_t crypt)
{
    srec_t *r = u; (void)idx; (void)crypt;
    if (r->n < 200) { memcpy(r->f[r->n], f, 9); r->fslot[r->n++] = slot; }
}
static void s_event(void *u, const dmr_event_t *ev)
{
    srec_t *r = u;
    r->ev[ev->type]++;
    r->last[ev->type] = *ev;
    if (ev->type == DMR_EV_RC && ev->data) { r->rc_cmd = ev->data[0]; r->rc_ok = ev->crc_ok; }
}

static uint8_t st_buf[40000];
static unsigned st_n;
static void put(const uint8_t *d, unsigned n) { memcpy(st_buf + st_n, d, n); st_n += n; }
static void noise(unsigned n) { for (unsigned i = 0; i < n; i++) st_buf[st_n++] = (uint8_t)(trand() & 3u); }

static void test_sync_classes(void)
{
    CHECK_EQ(dmr_link_voice_sync(DMR_LINK_DIRECT, 0), DMR_SYNC_MS_VOICE);
    CHECK_EQ(dmr_link_data_sync(DMR_LINK_DIRECT, 1), DMR_SYNC_MS_DATA);
    CHECK_EQ(dmr_link_voice_sync(DMR_LINK_REPEATER, 1), DMR_SYNC_MS_VOICE);
    CHECK_EQ(dmr_link_voice_sync(DMR_LINK_TDMA_DIRECT, 0), DMR_SYNC_TS1_VOICE);
    CHECK_EQ(dmr_link_data_sync(DMR_LINK_TDMA_DIRECT, 1), DMR_SYNC_TS2_DATA);
    CHECK_EQ(dmr_link_voice_sync(DMR_LINK_BS, 1), DMR_SYNC_BS_VOICE);
    for (int s = 1; s < DMR_SYNC_COUNT; s++) {
        int bs = dmr_sync_is_bs((dmr_sync_t)s), ms = dmr_sync_is_ms((dmr_sync_t)s), di = dmr_sync_is_direct((dmr_sync_t)s);
        CHECK(bs + ms + di == (s == DMR_SYNC_RESERVED ? 0 : 1));
        /* minimum distance between patterns: with <= 4 errors no pattern is confused with another */
        for (int t = s + 1; t < DMR_SYNC_COUNT; t++) {
            uint64_t x = DMR_SYNC_PATTERN[s] ^ DMR_SYNC_PATTERN[t];
            int d = 0;
            while (x) { x &= x - 1; d++; }
            CHECK(d >= 10);
        }
        for (int k = 0; k < 20; k++) {
            uint64_t w = DMR_SYNC_PATTERN[s];
            for (int e = 0; e < 4; e++) w ^= 1ull << (trand() % 48u);
            int err;
            CHECK_EQ(dmr_sync_match(w, 4, &err), s);
        }
    }
}

/* run a talkspurt from dmr_call_tx through the receiver; other slot: noise (or an RC burst) */
static void run_call(dmr_link_t link, uint8_t slot, bool rx_bs_cach, bool with_rc, srec_t *r)
{
    memset(r, 0, sizeof *r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, 5, rx_bs_cach };
    dmr_cb_t cb = { s_voice, s_event, r };
    dmr_rx_init(&rx, &cfg, &cb);
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, 2620, 2621001, 0);
    dmr_call_tx_t t;
    dmr_call_tx_start(&t, link, slot, 5, &lc, 2, NULL);
    dmr_slc_t slc = { DMR_SLCO_ACT_UPDT, 0x123456 };
    t.slc = &slc;
    uint8_t out[DMR_SLOT_DIBITS], fr[27];
    static uint8_t sent[36][9];
    st_n = 0;
    noise(777);
    int nv = 0;
    for (int i = 0; i < 2 + 12; i++) {
        for (int b = 0; b < 27; b++) fr[b] = (uint8_t)trand();
        /* TDMA order on air: slot 0 then slot 1; the CACH short LC sequence follows that order */
        uint8_t other[DMR_SLOT_DIBITS];
        if (link == DMR_LINK_BS && slot == 1) {
            dmr_tx_cach_slc(&slc, t.cach_n++, 1, 0, other);
            dmr_tx_idle(5, DMR_SYNC_BS_DATA, other + DMR_CACH_DIBITS);
        }
        dmr_txk_t k = dmr_call_tx_next(&t, fr, out);
        if (k == DMR_TXK_VOICE) for (int f = 0; f < 3; f++) memcpy(sent[nv++], fr + 9 * f, 9);
        CHECK_EQ(k, i < 2 ? DMR_TXK_HEADER : DMR_TXK_VOICE);
        if (link == DMR_LINK_BS && slot == 0) {
            dmr_tx_cach_slc(&slc, t.cach_n++, 1, 1, other);
            dmr_tx_idle(5, DMR_SYNC_BS_DATA, other + DMR_CACH_DIBITS);
        } else if (link != DMR_LINK_BS) {
            for (int d = 0; d < DMR_SLOT_DIBITS; d++) other[d] = (uint8_t)(trand() & 3u);
            if (with_rc && i == 8) {          /* listener interrupts: standalone RC in the backward slot */
                uint8_t rc[DMR_RC_BURST_DIBITS];
                dmr_tx_rc_standalone(5, DMR_RC_CEASE_TX_REQ, rc);
                memcpy(other + DMR_CACH_DIBITS + DMR_RC_BURST_OFFSET, rc, DMR_RC_BURST_DIBITS);
            }
        }
        if (slot == 0) { put(out, DMR_SLOT_DIBITS); put(other, DMR_SLOT_DIBITS); }
        else { put(other, DMR_SLOT_DIBITS); put(out, DMR_SLOT_DIBITS); }
    }
    dmr_call_tx_end(&t, out);
    put(out, DMR_SLOT_DIBITS);
    unsigned cut = st_n;
    noise(144 * 20);
    if (link == DMR_LINK_BS) {
        /* stop in the middle of the stream: the next slot-N burst start must match the TX layout */
        unsigned half = 777 + 6 * 2 * DMR_SLOT_DIBITS + 100;
        dmr_rx_dibits(&rx, st_buf, half);
        uint32_t at;
        CHECK(dmr_rx_next_slot_start(&rx, slot, &at));
        unsigned first = 777 + (slot ? DMR_SLOT_DIBITS : 0) + DMR_CACH_DIBITS;   /* slot burst start in TX layout */
        CHECK_EQ((at - first) % (2u * DMR_SLOT_DIBITS), 0);
        CHECK(at >= half && at < half + 2u * DMR_SLOT_DIBITS);
        dmr_rx_dibits(&rx, st_buf + half, st_n - half);
    } else {
        dmr_rx_dibits(&rx, st_buf, st_n);
    }
    (void)cut;
    int same = 0;
    for (int i = 0; i < r->n && i < nv; i++) same += !memcmp(r->f[i], sent[i], 9);
    CHECK_EQ(r->n, 36);
    CHECK_EQ(same, 36);
    CHECK_EQ(r->ev[DMR_EV_VOICE_START], 1);
    CHECK_EQ(r->last[DMR_EV_VOICE_START].lc.src, 2621001);
    CHECK_EQ(r->last[DMR_EV_VOICE_START].sync, dmr_link_data_sync(link, slot));
    CHECK_EQ(r->ev[DMR_EV_VOICE_END], 1);
}

static void test_dmr_simplex(void)
{
    srec_t r;
    /* plain simplex / talkaround, receiver left in "repeater" (CACH) configuration: CACH ignored */
    run_call(DMR_LINK_DIRECT, 0, true, true, &r);
    CHECK_EQ(r.fslot[0], 0);
    CHECK_EQ(r.ev[DMR_EV_RC], 1);
    CHECK_EQ(r.rc_cmd, DMR_RC_CEASE_TX_REQ);
    CHECK_EQ(r.rc_ok, 1);
    CHECK_EQ(r.last[DMR_EV_RC].slot, 1);                 /* backward slot */
    CHECK_EQ(r.ev[DMR_EV_SHORT_LC], 0);                  /* no CACH on a direct channel */
    run_call(DMR_LINK_DIRECT, 0, false, false, &r);
    /* dual capacity TDMA direct: TS2 */
    run_call(DMR_LINK_TDMA_DIRECT, 1, false, false, &r);
    CHECK_EQ(r.fslot[0], 1);
    run_call(DMR_LINK_TDMA_DIRECT, 0, true, false, &r);
    CHECK_EQ(r.fslot[0], 0);
    /* MS -> repeater inbound format (what a repeater / another radio monitoring the input hears) */
    run_call(DMR_LINK_REPEATER, 0, true, false, &r);
    /* BS / hotspot outbound with CACH: slot from TC, short LC decoded */
    run_call(DMR_LINK_BS, 1, true, false, &r);
    CHECK_EQ(r.fslot[0], 1);
    CHECK(r.ev[DMR_EV_SHORT_LC] >= 3);
    CHECK_EQ(r.last[DMR_EV_SHORT_LC].slc.data24, 0x123456);

    /* table E.12 layout of the standalone RC burst */
    uint8_t rc[DMR_RC_BURST_DIBITS], b[96];
    dmr_tx_rc_standalone(0xA, DMR_RC_CEASE_TX_CMD, rc);
    dibits_to_bits(b, rc, DMR_RC_BURST_DIBITS);
    CHECK_EQ(bits_arr_val(b, 4), 0xA);                   /* L24, L23 = CC(3..0) */
    CHECK_EQ(b[4], 1);                                   /* L22 bit 1 = PI */
    CHECK_EQ(bits_arr_val(b, 2) , 2);
    uint16_t info = dmr_rc_info(DMR_RC_CEASE_TX_CMD);
    CHECK_EQ(b[8], (info >> 10) & 1u);                   /* L20 bit 1 = RC(10) */
    uint64_t w = 0;
    for (int i = 0; i < 48; i++) w = (w << 1) | b[24 + i];
    CHECK_EQ(w, DMR_SYNC_PATTERN[DMR_SYNC_MS_RC]);
    uint8_t cmd;
    CHECK_EQ(dmr_rc_check(info, &cmd), 0);
    CHECK_EQ(cmd, DMR_RC_CEASE_TX_CMD);
    CHECK_EQ(dmr_rc_check((uint16_t)(info ^ 1u), &cmd), -1);

    /* embedded RC on a BS outbound voice burst (6.4.2): PI = 1, LCSS single */
    memset(&r, 0, sizeof r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, -1, true };
    dmr_cb_t cb = { s_voice, s_event, &r };
    dmr_rx_init(&rx, &cfg, &cb);
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, 1, 2, 0);
    dmr_voice_tx_t vt;
    dmr_voice_tx_init(&vt, &lc, 1, DMR_SYNC_BS_VOICE);
    uint8_t burst[DMR_BURST_DIBITS], cach[DMR_CACH_DIBITS], fr[27], e32[32];
    dmr_slc_t nul = { 0, 0 };
    st_n = 0;
    noise(300);
    for (int i = 0; i < 12; i++) {
        memset(fr, 0, sizeof fr);
        dmr_tx_cach_slc(&nul, (unsigned)i % 4u, 1, (uint8_t)(i & 1), cach);
        if (i & 1) {
            if (i == 5) { dmr_rc_emb32(DMR_RC_CEASE_TX_CMD, e32); dmr_burst_build_voice_emb(burst, 1, 1, DMR_LCSS_SINGLE, e32, fr); }
            else dmr_tx_idle(1, DMR_SYNC_BS_DATA, burst);
        } else dmr_voice_tx_burst(&vt, fr, burst);
        put(cach, 12); put(burst, 132);
    }
    noise(300);
    dmr_rx_dibits(&rx, st_buf, st_n);
    CHECK_EQ(r.ev[DMR_EV_RC], 1);
    CHECK_EQ(r.rc_cmd, DMR_RC_CEASE_TX_CMD);
    CHECK_EQ(r.rc_ok, 1);

    /* packet data in direct mode */
    static uint8_t bursts[8][DMR_BURST_DIBITS];
    const char *msg = "simplex data";
    int nb = dmr_tx_data_unconfirmed(false, 77, 88, 4, (const uint8_t *)msg, strlen(msg), NULL, 2,
                                     dmr_link_data_sync(DMR_LINK_DIRECT, 0), bursts, 8);
    CHECK_EQ(nb, 3);
    memset(&r, 0, sizeof r);
    dmr_rx_init(&rx, &cfg, &cb);
    st_n = 0;
    noise(200);
    for (int i = 0; i < nb; i++) { noise(12); put(bursts[i], 132); noise(144); }
    noise(400);
    dmr_rx_dibits(&rx, st_buf, st_n);
    CHECK_EQ(r.ev[DMR_EV_DATA_PACKET], 1);
    CHECK_EQ(r.last[DMR_EV_DATA_PACKET].crc_ok, 1);
}

/* ---------------- NXDN direct ---------------- */
typedef struct { int ev[20]; int nv; nxdn_call_t call; } nsrec_t;
static void n_voice(void *u, const uint8_t *f, unsigned n, uint8_t pos, uint8_t crypt)
{
    nsrec_t *r = u; (void)f; (void)n; (void)pos; (void)crypt; r->nv++;
}
static void n_event(void *u, const nxdn_event_t *ev)
{
    nsrec_t *r = u;
    r->ev[ev->type]++;
    if (ev->type == NXDN_EV_VOICE_START) r->call = ev->call;
}

static void nxdn_direct_call(uint8_t tx_ran, int rx_ran, nsrec_t *r)
{
    memset(r, 0, sizeof *r);
    nxdn_rx_t rx;
    nxdn_rx_cfg_t cfg = { 2, false, false, rx_ran };
    nxdn_cb_t cb = { n_voice, n_event, r };
    nxdn_rx_init(&rx, &cfg, &cb);
    uint8_t p[64], frame[NXDN_FRAME_DIBITS], vcall[10], rel[10], msg[9], info[4];
    for (int i = 0; i < 100; i++) { uint8_t d = (uint8_t)(trand() & 3u); nxdn_rx_dibits(&rx, &d, 1); }
    unsigned np = nxdn_tx_preamble(3, p);
    nxdn_rx_dibits(&rx, p, np);
    nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 4001, 20);
    nxdn_msg_build_vcall(rel, NXDN_MSG_TX_REL, NXDN_CALL_CONFERENCE, 4001, 20);
    memset(msg, 0, sizeof msg);
    memcpy(msg, vcall, 8);
    nxdn_sacch_quarter(msg, 0, tx_ran, false, info);
    nxdn_tx_voice_frame(nxdn_lich_rdch_voice(false, NXDN_STEAL_BOTH), info, vcall, vcall, NULL, false, frame);
    nxdn_rx_dibits(&rx, frame, sizeof frame);
    uint8_t vch[36];
    memset(vch, 0x55, sizeof vch);
    for (unsigned k = 0; k < 8; k++) {
        nxdn_sacch_quarter(msg, k % 4u, tx_ran, true, info);
        nxdn_tx_voice_frame(nxdn_lich_rdch_voice(true, NXDN_STEAL_NONE), info, NULL, NULL, vch, false, frame);
        nxdn_rx_dibits(&rx, frame, sizeof frame);
    }
    nxdn_sacch_quarter(msg, 0, tx_ran, false, info);
    nxdn_tx_voice_frame(nxdn_lich_rdch_voice(false, NXDN_STEAL_BOTH), info, rel, rel, NULL, false, frame);
    nxdn_rx_dibits(&rx, frame, sizeof frame);
    for (int i = 0; i < 600; i++) { uint8_t d = (uint8_t)(trand() & 3u); nxdn_rx_dibits(&rx, &d, 1); }
}

static void test_nxdn_simplex(void)
{
    /* preamble (table 4.4-1): last 12 symbols = +3 +3 +3 then -3 +3 -3 +3 +3 -3 -3 -3 +3 */
    uint8_t p[64];
    unsigned n = nxdn_tx_preamble(2, p);
    static const uint8_t tail[12] = { 1, 1, 1, 3, 1, 3, 1, 1, 3, 3, 3, 1 };
    CHECK_EQ(n, 20);
    CHECK(!memcmp(p + 8, tail, 12));
    CHECK(p[0] == 1 && p[1] == 1 && p[2] == 3 && p[3] == 3);
    /* LICH values from table 5.2-2 (conventional, SU TX) */
    CHECK_EQ(nxdn_lich_rdch_voice(true, NXDN_STEAL_NONE), 0x56);     /* 1010110 */
    CHECK_EQ(nxdn_lich_rdch_voice(true, NXDN_STEAL_SECOND), 0x54);   /* 1010100 */
    CHECK_EQ(nxdn_lich_rdch_voice(false, NXDN_STEAL_BOTH), 0x40);    /* 1000000 */
    CHECK_EQ(nxdn_lich_rdch_data(false), 0x4E);                      /* 1001110 */
    CHECK_EQ(nxdn_lich_rdch_data(true), 0x48);                       /* 1001000 */

    nsrec_t r;
    nxdn_direct_call(12, 12, &r);                 /* matching RAN */
    CHECK_EQ(r.ev[NXDN_EV_VOICE_START], 1);
    CHECK_EQ(r.call.src, 4001);
    CHECK_EQ(r.nv, 32);
    CHECK_EQ(r.ev[NXDN_EV_VOICE_END], 1);
    nxdn_direct_call(12, 5, &r);                  /* other RAN: stays muted */
    CHECK_EQ(r.nv, 0);
    nxdn_direct_call(0, 5, &r);                   /* RAN 0 transmissions are heard by everyone */
    CHECK_EQ(r.nv, 32);
    nxdn_direct_call(7, -1, &r);                  /* receiver set to any RAN */
    CHECK_EQ(r.nv, 32);
}

/* colour code filter must mute voice of other CCs, burst A included (no EMB there) */
static int cc_call(uint8_t tx_cc, int rx_cc, bool headers, bool emb_errs, srec_t *r)
{
    memset(r, 0, sizeof *r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, rx_cc, true };
    dmr_cb_t cb = { s_voice, s_event, r };
    dmr_rx_init(&rx, &cfg, &cb);
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, 91, 9990011, 0);
    dmr_call_tx_t t;
    dmr_call_tx_start(&t, DMR_LINK_DIRECT, 0, tx_cc, &lc, 2, NULL);
    uint8_t out[DMR_SLOT_DIBITS], fr[27], other[DMR_SLOT_DIBITS];
    static uint8_t sent[36][9];
    int nv = 0;
    st_n = 0;
    noise(500);
    for (int i = 0; i < 2 + 12; i++) {
        for (int b = 0; b < 27; b++) fr[b] = (uint8_t)trand();
        dmr_txk_t k = dmr_call_tx_next(&t, fr, out);
        if (k == DMR_TXK_VOICE) for (int f = 0; f < 3; f++) memcpy(sent[nv++], fr + 9 * f, 9);
        if (k == DMR_TXK_HEADER && !headers) continue;                 /* late entry */
        if (emb_errs && k == DMR_TXK_VOICE && i == 3) {                /* first burst B: EMB broken */
            uint8_t q[DMR_BURST_DIBITS];
            uint8_t cc, pi, lcss;
            memcpy(q, out + DMR_CACH_DIBITS, DMR_BURST_DIBITS);
            for (int d = 0; d < 3; d++) q[54 + d] ^= 1u;                /* 3 bit errors, QR(16,7) fixes 2 */
            if (dmr_burst_emb(q, &cc, &pi, &lcss, NULL) < 0) memcpy(out + DMR_CACH_DIBITS, q, DMR_BURST_DIBITS);
            else for (int d = 0; d < 8; d++) out[DMR_CACH_DIBITS + 54 + (d & 3) + (d >> 2) * 20] ^= 1u;
        }
        put(out, DMR_SLOT_DIBITS);
        for (int d = 0; d < DMR_SLOT_DIBITS; d++) other[d] = (uint8_t)(trand() & 3u);
        put(other, DMR_SLOT_DIBITS);
    }
    dmr_call_tx_end(&t, out);
    put(out, DMR_SLOT_DIBITS);
    noise(144 * 20);
    dmr_rx_dibits(&rx, st_buf, st_n);
    /* delivered frames must be the tail of what was sent, in order */
    int off = nv - r->n, same = 0;
    for (int i = 0; i < r->n; i++) same += off >= 0 && !memcmp(r->f[i], sent[off + i], 9);
    CHECK_EQ(same, r->n);
    return r->n;
}

static void test_dmr_cc_filter(void)
{
    srec_t r;
    CHECK_EQ(cc_call(5, 5, true, false, &r), 36);          /* header, matching CC */
    CHECK_EQ(cc_call(5, 5, false, false, &r), 36);         /* late entry: burst A held until B's EMB */
    CHECK_EQ(r.ev[DMR_EV_VOICE_START], 1);
    CHECK_EQ(cc_call(7, 5, true, false, &r), 0);           /* other CC with header */
    CHECK_EQ(r.ev[DMR_EV_VOICE_START], 0);
    CHECK_EQ(cc_call(7, 5, false, false, &r), 0);          /* other CC, late entry: not even burst A */
    CHECK_EQ(r.ev[DMR_EV_VOICE_START], 0);
    CHECK_EQ(r.ev[DMR_EV_VOICE_END], 0);
    CHECK_EQ(cc_call(7, -1, false, false, &r), 36);        /* filter off */
    CHECK_EQ(cc_call(5, 5, true, true, &r), 36);           /* header vouches for the CC, bad EMB in B */
    CHECK_EQ(cc_call(5, 5, false, true, &r), 30);          /* late entry, bad EMB in B: first A+B muted */
}

void test_simplex(void)
{
    test_sync_classes();
    test_dmr_simplex();
    test_dmr_cc_filter();
    test_nxdn_simplex();
}
