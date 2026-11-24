/* DMR layer tests: burst layout against annex E, sync table properties, full TX -> dibits -> RX
 * runs for a BS repeater channel (CACH, two slots), direct mode late entry, packet data. */
#include "test.h"
#include "../dmr/dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"

typedef struct {
    int n_ev[16];
    dmr_event_t last[16];
    uint8_t frames[200][9];
    uint8_t fslot[200];
    int nframes;
    uint8_t data[512];
    int data_len;
    int crypt[3];
} rec_t;

static void on_voice(void *u, uint8_t slot, const uint8_t f[9], uint8_t idx, uint8_t crypt)
{
    rec_t *r = u;
    (void)idx;
    r->crypt[crypt < 3 ? crypt : 0]++;
    if (r->nframes < 200) { memcpy(r->frames[r->nframes], f, 9); r->fslot[r->nframes++] = slot; }
}

static void on_event(void *u, const dmr_event_t *ev)
{
    rec_t *r = u;
    r->n_ev[ev->type]++;
    r->last[ev->type] = *ev;
    if (ev->type == DMR_EV_DATA_PACKET && ev->data && ev->len <= sizeof r->data) {
        memcpy(r->data, ev->data, ev->len);
        r->data_len = ev->len;
        r->last[ev->type].data = r->data;
    }
}

/* dibit stream builder */
static uint8_t g_stream[40000];
static unsigned g_n;
static void put(const uint8_t *d, unsigned n) { memcpy(g_stream + g_n, d, n); g_n += n; }
static void noise(unsigned n) { for (unsigned i = 0; i < n; i++) g_stream[g_n++] = (uint8_t)(trand() & 3u); }

