/* NXDN tests: scrambler / LICH / channel coding from TS 1-A, full conventional voice call through the
 * dibit receiver (EHR test pattern 7.2.1.1 carried opaquely), EFR, Type-D SCCH, Type-C trunking. */
#include "test.h"
#include "../nxdn/nxdn.h"
#include "../common/bits.h"

typedef struct {
    int ev[20];
    nxdn_event_t last[20];
    uint8_t lastdata[20][24];
    uint8_t frames[64][18];
    unsigned fbits[64];
    uint8_t pos[64];
    int nframes;
} nrec_t;

static void on_voice(void *u, const uint8_t *f, unsigned nbits, uint8_t pos, uint8_t crypt)
{
    (void)crypt;
    nrec_t *r = u;
    if (r->nframes < 64) r->pos[r->nframes] = pos;
    if (r->nframes < 64) { memcpy(r->frames[r->nframes], f, nbits / 8); r->fbits[r->nframes++] = nbits; }
}

static void on_event(void *u, const nxdn_event_t *ev)
{
    nrec_t *r = u;
    r->ev[ev->type]++;
    r->last[ev->type] = *ev;
    if (ev->data && ev->len <= 24 && (ev->type != NXDN_EV_SCCH || r->ev[ev->type] == 1))
        memcpy(r->lastdata[ev->type], ev->data, ev->len);       /* SCCH: keep the first one */
}

static uint8_t s_stream[30000];
static unsigned s_n;
static void sput(const uint8_t *d, unsigned n) { memcpy(s_stream + s_n, d, n); s_n += n; }
static void snoise(unsigned n) { for (unsigned i = 0; i < n; i++) s_stream[s_n++] = (uint8_t)(trand() & 3u); }

static void test_l1(void)
{
    /* FSW CDF59 = symbols -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 (table 4.4-2) */
    static const uint8_t fsw_sym[10] = { 3, 0, 3, 1, 3, 3, 1, 1, 2, 1 };
    uint8_t fb[20], fd[10];
    bits_arr_set(fb, 20, NXDN_FSW);
    bits_to_dibits(fd, fb, 10);
    CHECK(!memcmp(fd, fsw_sym, 10));

    /* scrambler (4.6): X^9+X^4+1, seed 0E4, first output for symbol S0 = frame symbol 10.
     * Derived by hand from table 4.6-1: S0..S5 = 0 0 1 0 0 1 */
    uint8_t f[NXDN_FRAME_DIBITS];
    memset(f, 0, sizeof f);
    nxdn_scramble(f, 10, NXDN_FRAME_DIBITS);
    static const uint8_t first[6] = { 0, 0, 2, 0, 0, 2 };
    CHECK(!memcmp(f + 10, first, 6));
    for (int i = 0; i < 10; i++) CHECK_EQ(f[i], 0);         /* FSW never scrambled */
    int ones = 0;
    for (int i = 10; i < 192; i++) ones += f[i] >> 1;
    CHECK(ones > 70 && ones < 112);                         /* m-sequence: balanced */
    nxdn_scramble(f, 10, NXDN_FRAME_DIBITS);
    for (int i = 0; i < 192; i++) CHECK_EQ(f[i], 0);

    /* LICH: table 5.2-2 RDCH SU voice superframe VCH only = 1010110, even parity over 4 MSBs */
    uint8_t ld[8], v;
    nxdn_lich_encode(0x56, ld);
    static const uint8_t lexp[8] = { 3, 1, 3, 1, 3, 3, 1, 1 };   /* bits 1010110 0 -> -3 +3 -3 +3 -3 -3 +3 +3 */
    CHECK(!memcmp(ld, lexp, 8));
    for (uint8_t x = 0; x < 128; x++) {
        nxdn_lich_encode(x, ld);
        CHECK_EQ(nxdn_lich_decode(ld, &v), 0);
        CHECK_EQ(v, x);
        ld[x % 4] ^= 2u;                                    /* flip one of the parity-covered bits */
        CHECK_EQ(nxdn_lich_decode(ld, &v), -1);
    }
    nxdn_lich_t l = { NXDN_RF_RDCH, NXDN_USC_SACCH_SS, NXDN_STEAL_NONE, 0 };
    CHECK_EQ(nxdn_lich_pack(&l), 0x56);

    /* functional channels: sizes (figures 4.5-1..6) and error correction */
    static const unsigned info[NXDN_CH_COUNT] = { 26, 80, 184, 152, 136, 104, 25 };
    static const unsigned coded[NXDN_CH_COUNT] = { 60, 144, 348, 300, 252, 252, 60 };
    for (int ch = 0; ch < NXDN_CH_COUNT; ch++) {
        CHECK_EQ(nxdn_ch_info_bits((nxdn_ch_t)ch), info[ch]);
        CHECK_EQ(nxdn_ch_coded_bits((nxdn_ch_t)ch), coded[ch]);
        for (int t = 0; t < 40; t++) {
            uint8_t in[24], out[24], bits[400];
            memset(in, 0, sizeof in);
            for (unsigned i = 0; i < info[ch]; i++) bits_put(in, i, (int)(trand() & 1u));
            nxdn_ch_encode((nxdn_ch_t)ch, in, bits);
            int m;
            CHECK_EQ(nxdn_ch_decode((nxdn_ch_t)ch, bits, out, &m), 0);
            CHECK_EQ(m, 0);
            CHECK(!memcmp(in, out, (info[ch] + 7) / 8));
            unsigned ne = (ch == NXDN_CH_FACCH1 || ch == NXDN_CH_LCAC || coded[ch] < 144) ? 1 : 2;   /* 3/4-punctured codes: d_free ~4 */
            for (unsigned e = 0; e < ne; e++) bits[e * (coded[ch] / ne) + trand() % (coded[ch] / ne / 2)] ^= 1u;
            CHECK_EQ(nxdn_ch_decode((nxdn_ch_t)ch, bits, out, &m), 0);
            CHECK(!memcmp(in, out, (info[ch] + 7) / 8));
        }
    }
    /* CRC detects garbage */
    uint8_t junk[300], out[24];
    for (int i = 0; i < 300; i++) junk[i] = (uint8_t)(trand() & 1u);
    CHECK_EQ(nxdn_ch_decode(NXDN_CH_CAC, junk, out, NULL), -1);
}

