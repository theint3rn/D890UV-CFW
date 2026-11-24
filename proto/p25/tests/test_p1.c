/* Phase 1 tests: IMBE FEC/interleave, frame builders vs. published bit
 * positions, and a full TX -> channel errors -> RX loop. */
#include "tst.h"
#include "p25_p1.h"
#include "p25_fec.h"
#include <string.h>

/* raw (status-included) bit positions of IMBE frame 0 / frame 4 code word
 * bits 0..47 in an LDU (TIA-102.BABA interleave; same values as OP25
 * op25_imbe_frame.h voice_codeword_bits) */
static const uint16_t raw_v0[48] = {
    114, 121, 126, 133, 138, 147, 152, 159, 164, 171, 176, 183, 188, 195, 200, 207, 212, 221, 226, 233, 238, 245, 250, 257,
    115, 120, 127, 132, 139, 146, 153, 158, 165, 170, 177, 182, 189, 194, 201, 206, 213, 220, 227, 232, 239, 244, 251, 256 };
static const uint16_t raw_v4[24] = {
    830, 837, 842, 849, 854, 861, 868, 875, 880, 887, 892, 899, 904, 911, 916, 923, 928, 937, 942, 949, 954, 961, 966, 973 };
/* raw positions of the LC hex-word info bits, first 3 segments (OP25 imbe_ldu_lcf_bits) */
static const uint16_t raw_lcf[72] = {
    410, 411, 412, 413, 414, 415, 420, 421, 422, 423, 424, 425, 432, 433, 434, 435, 436, 437, 442, 443, 444, 445, 446, 447,
    600, 601, 602, 603, 604, 605, 610, 611, 612, 613, 614, 615, 620, 621, 622, 623, 624, 625, 630, 631, 632, 633, 634, 635,
    788, 789, 792, 793, 794, 795, 800, 801, 802, 803, 804, 805, 810, 811, 812, 813, 814, 815, 820, 821, 822, 823, 824, 825 };

static unsigned raw_to_data(unsigned r)
{
    unsigned d = r / 2;
    return (d - (d + 1) / 36) * 2 + (r & 1);
}
static unsigned air_bit(const uint8_t *air, unsigned r) { return (air[r / 2] >> (1 - (r & 1))) & 1; }

static void t_interleave(void)
{
    int bad = 0;
    for (unsigned j = 0; j < 48; j++)
        if (112 + p25_imbe_il(j) != raw_to_data(raw_v0[j])) bad++;
    for (unsigned j = 0; j < 24; j++)
        if (808 + p25_imbe_il(j) != raw_to_data(raw_v4[j])) bad++;
    CHECK_EQ(bad, 0);
    uint8_t seen[144] = { 0 };
    for (unsigned j = 0; j < 144; j++) seen[p25_imbe_il(j)]++;
    for (unsigned j = 0; j < 144; j++) if (seen[j] != 1) bad++;
    CHECK_EQ(bad, 0);
}

static void rand_imbe(uint8_t f[11])
{
    for (int i = 0; i < 11; i++) f[i] = (uint8_t)rnd();
    f[10] &= 0xFE;                       /* 88 bits: last bit unused */
}

static void t_imbe(void)
{
    int bad = 0;
    for (int t = 0; t < 500; t++) {
        uint8_t f[11], g[11], air[18];
        rand_imbe(f);
        p25_imbe_encode(f, air);
        /* up to 3 errors in each Golay word, 1 in each Hamming word */
        static const uint8_t start[7] = { 0, 23, 46, 69, 92, 107, 122 };
        for (int w = 0; w < 7; w++) {
            int ne = w < 4 ? (int)rndn(4) : (int)rndn(2);
            unsigned used = 0;
            for (int e = 0; e < ne; e++) {
                unsigned b; do b = rndn(w < 4 ? 23 : 15); while (used & (1u << b));
                used |= 1u << b;
                unsigned pos = p25_imbe_il(start[w] + b);
                p25_bit_put(air, pos, !p25_bit_get(air, pos));
            }
        }
        p25_imbe_decode(air, g);
        if (memcmp(f, g, 11)) bad++;
    }
    CHECK_EQ(bad, 0);
}

static void t_positions(void)
{
    uint8_t air[P25_LDU_AIR];
    uint8_t imbe[9][11];
    memset(imbe, 0, sizeof imbe);
    p25_lc_t lc;
    memset(lc.b, 0xFF, 9);
    uint8_t lsd[2] = { 0xA5, 0x3C };
    CHECK_EQ(p25_p1_build_ldu1(air, 0x293, (const uint8_t(*)[11])imbe, &lc, lsd, P25_SS_BUSY), P25_LDU_AIR);
    int bad = 0;
    for (int i = 0; i < 72; i++) if (!air_bit(air, raw_lcf[i])) bad++;
    CHECK_EQ(bad, 0);
    uint32_t c = p25_cyclic16_encode(0xA5);
    CHECK_EQ(raw_to_data(1546), 1504);   /* OP25: LSD starts at raw bit 1546 */
    for (int i = 0; i < 16; i++) if (air_bit(air, 1546u + i) != ((c >> (15 - i)) & 1)) bad++;
    CHECK_EQ(bad, 0);
    /* status symbols at air dibits 35, 71, ... */
    for (int f = 35; f < P25_LDU_AIR; f += 36) if (air[f] != P25_SS_BUSY) bad++;
    CHECK_EQ(bad, 0);
    /* sync + NID */
    uint64_t s = 0;
    for (int i = 0; i < 24; i++) s = (s << 2) | air[i];
    CHECK_EQ(s, P25_SYNC_P1);
    uint64_t nid = 0;
    for (int i = 24; i < 57; i++) if (i != 35) nid = (nid << 2) | air[i];
    CHECK_EQ(nid, p25_nid_encode(0x293, P25_DUID_LDU1));
}