static void test_layout(void)
{
    /* table 9.2 note: data sync = symbol-wise complement of voice sync (flip each dibit's first bit) */
    CHECK_EQ(DMR_SYNC_PATTERN[DMR_SYNC_BS_VOICE] ^ 0xAAAAAAAAAAAAull, DMR_SYNC_PATTERN[DMR_SYNC_BS_DATA]);
    CHECK_EQ(DMR_SYNC_PATTERN[DMR_SYNC_MS_VOICE] ^ 0xAAAAAAAAAAAAull, DMR_SYNC_PATTERN[DMR_SYNC_MS_DATA]);
    CHECK_EQ(DMR_SYNC_PATTERN[DMR_SYNC_TS1_VOICE] ^ 0xAAAAAAAAAAAAull, DMR_SYNC_PATTERN[DMR_SYNC_TS1_DATA]);
    CHECK_EQ(DMR_SYNC_PATTERN[DMR_SYNC_TS2_VOICE] ^ 0xAAAAAAAAAAAAull, DMR_SYNC_PATTERN[DMR_SYNC_TS2_DATA]);
    for (int s = 1; s < DMR_SYNC_COUNT; s++) {
        int err;
        uint64_t w = DMR_SYNC_PATTERN[s] ^ 0x800000000101ull;     /* 3 bit errors */
        CHECK_EQ(dmr_sync_match(w, 4, &err), s);
        CHECK_EQ(err, 3);
    }

    /* table E.1: L17..L13 = CC(3..0) DT(3..0) Golay(11,10); L12..R12 sync; R13 = Golay(9) ... */
    uint8_t bits[196], burst[DMR_BURST_DIBITS], cc, dt;
    memset(bits, 0, sizeof bits);
    dmr_burst_build_data(burst, DMR_SYNC_BS_DATA, 0xA, 0x3, bits);
    CHECK_EQ(burst[49], 2);      /* CC = 1010: L17 = CC3 CC2 = 10 */
    CHECK_EQ(burst[50], 2);      /* L16 = CC1 CC0 = 10 */
    CHECK_EQ(burst[51], 0);      /* DT 0011: L15 = 00 */
    CHECK_EQ(burst[52], 3);      /* L14 = 11 */
    uint32_t st = fec_golay2087_enc(0xA3);
    CHECK_EQ(burst[53], (st >> 10) & 3u);   /* Golay(11..10) */
    CHECK_EQ(burst[78], (st >> 8) & 3u);    /* R13 = Golay(9..8) */
    CHECK_EQ(dmr_burst_sync(burst), DMR_SYNC_PATTERN[DMR_SYNC_BS_DATA]);
    CHECK_EQ(dmr_burst_slot_type(burst, &cc, &dt), 0);
    CHECK_EQ(cc, 0xA); CHECK_EQ(dt, 3);

    /* table E.6: L12 = CC3 CC2, L11 = CC1 CC0, L10 = PI LCSS1, L9 = LCSS0 QR8, L8 = LC(71) LC(60) */
    uint8_t fr[27], emb[32];
    memset(fr, 0, sizeof fr);
    memset(emb, 0, sizeof emb);
    emb[0] = 1;
    dmr_burst_build_voice_emb(burst, 0x5, 1, DMR_LCSS_FIRST, emb, fr);
    CHECK_EQ(burst[54], 1); CHECK_EQ(burst[55], 1); CHECK_EQ(burst[56], 2);
    CHECK_EQ(burst[58] >> 1, 1);
    uint8_t c2, pi, lcss, e2[32];
    CHECK_EQ(dmr_burst_emb(burst, &c2, &pi, &lcss, e2), 0);
    CHECK_EQ(c2, 5); CHECK_EQ(pi, 1); CHECK_EQ(lcss, DMR_LCSS_FIRST);

    /* E.5: VS(215) is the first bit of the burst */
    memset(fr, 0, sizeof fr);
    fr[0] = 0x80;
    dmr_burst_build_voice_sync(burst, DMR_SYNC_BS_VOICE, fr);
    CHECK_EQ(burst[0], 2);
    fr[0] = 0; fr[26] = 1;   /* VS(0) last bit */
    dmr_burst_build_voice_sync(burst, DMR_SYNC_BS_VOICE, fr);
    CHECK_EQ(burst[131], 1);

    /* CACH: AT TC LCSS round trip with one bit error */
    dmr_cach_t c = { 1, 1, DMR_LCSS_LAST, { 0 } }, o;
    for (int i = 0; i < 17; i++) c.payload[i] = (uint8_t)(i & 1);
    uint8_t cd[12];
    dmr_cach_encode(&c, cd);
    CHECK_EQ(cd[0] >> 1, 1);         /* TX(23) = AT */
    cd[7] ^= 1u;
    CHECK_EQ(dmr_cach_decode(cd, &o), 0);
    CHECK_EQ(o.at, 1); CHECK_EQ(o.tc, 1); CHECK_EQ(o.lcss, DMR_LCSS_LAST);
}

static void slot_out(const uint8_t cach[12], const uint8_t burst[132]) { put(cach, 12); put(burst, 132); }