static void test_conventional_call(bool efr)
{
    nrec_t r;
    memset(&r, 0, sizeof r);
    nxdn_rx_t rx;
    nxdn_rx_cfg_t cfg = { 2, efr, false, 5 };
    nxdn_cb_t cb = { on_voice, on_event, &r };
    nxdn_rx_init(&rx, &cfg, &cb);

    uint8_t vcall[10], rel[10], sacch_msg[9], info[4], frame[NXDN_FRAME_DIBITS];
    nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
    nxdn_msg_build_vcall(rel, NXDN_MSG_TX_REL, NXDN_CALL_CONFERENCE, 1001, 300);
    memset(sacch_msg, 0, sizeof sacch_msg);
    memcpy(sacch_msg, vcall, 8);
    static const uint8_t ehr_tone[9] = { 0xCE, 0xA8, 0xFE, 0x83, 0xAC, 0xC4, 0x58, 0x20, 0x0A };  /* 7.2.1.1 */
    uint8_t sent[32][18];
    int ns = 0;

    s_n = 0;
    snoise(150);
    sput((const uint8_t[]){ 1, 1, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1 }, 12);   /* +3 +3 -3 -3 preamble filler */
    /* first frame: non-superframe SACCH + two FACCH1 VCALL (RDCH, SU -> CR) */
    nxdn_sacch_quarter(sacch_msg, 0, 5, false, info);
    nxdn_tx_voice_frame(0x40, info, vcall, vcall, NULL, efr, frame);
    sput(frame, sizeof frame);
    for (int k = 0; k < 8; k++) {
        uint8_t vch[36];
        for (int i = 0; i < 36; i++) vch[i] = (uint8_t)trand();
        if (k == 0 && !efr) memcpy(vch, ehr_tone, 9);
        int per = efr ? 2 : 4, sz = efr ? 18 : 9;
        for (int i = 0; i < per; i++) memcpy(sent[ns++], vch + sz * i, (size_t)sz);
        nxdn_sacch_quarter(sacch_msg, (unsigned)k % 4u, 5, true, info);
        nxdn_tx_voice_frame(0x56, info, NULL, NULL, vch, efr, frame);
        if (k == 3) { frame[60] ^= 1u; frame[150] ^= 3u; }   /* errors in a VCH (passed through) */
        if (k == 5) frame[25] ^= 1u;                         /* error in the SACCH: corrected */
        sput(frame, sizeof frame);
    }
    nxdn_sacch_quarter(sacch_msg, 0, 5, false, info);
    nxdn_tx_voice_frame(0x40, info, rel, rel, NULL, efr, frame);
    sput(frame, sizeof frame);
    snoise(600);
    nxdn_rx_dibits(&rx, s_stream, s_n);

    CHECK_EQ(r.ev[NXDN_EV_VOICE_START], 1);
    CHECK_EQ(r.last[NXDN_EV_VOICE_START].call.src, 1001);
    CHECK_EQ(r.last[NXDN_EV_VOICE_START].call.dst, 300);
    CHECK_EQ(r.last[NXDN_EV_VOICE_START].call.call_type, NXDN_CALL_CONFERENCE);
    CHECK_EQ(r.nframes, efr ? 16 : 32);
    int same = 0;
    for (int i = 0; i < r.nframes && i < ns; i++) same += !memcmp(r.frames[i], sent[i], efr ? 18 : 9);
    CHECK_EQ(same, (efr ? 16 : 32) - (efr ? 2 : 2));      /* frame k=3 carries two injected errors */
    if (!efr) CHECK(!memcmp(r.frames[0], ehr_tone, 9));
    CHECK_EQ(r.fbits[0], efr ? 144u : 72u);
    CHECK(r.ev[NXDN_EV_SACCH_MSG] >= 2);
    CHECK_EQ(bits_field(r.lastdata[NXDN_EV_SACCH_MSG], 0, 18), bits_field(sacch_msg, 0, 18));   /* last = single SACCH */
    CHECK_EQ(r.ev[NXDN_EV_FACCH1], 2);                    /* VCALL and TX_REL, duplicates removed */
    CHECK_EQ(r.ev[NXDN_EV_VOICE_END], 1);
    CHECK(r.ev[NXDN_EV_CRC_ERROR] <= 3);   /* trailing noise can pass the flywheel FSW check (<= 4 bit errors) */
}

