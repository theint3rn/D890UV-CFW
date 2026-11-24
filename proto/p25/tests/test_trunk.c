/* Trunking: message layouts, channel math and a simulated site (control
 * channel TSBKs through the real Phase 1 framer, voice channel events). */
#include "tst.h"
#include "p25_trunk.h"
#include "p25_fec.h"
#include <string.h>

typedef struct {
    uint32_t rx_hz, tx_hz;
    p25_mode_t mode;
    uint8_t slot;
    int tunes;
    int n_ev[16];
    p25_trunk_event_t last[16];
} tsim_t;

static void cb_tune(void *ctx, uint32_t rx, uint32_t tx, p25_mode_t mode, uint8_t slot)
{
    tsim_t *s = ctx;
    s->rx_hz = rx; s->tx_hz = tx; s->mode = mode; s->slot = slot; s->tunes++;
}

static void cb_event(void *ctx, const p25_trunk_event_t *ev)
{
    tsim_t *s = ctx;
    s->n_ev[ev->type]++;
    s->last[ev->type] = *ev;
}

/* Phase 1 receiver feeding the trunk follower */
static p25_trunk_t T;
static p25_p1_rx_t RX;
static void p1_ev(void *ctx, const p25_p1_event_t *ev) { (void)ctx; p25_trunk_p1_event(&T, ev); }
static void p1_voice(void *ctx, const uint8_t f[11], int e) { (void)ctx; (void)f; (void)e; p25_trunk_voice_activity(&T); }
static const p25_p1_cb_t P1CB = { p1_voice, p1_ev };

static void send_tsbk(const uint8_t b[10])
{
    uint8_t air[P25_TSBK_AIR(1)];
    uint8_t blk[1][10];
    memcpy(blk[0], b, 10);
    size_t n = p25_p1_build_tsbk(air, 0x5A3, (const uint8_t(*)[10])blk, 1, P25_SS_IDLE);
    p25_p1_rx_push(&RX, air, n, -90);
}

static void t_layouts(void)
{
    uint8_t b[10], t12[12];
    p25_tsbk_t m;
    p25_osp_iden_up_vu(b, 1, 5, -45000000, 12500, 851006250);
    memcpy(t12, b, 10); t12[10] = t12[11] = 0;
    p25_tsbk_parse(t12, &m);
    CHECK(m.known);
    CHECK_EQ(m.u.iden.iden, 1);
    CHECK_EQ(m.u.iden.e.base_hz, 851006250);
    CHECK_EQ(m.u.iden.e.spacing_hz, 12500);
    CHECK_EQ(m.u.iden.e.tx_offset_hz, -45000000);
    CHECK_EQ(m.u.iden.e.bw_hz, 12500);
    p25_osp_iden_up_tdma(b, 2, 3, 30000000, 12500, 769006250);
    memcpy(t12, b, 10);
    p25_tsbk_parse(t12, &m);
    CHECK(m.u.iden.e.tdma);
    CHECK_EQ(m.u.iden.e.slots, 2);
    CHECK_EQ(m.u.iden.e.tx_offset_hz, 30000000);
    /* IDEN_UP (0x3D, 800 MHz form): hand-packed bits */
    memset(t12, 0, 12);
    t12[0] = 0x3D;
    /* iden 3, bw 100 (12.5k), sign 0, offset 180 (45 MHz), spacing 50 (6.25k), base 851.00625 MHz/5 */
    p25_bits_put(t12 + 2, 0, 4, 3); p25_bits_put(t12 + 2, 4, 9, 100); p25_bits_put(t12 + 2, 13, 1, 0);
    p25_bits_put(t12 + 2, 14, 8, 180); p25_bits_put(t12 + 2, 22, 10, 50); p25_bits_put(t12 + 2, 32, 32, 851006250u / 5);
    p25_tsbk_parse(t12, &m);
    CHECK_EQ(m.u.iden.iden, 3);
    CHECK_EQ(m.u.iden.e.bw_hz, 12500);
    CHECK_EQ(m.u.iden.e.tx_offset_hz, -45000000);
    CHECK_EQ(m.u.iden.e.spacing_hz, 6250);
    /* channel math */
    p25_iden_t tab[16];
    memset(tab, 0, sizeof tab);
    tab[2].valid = true; tab[2].tdma = true; tab[2].slots = 2;
    tab[2].base_hz = 852000000; tab[2].spacing_hz = 12500; tab[2].tx_offset_hz = -45000000;
    uint32_t rx, tx; uint8_t slot; bool tdma;
    CHECK(p25_chan_to_freq(tab, 0x2005, &rx, &tx, &slot, &tdma));
    CHECK_EQ(rx, 852025000); CHECK_EQ(tx, 807025000); CHECK_EQ(slot, 1); CHECK(tdma);
    CHECK(!p25_chan_to_freq(tab, 0x5005, &rx, &tx, &slot, &tdma));
    /* grant / status builders vs parser */
    p25_osp_grp_v_grant(b, 0x40, 0x1010, 100, 0x123456);
    memcpy(t12, b, 10);
    p25_tsbk_parse(t12, &m);
    CHECK_EQ(m.opcode, 0);
    CHECK_EQ(m.u.grant.svc, 0x40); CHECK_EQ(m.u.grant.ch, 0x1010); CHECK_EQ(m.u.grant.grp, 100);
    CHECK_EQ(m.u.grant.src, 0x123456);
    p25_osp_net_sts(b, 1, 0xBEE00, 0x3C1, 0x1028, 0x70);
    memcpy(t12, b, 10);
    p25_tsbk_parse(t12, &m);
    CHECK_EQ(m.u.net.wacn, 0xBEE00); CHECK_EQ(m.u.net.sysid, 0x3C1); CHECK_EQ(m.u.net.ch, 0x1028);
    /* ISP builders: field positions */
    p25_isp_u_reg_req(b, 0xBEE00, 0x3C1, 0x00ABCD);
    CHECK_EQ(b[0], 0x2C);
    CHECK_EQ(p25_bits_get(b + 2, 8, 20), 0xBEE00);
    CHECK_EQ(p25_bits_get(b + 2, 28, 12), 0x3C1);
    CHECK_EQ(p25_bits_get(b + 2, 40, 24), 0x00ABCD);
    p25_isp_grp_aff_req(b, 0x3C1, 100, 0x00ABCD);
    CHECK_EQ(b[0], 0x28);
    CHECK_EQ(p25_bits_get(b + 2, 24, 16), 100);
    p25_isp_grp_v_req(b, 0, 100, 0x00ABCD);
    CHECK_EQ(b[0], 0x00);
    CHECK_EQ(p25_bits_get(b + 2, 24, 16), 100);
    CHECK_EQ(p25_bits_get(b + 2, 40, 24), 0x00ABCD);
}