/* ------------------------------------------------------------ loop test */
typedef struct {
    int n_ev[16];
    uint8_t voice[64][11];
    int n_voice;
    p25_p1_event_t last[16];
    uint8_t tsbk[8][12];
    int n_tsbk;
    uint8_t pdu[2][256];
    size_t pdu_len[2];
    bool pdu_ok[2];
    int n_pdu;
} sink_t;

static void on_voice(void *ctx, const uint8_t f[11], int errs)
{
    sink_t *s = ctx;
    (void)errs;
    if (s->n_voice < 64) memcpy(s->voice[s->n_voice], f, 11);
    s->n_voice++;
}

static void on_event(void *ctx, const p25_p1_event_t *ev)
{
    sink_t *s = ctx;
    s->n_ev[ev->type]++;
    s->last[ev->type] = *ev;
    if (ev->type == P25_P1_EV_TSBK && ev->u.tsbk.crc_ok && s->n_tsbk < 8)
        memcpy(s->tsbk[s->n_tsbk++], ev->u.tsbk.tsbk, 12);
    if (ev->type == P25_P1_EV_PDU && s->n_pdu < 2) {
        s->pdu_ok[s->n_pdu] = ev->u.pdu.crc_ok;
        s->pdu_len[s->n_pdu] = ev->u.pdu.len;
        if (ev->u.pdu.data) memcpy(s->pdu[s->n_pdu], ev->u.pdu.data, ev->u.pdu.len);
        s->n_pdu++;
    }
}

static uint8_t stream[12000];

