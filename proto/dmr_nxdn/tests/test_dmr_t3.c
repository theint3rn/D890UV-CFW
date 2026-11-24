/* Tier III MS procedure test with a scripted TSCC (TS 102 361-4 clause 6). */
#include "test.h"
#include "../dmr/dmr_t3.h"

typedef struct {
    uint16_t tune_lpcn; uint8_t tune_slot; bool tune_ctrl; int tunes;
    dmr_csbk_t tx[8]; int ntx;
    int ev[16]; t3_event_t last[16];
} t3rec_t;

static void cb_tune(void *u, uint16_t l, uint8_t s, bool c) { t3rec_t *r = u; r->tune_lpcn = l; r->tune_slot = s; r->tune_ctrl = c; r->tunes++; }
static void cb_tx(void *u, const dmr_csbk_t *c, uint8_t s) { t3rec_t *r = u; (void)s; if (r->ntx < 8) r->tx[r->ntx++] = *c; }
static void cb_ev(void *u, const t3_event_t *e) { t3rec_t *r = u; r->ev[e->type]++; r->last[e->type] = *e; }

static uint32_t now;
static void run(dmr_t3_t *t, uint32_t ms) { for (uint32_t i = 0; i < ms; i += 10) dmr_t3_tick(t, now += 10); }

/* deliver a CSBK the way dmr_rx would (encoded + decoded through a real burst) */
static void feed_csbk(dmr_t3_t *t, dmr_csbk_t *c, uint8_t slot)
{
    uint8_t burst[132], bits[196];
    dmr_tx_csbk(c, 1, DMR_SYNC_BS_DATA, burst);
    dmr_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = DMR_EV_CSBK;
    ev.slot = slot;
    dmr_burst_info_bits(burst, bits);
    CHECK_EQ(dmr_csbk_decode(bits, DMR_MASK_CSBK, &ev.csbk), 0);
    dmr_t3_on_rx(t, &ev);
}

static void feed(dmr_t3_t *t, dmr_ev_type_t type, uint8_t slot)
{
    dmr_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.slot = slot;
    dmr_t3_on_rx(t, &ev);
}