static void t_site(void)
{
    tsim_t s;
    memset(&s, 0, sizeof s);
    p25_trunk_cb_t cb = { cb_tune, cb_event };
    p25_trunk_init(&T, &cb, &s);
    p25_p1_rx_init(&RX, P25_NAC_ANY, &P1CB, NULL);
    p25_trunk_add_cc(&T, 851012500);       /* dead */
    p25_trunk_add_cc(&T, 851506250);
    p25_trunk_add_tg(&T, 100, 5, false);
    p25_trunk_add_tg(&T, 200, 1, false);
    p25_trunk_add_tg(&T, 300, 1, true);
    p25_trunk_start(&T);
    CHECK_EQ(s.rx_hz, 851012500);
    CHECK_EQ(T.state, P25_TS_HUNT);
    p25_trunk_tick(&T, 1000);
    CHECK_EQ(s.rx_hz, 851012500);
    p25_trunk_tick(&T, 600);                 /* dwell over: next CC */
    CHECK_EQ(s.rx_hz, 851506250);

    uint8_t b[10];
    p25_osp_iden_up_vu(b, 1, 5, -45000000, 12500, 851006250);
    send_tsbk(b);
    CHECK_EQ(T.state, P25_TS_CC);
    CHECK_EQ(s.n_ev[P25_TE_CC_FOUND], 1);
    CHECK_EQ(T.nac, 0x5A3);
    p25_osp_iden_up_tdma(b, 2, 3, -45000000, 12500, 852000000);
    send_tsbk(b);
    p25_osp_net_sts(b, 1, 0xBEE00, 0x3C1, 0x1028, 0x70);
    send_tsbk(b);
    p25_osp_rfss_sts(b, 1, 0x3C1, 1, 2, 0x1028, 0x70);
    send_tsbk(b);
    CHECK_EQ(T.wacn, 0xBEE00);
    CHECK_EQ(T.site, 2);
    CHECK(s.n_ev[P25_TE_SITE] >= 1);
    uint32_t cctx;
    CHECK(p25_trunk_cc_tx_hz(&T, &cctx));
    CHECK_EQ(cctx, 851506250u - 45000000u);
    p25_osp_adj_sts(b, 1, 0x3C1, 1, 3, 0x1030, 0x70);
    send_tsbk(b);
    CHECK_EQ(T.n_adj, 1);
    CHECK_EQ(s.last[P25_TE_ADJ].rx_hz, 851006250u + 48u * 12500u);

    /* grants: locked-out and unlisted ignored */
    p25_osp_grp_v_grant(b, 0, 0x1011, 300, 1);
    send_tsbk(b);
    p25_osp_grp_v_grant(b, 0, 0x1012, 999, 1);
    send_tsbk(b);
    CHECK_EQ(T.state, P25_TS_CC);
    /* tg 100: follow (Phase 1) */
    p25_osp_grp_v_grant(b, 0, 0x1010, 100, 0x0ABCDE);
    send_tsbk(b);
    CHECK_EQ(T.state, P25_TS_VOICE);
    CHECK_EQ(s.rx_hz, 851006250u + 16u * 12500u);
    CHECK_EQ(s.tx_hz, 851006250u + 16u * 12500u - 45000000u);
    CHECK_EQ(s.mode, P25_MODE_P1);
    CHECK_EQ(s.last[P25_TE_CALL_START].tg, 100);
    CHECK_EQ(s.last[P25_TE_CALL_START].src, 0x0ABCDE);

    /* voice channel: LDU1 with a channel update announcing tg 200 (higher prio) on TDMA */
    static uint8_t air[P25_LDU_AIR];
    uint8_t imbe[9][11];
    memset(imbe, 0, sizeof imbe);
    p25_lc_t lc;
    p25_lc_group_voice(&lc, 0, 100, 0x0ABCDE);
    size_t n = p25_p1_build_ldu1(air, 0x5A3, (const uint8_t(*)[11])imbe, &lc, NULL, 0);
    p25_p1_rx_push(&RX, air, n, -80);
    CHECK_EQ(T.state, P25_TS_VOICE);
    CHECK(T.voice_seen);
    p25_trunk_tick(&T, 1000);                /* below hang time */
    CHECK_EQ(T.state, P25_TS_VOICE);
    memset(&lc, 0, sizeof lc);
    lc.b[0] = P25_LCO_GRP_V_UPDT;
    lc.b[1] = 0x20; lc.b[2] = 0x05; lc.b[3] = 0x00; lc.b[4] = 200;     /* ch A 0x2005, grp A 200 */
    lc.b[5] = 0x10; lc.b[6] = 0x10; lc.b[7] = 0x00; lc.b[8] = 100;     /* ch B = current */
    n = p25_p1_build_ldu1(air, 0x5A3, (const uint8_t(*)[11])imbe, &lc, NULL, 0);
    p25_p1_rx_push(&RX, air, n, -80);
    CHECK_EQ(s.n_ev[P25_TE_CALL_PREEMPT], 1);
    CHECK_EQ(T.call_tg, 200);
    CHECK_EQ(s.mode, P25_MODE_P2);
    CHECK_EQ(s.slot, 1);
    CHECK_EQ(s.rx_hz, 852025000);

    /* Phase 2 voice: PTT on the other slot is ignored, our slot keeps us, END_PTT ends */
    p25_p2_event_t pe;
    memset(&pe, 0, sizeof pe);
    pe.type = P25_P2_EV_MAC;
    pe.channel = 1;
    p25_mac_ptt(&pe.mac, false, 0x777, 200);
    p25_trunk_p2_event(&T, &pe);
    CHECK(T.voice_seen);
    CHECK_EQ(T.call_src, 0x777);
    p25_trunk_tick(&T, 1400);
    CHECK_EQ(T.state, P25_TS_VOICE);
    pe.channel = 0;
    p25_mac_end_ptt(&pe.mac, false, 0x5A3, 0x777, 200);
    p25_trunk_p2_event(&T, &pe);             /* wrong slot */
    CHECK_EQ(T.state, P25_TS_VOICE);
    pe.channel = 1;
    p25_trunk_p2_event(&T, &pe);
    CHECK_EQ(T.state, P25_TS_CC);
    CHECK_EQ(s.rx_hz, 851506250);
    CHECK_EQ(s.n_ev[P25_TE_CALL_END], 1);

    /* grant with no voice: back after grant_wait */
    p25_osp_grp_v_grant_updt(b, 0x1010, 100, 0x1013, 999);
    send_tsbk(b);
    CHECK_EQ(T.state, P25_TS_VOICE);
    p25_trunk_tick(&T, 900);
    CHECK_EQ(T.state, P25_TS_VOICE);
    p25_trunk_tick(&T, 200);
    CHECK_EQ(T.state, P25_TS_CC);

    /* TDU on the voice channel returns immediately */
    p25_osp_grp_v_grant(b, 0, 0x1010, 100, 5);
    send_tsbk(b);
    CHECK_EQ(T.state, P25_TS_VOICE);
    n = p25_p1_build_tdu(air, 0x5A3, 0);
    p25_p1_rx_push(&RX, air, n, -80);
    CHECK_EQ(T.state, P25_TS_CC);

    /* lower priority grant while on a call is ignored */
    p25_osp_grp_v_grant(b, 0, 0x2005, 200, 5);
    send_tsbk(b);
    CHECK_EQ(T.call_tg, 200);
    CHECK_EQ(T.state, P25_TS_VOICE);
    T.state = P25_TS_VOICE;                  /* simulate a TSBK seen on the voice carrier: ignored */
    p25_osp_grp_v_grant(b, 0, 0x1010, 100, 5);
    send_tsbk(b);
    CHECK_EQ(T.call_tg, 200);
    p25_trunk_tick(&T, 1100);                /* no voice -> back to CC */
    CHECK_EQ(T.state, P25_TS_CC);

    /* CC loss -> hunt (same CC first) */
    p25_trunk_tick(&T, 1600);
    CHECK_EQ(T.state, P25_TS_HUNT);
    CHECK(s.n_ev[P25_TE_CC_LOST] >= 1);

    /* system filter: wrong WACN makes it move on */
    T.wacn_filter = 0x12345;
    send_tsbk((p25_osp_net_sts(b, 1, 0xBEE00, 0x3C1, 0x1028, 0x70), b));
    CHECK_EQ(T.state, P25_TS_HUNT);
    CHECK_EQ(s.rx_hz, 851012500);
}

void test_trunk(void)
{
    t_layouts();
    t_site();
}