static void t_loop(int invert, int nerr)
{
    size_t n = 0;
    for (int i = 0; i < 37; i++) stream[n++] = (uint8_t)rndn(4);
    p25_hdu_t h;
    memset(&h, 0, sizeof h);
    h.algid = 0x80; h.tgid = 0x1234; h.mi[3] = 0x77;
    n += p25_p1_build_hdu(stream + n, 0x293, &h, P25_SS_UNKNOWN_TA);
    uint8_t imbe[27][11];
    for (int i = 0; i < 27; i++) rand_imbe(imbe[i]);
    p25_lc_t lc;
    p25_lc_group_voice(&lc, 0x00, 0x1234, 0x0ABCDE);
    uint8_t lsd[2] = { 0x11, 0x22 };
    n += p25_p1_build_ldu1(stream + n, 0x293, (const uint8_t(*)[11])(imbe + 0), &lc, lsd, 0);
    p25_es_t es;
    memset(&es, 0, sizeof es);
    es.algid = 0x80; es.kid = 0; es.mi[8] = 0x42;
    n += p25_p1_build_ldu2(stream + n, 0x293, (const uint8_t(*)[11])(imbe + 9), &es, lsd, 0);
    n += p25_p1_build_ldu1(stream + n, 0x293, (const uint8_t(*)[11])(imbe + 18), &lc, lsd, 0);
    n += p25_p1_build_tdu(stream + n, 0x293, 0);
    /* wrong NAC: must be ignored */
    n += p25_p1_build_ldu1(stream + n, 0x111, (const uint8_t(*)[11])(imbe + 0), &lc, lsd, 0);
    uint8_t tb[2][10] = { { 0x3D, 0, 1, 2, 3, 4, 5, 6, 7, 8 }, { 0x00, 0, 0x00, 0x10, 0x01, 0x12, 0x34, 0x0A, 0xBC, 0xDE } };
    n += p25_p1_build_tsbk(stream + n, 0x293, (const uint8_t(*)[10])tb, 2, P25_SS_IDLE);
    n += p25_p1_build_tdulc(stream + n, 0x293, &lc, 0);
    uint8_t data[200];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)(i * 7 + 3);
    p25_pdu_hdr_t ph;
    memset(&ph, 0, sizeof ph);
    ph.format = P25_PDU_FMT_UNCONF; ph.sap = 4; ph.llid = 0x00ABCD; ph.outbound = true;
    n += p25_p1_build_pdu(stream + n, 0x293, &ph, data, 41, 0);
    ph.format = P25_PDU_FMT_CONF;
    n += p25_p1_build_pdu(stream + n, 0x293, &ph, data, 70, 0);
    for (int i = 0; i < 40; i++) stream[n++] = (uint8_t)rndn(4);
    CHECK(n < sizeof stream);

    /* channel: random symbol errors (avoid frame syncs: they tolerate 4 bit errors anyway) */
    for (int e = 0; e < nerr; e++) stream[rndn((uint32_t)n)] ^= (uint8_t)(1 + rndn(3));
    if (invert) for (size_t i = 0; i < n; i++) stream[i] ^= 2;

    sink_t s;
    memset(&s, 0, sizeof s);
    p25_p1_cb_t cb = { on_voice, on_event };
    static p25_p1_rx_t rx;
    p25_p1_rx_init(&rx, 0x293, &cb, &s);
    /* feed in odd-sized chunks */
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rndn(50);
        if (i + k > n) k = n - i;
        p25_p1_rx_push(&rx, stream + i, k, -80);
        i += k;
    }
    CHECK_EQ(s.n_ev[P25_P1_EV_HDU], 1);
    CHECK_EQ(s.last[P25_P1_EV_HDU].u.hdu.tgid, 0x1234);
    CHECK_EQ(s.last[P25_P1_EV_HDU].u.hdu.mi[3], 0x77);
    CHECK_EQ(s.n_ev[P25_P1_EV_LDU1], 2);
    CHECK_EQ(s.n_ev[P25_P1_EV_LDU2], 1);
    CHECK(s.last[P25_P1_EV_LDU1].u.ldu1.lc_ok);
    CHECK(memcmp(s.last[P25_P1_EV_LDU1].u.ldu1.lc.b, lc.b, 9) == 0);
    CHECK_EQ(s.last[P25_P1_EV_LDU1].u.ldu1.lsd[1], 0x22);
    CHECK(s.last[P25_P1_EV_LDU2].u.ldu2.es_ok);
    CHECK_EQ(s.last[P25_P1_EV_LDU2].u.ldu2.es.mi[8], 0x42);
    CHECK_EQ(s.last[P25_P1_EV_LDU2].u.ldu2.es.algid, 0x80);
    CHECK_EQ(s.last[P25_P1_EV_LDU1].rssi, -80);
    CHECK_EQ(s.last[P25_P1_EV_LDU1].inverted, invert);
    CHECK_EQ(s.n_voice, 27);
    int vbad = 0;
    for (int i = 0; i < 27 && i < s.n_voice; i++) if (memcmp(s.voice[i], imbe[i], 11)) vbad++;
    CHECK(vbad <= (nerr ? 2 : 0));        /* heavy hits on an uncoded c7 bit are allowed */
    CHECK_EQ(s.n_ev[P25_P1_EV_TDU], 1);
    CHECK_EQ(s.n_tsbk, 2);
    CHECK_EQ(s.tsbk[0][0], 0x3D);
    CHECK_EQ(s.tsbk[1][0], 0x80);
    CHECK_EQ(s.tsbk[1][9], 0xDE);
    CHECK_EQ(s.last[P25_P1_EV_TSBK].status, P25_SS_IDLE);
    CHECK_EQ(s.n_ev[P25_P1_EV_TDULC], 1);
    CHECK(s.last[P25_P1_EV_TDULC].u.tdulc.lc_ok);
    p25_lc_info_t li;
    p25_lc_parse(&s.last[P25_P1_EV_TDULC].u.tdulc.lc, &li);
    CHECK(li.known && li.is_group);
    CHECK_EQ(li.tgid, 0x1234);
    CHECK_EQ(li.src, 0x0ABCDE);
    CHECK_EQ(s.n_pdu, 2);
    CHECK(s.pdu_ok[0]);
    CHECK_EQ(s.pdu_len[0], 41);
    CHECK(memcmp(s.pdu[0], data, 41) == 0);
    /* 3/4-rate confirmed data has little FEC margin: with channel errors only
     * require that a passing CRC-32 means correct data */
    if (!nerr || s.pdu_ok[1]) {
        CHECK(s.pdu_ok[1]);
        CHECK_EQ(s.pdu_len[1], 70);
        CHECK(memcmp(s.pdu[1], data, 70) == 0);
    }
    CHECK_EQ(s.last[P25_P1_EV_PDU].u.pdu.hdr.llid, 0x00ABCD);
}

static void t_voice_tx(void)
{
    static uint8_t air[P25_LDU_AIR * 4];
    p25_p1_voice_tx_t tx;
    p25_lc_t lc;
    p25_lc_group_voice(&lc, 0, 100, 1234);
    size_t n = p25_p1_voice_tx_start(&tx, air, 0x293, &lc, 0);
    CHECK_EQ(n, P25_HDU_AIR);
    uint8_t f[11] = { 1 };
    size_t total = 0;
    for (int i = 0; i < 20; i++) total += p25_p1_voice_tx_push(&tx, air, f);
    CHECK_EQ(total, 2 * P25_LDU_AIR);
    uint8_t sil[11] = { 0 };
    CHECK_EQ(p25_p1_voice_tx_end(&tx, air, sil), P25_LDU_AIR + P25_TDU_AIR);
}

void test_p1(void)
{
    t_interleave();
    t_imbe();
    t_positions();
    t_loop(0, 0);
    t_loop(1, 0);
    t_loop(0, 60);
    t_voice_tx();
}