static void test_bs_call(void)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, 1, true };
    dmr_cb_t cb = { on_voice, on_event, &r };
    dmr_rx_init(&rx, &cfg, &cb);

    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, 91, 9990201, 0);
    dmr_slc_t slc = { DMR_SLCO_C_SYS_PARMS, 0x4A5B6C };
    dmr_csbk_t csbk;
    uint8_t cdata[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    dmr_csbk_set(&csbk, 0x3D, 0, cdata);

    uint8_t sent[36][9];
    int nsent = 0;
    g_n = 0;
    noise(333);
    dmr_voice_tx_t vt;
    dmr_voice_tx_init(&vt, &lc, 1, DMR_SYNC_BS_VOICE);
    uint8_t burst[132], cach[12];
    /* slot 0: 2 headers, 12 voice bursts, terminator. slot 1: idle, one CSBK */
    for (int k = 0; k < 2 * 18; k++) {
        int slot = k & 1, i = k >> 1;
        dmr_tx_cach_slc(&slc, (unsigned)k % 4u, 1, (uint8_t)slot, cach);
        if (slot == 0) {
            if (i < 2) dmr_tx_voice_header(&lc, 1, DMR_SYNC_BS_DATA, burst);
            else if (i < 14) {
                uint8_t fr[27];
                for (int f = 0; f < 3; f++) { for (int b = 0; b < 9; b++) fr[9 * f + b] = (uint8_t)trand(); memcpy(sent[nsent++], fr + 9 * f, 9); }
                dmr_voice_tx_burst(&vt, fr, burst);
                if (i == 5) burst[100] ^= 1u;            /* a bit error in the voice payload */
            } else if (i == 14) dmr_tx_terminator(&lc, 1, DMR_SYNC_BS_DATA, burst);
            else dmr_tx_idle(1, DMR_SYNC_BS_DATA, burst);
        } else {
            if (i == 4) dmr_tx_csbk(&csbk, 1, DMR_SYNC_BS_DATA, burst);
            else dmr_tx_idle(1, DMR_SYNC_BS_DATA, burst);
            if (i == 6) burst[10] ^= 3u;                 /* error inside a BPTC burst */
        }
        slot_out(cach, burst);
    }
    noise(500);
    dmr_rx_dibits(&rx, g_stream, g_n);

    CHECK_EQ(r.n_ev[DMR_EV_VOICE_START], 1);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].lc.dst, 91);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].lc.src, 9990201);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].slot, 0);
    CHECK_EQ(r.nframes, 36);
    int same = 0;
    for (int f = 0; f < 36 && f < r.nframes; f++) same += !memcmp(r.frames[f], sent[f], 9);
    CHECK_EQ(same, 35);             /* frame 11 (dibit 100 = VS(63)) carries the injected bit error: frames are opaque */
    CHECK(memcmp(r.frames[11], sent[11], 9) != 0);
    CHECK(r.n_ev[DMR_EV_VOICE_LC_EMB] >= 1);
    CHECK_EQ(r.last[DMR_EV_VOICE_LC_EMB].lc.src, 9990201);
    CHECK_EQ(r.n_ev[DMR_EV_VOICE_END], 1);
    CHECK_EQ(r.n_ev[DMR_EV_CSBK], 1);
    CHECK_EQ(r.last[DMR_EV_CSBK].csbk.opcode, 0x3D);
    CHECK(!memcmp(r.last[DMR_EV_CSBK].csbk.data, cdata, 8));
    CHECK_EQ(r.last[DMR_EV_CSBK].slot, 1);
    CHECK(r.n_ev[DMR_EV_SHORT_LC] >= 7);
    CHECK_EQ(r.last[DMR_EV_SHORT_LC].slc.slco, DMR_SLCO_C_SYS_PARMS);
    CHECK_EQ(r.last[DMR_EV_SHORT_LC].slc.data24, 0x4A5B6C);
    CHECK(r.n_ev[DMR_EV_IDLE] >= 15);
}

static void test_direct_late_entry(void)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, -1, false };
    dmr_cb_t cb = { on_voice, on_event, &r };
    dmr_rx_init(&rx, &cfg, &cb);
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, false, 1234567, 7654321, 0x20);
    dmr_voice_tx_t vt;
    dmr_voice_tx_init(&vt, &lc, 7, DMR_SYNC_TS2_VOICE);
    uint8_t burst[132], fr[27];
    g_n = 0;
    noise(1000);
    for (int i = 0; i < 18; i++) {          /* three superframes, no header: late entry */
        for (int b = 0; b < 27; b++) fr[b] = (uint8_t)trand();
        dmr_voice_tx_burst(&vt, fr, burst);
        noise(12);
        put(burst, 132);
        noise(144);                         /* the other timeslot is silent / noise */
    }
    dmr_tx_terminator(&lc, 7, DMR_SYNC_TS2_DATA, burst);
    noise(12); put(burst, 132); noise(144 * 20);
    dmr_rx_dibits(&rx, g_stream, g_n);
    CHECK_EQ(r.n_ev[DMR_EV_VOICE_START], 1);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].lc.flco, DMR_FLCO_UNIT_VOICE);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].lc.dst, 1234567);
    CHECK_EQ(r.last[DMR_EV_VOICE_START].slot, 1);
    CHECK_EQ(r.nframes, 54);
    CHECK_EQ(r.fslot[0], 1);
    CHECK_EQ(r.n_ev[DMR_EV_VOICE_END], 1);
    CHECK_EQ(r.last[DMR_EV_VOICE_END].lc.svc_opts, 0x20);
    CHECK(r.n_ev[DMR_EV_SYNC_LOST] >= 1);
}