static void test_type_d(void)
{
    nrec_t r;
    memset(&r, 0, sizeof r);
    nxdn_rx_t rx;
    nxdn_rx_cfg_t cfg = { 2, false, true, -1 };
    nxdn_cb_t cb = { on_voice, on_event, &r };
    nxdn_rx_init(&rx, &cfg, &cb);
    uint8_t scch[4] = { 0xC5, 0x12, 0x34, 0x80 }, vch[36], frame[NXDN_FRAME_DIBITS];   /* INFO1, 23 bits */
    for (int i = 0; i < 36; i++) vch[i] = (uint8_t)i;
    s_n = 0;
    snoise(77);
    for (int k = 0; k < 4; k++) {
        nxdn_tx_typed_voice_frame(0x77, scch, NULL, NULL, vch, frame);      /* RTCH2, superframe VCH, outbound */
        sput(frame, sizeof frame);
    }
    snoise(400);
    nxdn_rx_dibits(&rx, s_stream, s_n);
    CHECK(r.ev[NXDN_EV_SCCH] >= 4);        /* + rare noise frames passing CRC-7 */
    CHECK(!memcmp(r.lastdata[NXDN_EV_SCCH], scch, 4));
    CHECK_EQ(r.nframes, 16);
    CHECK(!memcmp(r.frames[1], vch + 9, 9));
}

/* ---------------- Type-C trunking ---------------- */
typedef struct { uint16_t ch; bool ctrl; int tunes; uint8_t tx[4][20]; int ntx; int ev[10]; nxt_event_t last[10]; } trec_t;
static void t_tune(void *u, uint16_t ch, bool c) { trec_t *r = u; r->ch = ch; r->ctrl = c; r->tunes++; }
static void t_tx(void *u, const uint8_t *m, unsigned n, bool s) { trec_t *r = u; (void)s; if (r->ntx < 4) memcpy(r->tx[r->ntx++], m, n); }
static void t_ev(void *u, const nxt_event_t *e) { trec_t *r = u; r->ev[e->type]++; r->last[e->type] = *e; }

static nxt_t *g_nxt;
static void trunk_rx_event(void *u, const nxdn_event_t *ev) { (void)u; nxt_on_rx(g_nxt, ev); }

static uint32_t tnow;
static void cac_out(nxdn_rx_t *rx, const uint8_t *msg, unsigned len, uint8_t ran)
{
    uint8_t info[19], frame[NXDN_FRAME_DIBITS];
    memset(info, 0, sizeof info);
    info[0] = (uint8_t)(0x80u | ran);          /* head of superframe, single message format */
    memcpy(info + 1, msg, len);
    nxdn_tx_cac_frame(0x01, info, frame);      /* RCCH, CAC, normal data, outbound */
    /* post field: last 9 symbols are the fixed pattern, not scrambled */
    uint8_t pb[24], pd[12];
    bits_arr_set(pb, 24, NXDN_PREAMBLE);
    bits_to_dibits(pd, pb, 12);
    CHECK(!memcmp(frame + 180, pd, 12));
    nxdn_rx_dibits(rx, frame, sizeof frame);
    tnow += 80;
    nxt_tick(g_nxt, tnow);
}