void test_dmr_t3(void)
{
    /* PDU field coding round trips (table 7.11 / 7.19 / 7.20 / 7.25) */
    dmr_csbk_t c;
    t3_grant_t g = { 0x123, 1, 1, 0, 1, 0x00ABCD, 0x31A2B3 }, g2;
    dmr_t3_build_grant(&c, T3_TV_GRANT, &g);
    CHECK_EQ(c.raw[0], 0x80 | 0x31);
    CHECK_EQ(c.raw[2], 0x12); CHECK_EQ(c.raw[3], 0x3D);       /* LPCN 0x123, LCN 1, LE 1, EM 0, OFF 1 */
    dmr_t3_parse_grant(&c, &g2);
    CHECK_EQ(g2.lpcn, 0x123); CHECK_EQ(g2.target, 0xABCD); CHECK_EQ(g2.source, 0x31A2B3); CHECK_EQ(g2.offset, 1);
    dmr_t3_build_rand(&c, 0x01, 0, T3_SK_REGISTRATION, T3_ADDR_REGI, 2345678);
    CHECK_EQ(c.raw[0], 0x80 | 0x1F);
    CHECK_EQ(c.raw[2], 0x02); CHECK_EQ(c.raw[3], 0x0E);
    CHECK_EQ(c.raw[4], 0xFF); CHECK_EQ(c.raw[5], 0xFE); CHECK_EQ(c.raw[6], 0xC6);
    t3_syscode_t sc;
    dmr_t3_syscode_parse((uint16_t)((1u << 14) | (5u << 7) | (9u << 2) | 2u), &sc);   /* small: net 7 / site 5 bits */
    CHECK_EQ(sc.model, 1); CHECK_EQ(sc.net, 5); CHECK_EQ(sc.site, 9); CHECK_EQ(sc.par, 2);
    CHECK_EQ(dmr_t3_ack_class(T3_REASON_REG_ACCEPTED), T3_ACK_ACK);
    CHECK_EQ(dmr_t3_ack_class(T3_REASON_REG_REFUSED), T3_ACK_NACK);
    CHECK_EQ(dmr_t3_ack_class(0xA0), T3_ACK_QUEUED);
    CHECK_EQ(dmr_t3_ack_class(0xE0), T3_ACK_WAIT);

    /* ---- state machine ---- */
    t3rec_t r;
    memset(&r, 0, sizeof r);
    dmr_t3_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.my_id = 2345678;
    cfg.tg[0] = 9; cfg.tg[1] = 3100; cfg.n_tg = 2;
    cfg.hunt[0] = 5; cfg.hunt[1] = 7; cfg.n_hunt = 2;
    cfg.tscc_slot = 0;
    cfg.syscode_filter = -1;
    cfg.auto_register = true;
    dmr_t3_cb_t cb = { cb_tune, cb_tx, cb_ev, &r };
    dmr_t3_t t;
    now = 0;
    dmr_t3_init(&t, &cfg, &cb);
    dmr_t3_start(&t);
    CHECK_EQ(r.tune_lpcn, 5);
    run(&t, 700);                                  /* nothing on 5: dwell expires */
    CHECK_EQ(r.tune_lpcn, 7);
    CHECK(r.tune_ctrl);

    t3_aloha_t a;
    memset(&a, 0, sizeof a);
    a.syscode = 0x4123; a.backoff = 2; a.nrand_wait = 2; a.reg = 1;
    dmr_t3_build_aloha(&c, &a);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_CAMPED], 1);
    CHECK_EQ(r.last[T3_EV_CAMPED].syscode, 0x4123);
    run(&t, 200);
    CHECK_EQ(r.ntx, 1);                             /* registration C_RAND sent */
    CHECK_EQ(r.tx[0].opcode, T3_C_RAND);
    CHECK_EQ(r.tx[0].data[1] & 0xF, T3_SK_REGISTRATION);
    feed_csbk(&t, &c, 0);                           /* keep the CC alive */
    t3_ack_t ack = { 0, T3_REASON_REG_ACCEPTED, cfg.my_id, T3_ADDR_REGI };
    dmr_t3_build_ack(&c, T3_C_ACKD, &ack);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_REGISTERED], 1);

    /* a grant for a wanted talkgroup: follow it, then return after the call + hang time */
    t3_grant_t tv = { 33, 1, 0, 0, 0, 3100, 1111111 };
    dmr_t3_build_grant(&c, T3_TV_GRANT, &tv);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_GRANT], 1);
    CHECK_EQ(r.tune_lpcn, 33); CHECK_EQ(r.tune_slot, 1); CHECK(!r.tune_ctrl);
    feed(&t, DMR_EV_VOICE_START, 1);
    run(&t, 1000);
    feed(&t, DMR_EV_VOICE_LC_EMB, 1);
    feed(&t, DMR_EV_VOICE_END, 1);
    run(&t, 2000);
    CHECK_EQ(r.ev[T3_EV_RETURN], 0);                /* hang time not over yet */
    run(&t, 1200);
    CHECK_EQ(r.ev[T3_EV_RETURN], 1);
    CHECK_EQ(r.tune_lpcn, 7); CHECK(r.tune_ctrl);

    /* unwanted talkgroup: ignored */
    tv.target = 4444;
    dmr_t3_build_grant(&c, T3_TV_GRANT, &tv);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_GRANT], 1);

    /* our own call: C_RAND(TG voice), WACK, then grant; P_CLEAR returns */
    CHECK(dmr_t3_request_voice(&t, 91, true));
    int n0 = r.ntx;
    run(&t, 200);
    CHECK_EQ(r.ntx, n0 + 1);
    CHECK_EQ(r.tx[n0].data[1] & 0xF, T3_SK_TG_VOICE);
    CHECK_EQ(r.tx[n0].data[4], 91);
    ack.reason = 0xE0; ack.target = cfg.my_id;
    dmr_t3_build_ack(&c, T3_C_ACKD, &ack);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_REQUEST_ACK], 1);
    CHECK_EQ(r.last[T3_EV_REQUEST_ACK].ack, T3_ACK_WAIT);
    tv.target = 91; tv.source = cfg.my_id; tv.lpcn = 40; tv.lcn = 0;
    dmr_t3_build_grant(&c, T3_TV_GRANT, &tv);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_GRANT], 2);
    CHECK_EQ(r.tune_lpcn, 40);
    dmr_t3_build_p_clear(&c, 0, 1, 91, T3_ADDR_TSI);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_RETURN], 2);

    /* presence check: C_AHOY to us -> C_ACKU */
    t3_ahoy_t ah;
    memset(&ah, 0, sizeof ah);
    dmr_csbk_t ahc;
    uint8_t d[8] = { 0 };
    d[1] = T3_SK_REGISTRATION;
    d[2] = (uint8_t)(cfg.my_id >> 16); d[3] = (uint8_t)(cfg.my_id >> 8); d[4] = (uint8_t)cfg.my_id;
    d[5] = 0xFF; d[6] = 0xFE; d[7] = 0xCA;
    dmr_csbk_set(&ahc, T3_C_AHOY, 0, d);
    dmr_t3_parse_ahoy(&ahc, &ah);
    CHECK_EQ(ah.target, cfg.my_id);
    n0 = r.ntx;
    feed_csbk(&t, &ahc, 0);
    CHECK_EQ(r.ntx, n0 + 1);
    CHECK_EQ(r.tx[n0].opcode, T3_C_ACKU);

    /* manufacturer CSBK reaches the hook */
    uint8_t md[8] = { 0 };
    dmr_csbk_set(&c, 0x3E, 0x10, md);
    feed_csbk(&t, &c, 0);
    CHECK_EQ(r.ev[T3_EV_MFID_CSBK], 1);

    /* TSCC goes silent: hunt again */
    run(&t, 3000);
    CHECK(r.ev[T3_EV_LOST_CC] >= 1);
    CHECK(r.tunes >= 7);
}