static void test_data(void)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    dmr_rx_t rx;
    dmr_rx_cfg_t cfg = { 4, -1, false };
    dmr_cb_t cb = { on_voice, on_event, &r };
    dmr_rx_init(&rx, &cfg, &cb);
    static uint8_t bursts[8][132];
    const char *msg = "Hello DMR world, unconfirmed packet data!";
    size_t len = strlen(msg);
    int nb = dmr_tx_data_unconfirmed(true, 9, 9990201, 4, (const uint8_t *)msg, len, NULL, 3, DMR_SYNC_MS_DATA, bursts, 8);
    CHECK_EQ(nb, 5);
    g_n = 0;
    noise(200);
    for (int i = 0; i < nb; i++) { noise(12); put(bursts[i], 132); noise(144); }
    noise(300);
    dmr_rx_dibits(&rx, g_stream, g_n);
    CHECK_EQ(r.n_ev[DMR_EV_DATA_HEADER], 1);
    CHECK_EQ(r.last[DMR_EV_DATA_HEADER].dh.dpf, DMR_DPF_UNCONFIRMED);
    CHECK_EQ(r.last[DMR_EV_DATA_HEADER].dh.bf, 4);
    CHECK_EQ(r.last[DMR_EV_DATA_HEADER].dh.src, 9990201);
    CHECK_EQ(r.last[DMR_EV_DATA_HEADER].dh.sap, 4);
    CHECK_EQ(r.n_ev[DMR_EV_DATA_PACKET], 1);
    CHECK_EQ(r.last[DMR_EV_DATA_PACKET].crc_ok, 1);
    CHECK(!memcmp(r.data, msg, len));

    /* confirmed rate 3/4: 2 blocks (16 + 12 user octets + CRC-32), CRC-9 per block */
    memset(&r, 0, sizeof r);
    dmr_rx_init(&rx, &cfg, &cb);
    dmr_data_hdr_t h;
    dmr_data_header_build_unconfirmed(&h, false, 100, 200, 4, 2, 0);
    h.raw[0] = (uint8_t)((h.raw[0] & 0xF0u) | DMR_DPF_CONFIRMED);
    uint8_t bits[196], user[32];
    for (int i = 0; i < 28; i++) user[i] = (uint8_t)(0x30 + i);
    uint32_t crc = crc32_dmr(user, 28);
    user[28] = (uint8_t)crc; user[29] = (uint8_t)(crc >> 8); user[30] = (uint8_t)(crc >> 16); user[31] = (uint8_t)(crc >> 24);
    g_n = 0;
    noise(100);
    dmr_data_header_encode(&h, bits);
    uint8_t burst[132];
    dmr_burst_build_data(burst, DMR_SYNC_MS_DATA, 2, DMR_DT_DATA_HEADER, bits);
    noise(12); put(burst, 132); noise(144);
    for (int b = 0; b < 2; b++) {
        uint8_t blk[18], d98[98];
        memcpy(blk + 2, user + 16 * b, 16);
        uint16_t c9 = (uint16_t)(crc9_dmr(blk + 2, 16, (uint8_t)b) ^ DMR_MASK_R34_CRC9);
        blk[0] = (uint8_t)((b << 1) | (c9 >> 8));
        blk[1] = (uint8_t)c9;
        trellis34_encode(blk, d98);
        dibits_to_bits(bits, d98, 98);
        dmr_burst_build_data(burst, DMR_SYNC_MS_DATA, 2, DMR_DT_RATE_34, bits);
        if (b == 1) burst[40] = (uint8_t)(burst[40] == 1 ? 0 : 1);   /* adjacent-level symbol error */
        noise(12); put(burst, 132); noise(144);
    }
    noise(300);
    dmr_rx_dibits(&rx, g_stream, g_n);
    CHECK_EQ(r.n_ev[DMR_EV_DATA_PACKET], 1);
    CHECK_EQ(r.last[DMR_EV_DATA_PACKET].crc_ok, 1);
    CHECK_EQ(r.data_len, 32);
    CHECK(!memcmp(r.data, user, 32));
}

void test_dmr(void)
{
    test_layout();
    test_bs_call();
    test_direct_late_entry();
    test_data();
}