static void test_trunk(void)
{
    trec_t r;
    memset(&r, 0, sizeof r);
    nxt_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.unit_id = 1234;
    cfg.groups[0] = 500; cfg.n_groups = 1;
    cfg.hunt[0] = 101; cfg.hunt[1] = 202; cfg.n_hunt = 2;
    cfg.ran = 9;
    cfg.auto_register = true;
    nxt_cb_t cb = { t_tune, t_tx, t_ev, &r };
    nxt_t t;
    g_nxt = &t;
    nxt_init(&t, &cfg, &cb);
    tnow = 0;
    nxt_start(&t);
    CHECK_EQ(r.ch, 101);

    nxdn_rx_t rx;
    nxdn_rx_cfg_t rcfg = { 2, false, false, -1 };
    nxdn_cb_t rcb = { NULL, trunk_rx_event, NULL };
    nxdn_rx_init(&rx, &rcfg, &rcb);
    uint8_t noise[200];
    for (int i = 0; i < 200; i++) noise[i] = (uint8_t)(trand() & 3u);
    nxdn_rx_dibits(&rx, noise, 200);

    nxdn_site_info_t si;
    memset(&si, 0, sizeof si);
    si.loc = (0u << 22) | (77u << 12) | 9u;     /* global: system 77, site 9 -> RAN 9 */
    si.cc1 = 101; si.cc2 = 202; si.version = 1;
    uint8_t m[18];
    nxdn_msg_build_site_info(m, &si);
    nxdn_site_info_t back;
    nxdn_msg_parse_site_info(m, &back);
    CHECK_EQ(back.cc1, 101); CHECK_EQ(back.cc2, 202); CHECK_EQ(back.lid.system, 77); CHECK_EQ(back.lid.site, 9);
    cac_out(&rx, m, 18, 9);
    cac_out(&rx, m, 18, 9);
    CHECK_EQ(r.ev[NXT_EV_CAMPED], 1);
    for (int i = 0; i < 6; i++) cac_out(&rx, m, 18, 9);
    CHECK_EQ(r.ntx, 1);
    CHECK_EQ(r.tx[0][1] & 0x3F, NXDN_MSG_REG);       /* REG_REQ after SR octet */
    CHECK_EQ((r.tx[0][5] << 8) | r.tx[0][6], 1234);

    /* REG_RESP accepted -> GRP_REG_REQ */
    uint8_t resp[18];
    memset(resp, 0, sizeof resp);
    resp[0] = NXDN_MSG_REG; resp[4] = 1234 >> 8; resp[5] = 1234 & 0xFF; resp[6] = 500 >> 8; resp[7] = 500 & 0xFF;
    resp[8] = NXDN_CAUSE_REG_ACCEPTED;
    cac_out(&rx, resp, 18, 9);
    CHECK_EQ(r.ev[NXT_EV_REGISTERED], 1);
    for (int i = 0; i < 6; i++) cac_out(&rx, m, 18, 9);
    CHECK_EQ(r.ntx, 2);
    CHECK_EQ(r.tx[1][1] & 0x3F, NXDN_MSG_GRP_REG);

    /* other RAN is ignored */
    uint8_t as[18];
    memset(as, 0, sizeof as);
    nxdn_msg_build_assgn(as, NXDN_CALL_CONFERENCE, 777, 500, 345);
    nxdn_call_t c;
    nxdn_msg_parse_call(as, &c);
    CHECK_EQ(c.channel, 345); CHECK_EQ(c.dst, 500);
    cac_out(&rx, as, 18, 10);
    CHECK_EQ(r.ev[NXT_EV_GRANT], 0);
    cac_out(&rx, as, 18, 9);
    CHECK_EQ(r.ev[NXT_EV_GRANT], 1);
    CHECK_EQ(r.ch, 345); CHECK(!r.ctrl);
    /* traffic channel silent -> back to the control channel */
    for (int i = 0; i < 25; i++) { tnow += 80; nxt_tick(&t, tnow); }
    CHECK_EQ(r.ev[NXT_EV_RETURN], 1);
    CHECK_EQ(r.ch, 101); CHECK(r.ctrl);
    /* control channel lost -> hunt the next one */
    for (int i = 0; i < 32; i++) { tnow += 80; nxt_tick(&t, tnow); }
    CHECK(r.ev[NXT_EV_LOST_CC] >= 1);
    CHECK_EQ(r.ch, 202);
}

void test_nxdn(void)
{
    test_l1();
    test_conventional_call(false);
    test_conventional_call(true);
    test_type_d();
    test_trunk();
}
