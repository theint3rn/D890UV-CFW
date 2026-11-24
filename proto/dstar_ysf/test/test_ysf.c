#include <string.h>

#include "../common/dv_common.h"
#include "../ysf/ysf.h"
#include "../ysf/ysf_c4fm.h"
#include "test.h"

typedef struct {
    int headers, terms, lost, fichs, fich_bad, data_ev, csd_ev, dch_bad;
    ysf_csd_t csd;
    uint8_t voice[64][18];
    unsigned vlen;
    int nvoice;
    ysf_voice_kind_t kind;
    uint8_t data[1100];
    unsigned data_len;
    uint8_t dgid;
} ycap_t;

static void y_voice(void *u, ysf_voice_kind_t k, const uint8_t *fr, unsigned idx, const ysf_fich_t *f)
{
    ycap_t *c = u;
    unsigned len = k == YSF_VOICE_VD1 ? 9 : k == YSF_VOICE_VD2 ? 7 : 18;
    (void)idx;
    c->kind = k;
    c->vlen = len;
    c->dgid = ysf_fich_dgid(f);
    if (c->nvoice < 64)
        memcpy(c->voice[c->nvoice], fr, len);
    c->nvoice++;
}

static void y_event(void *u, const ysf_event_t *ev)
{
    ycap_t *c = u;
    switch (ev->type) {
    case YSF_EV_HEADER: c->headers++; c->csd = ev->csd; break;
    case YSF_EV_TERMINATOR: c->terms++; break;
    case YSF_EV_LOST: c->lost++; break;
    case YSF_EV_FICH:
        c->fichs++;
        if (!ev->fich_ok)
            c->fich_bad++;
        break;
    case YSF_EV_CSD: c->csd_ev++; c->csd = ev->csd; break;
    case YSF_EV_DCH:
        if (!ev->dch.crc_ok)
            c->dch_bad++;
        break;
    case YSF_EV_DATA:
        c->data_ev++;
        if (c->data_len + ev->dch.len <= sizeof(c->data)) {
            memcpy(c->data + c->data_len, ev->dch.data, ev->dch.len);
            c->data_len += ev->dch.len;
        }
        break;
    default: break;
    }
}

static const ysf_rx_cb_t YCB = {y_voice, y_event};

static uint8_t g_dib[40 * 480];
static size_t g_nd;

static void voice_pattern(int i, uint8_t *v, unsigned len)
{
    for (unsigned k = 0; k < len; k++)
        v[k] = (uint8_t)(i * 13 + k * 5 + 3);
    if (len == 7)
        v[6] &= 0x80; /* 49 bits */
}

/* Send dibits through C4FM levels + gain/offset/noise + slicer into rx. */
static void through_slicer(ysf_rx_t *rx, const uint8_t *d, size_t n, float gain, float off, float noise)
{
    ysf_c4fm_slicer_t s;
    ysf_c4fm_slicer_init(&s);
    for (size_t i = 0; i < n; i++) {
        float lv = (float)ysf_dibit_to_level(d[i]);
        float nz = ((float)(rnd() % 2001) / 1000.0f - 1.0f) * noise;
        uint8_t o = ysf_c4fm_slice(&s, lv * gain + off + nz * gain, NULL);
        ysf_rx_push_dibit(rx, o);
    }
}

static void test_fec(void)
{
    /* Golay(24,12,8): transcription check of Appendix A, min distance 8 */
    unsigned minw = 24;
    for (unsigned d = 1; d < 4096; d++) {
        unsigned w = dv_popcount32(ysf_golay24_encode((uint16_t)d));
        if (w < minw)
            minw = w;
    }
    CHECK_EQ(minw, 8);
    CHECK_EQ(ysf_golay24_encode(0x800), 0x800C75u);
    int bad = 0;
    for (int t = 0; t < 3000; t++) {
        uint16_t d = (uint16_t)(rnd() & 0xFFF);
        uint32_t w = ysf_golay24_encode(d);
        int ne = (int)(rnd() % 4);
        uint32_t e = 0;
        while ((int)dv_popcount32(e) < ne)
            e |= 1u << (rnd() % 24);
        int got, n;
        got = ysf_golay24_decode(w ^ e, &n);
        if (got != d || n != ne)
            bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(ysf_golay24_decode(ysf_golay24_encode(0x123) ^ 0xF, NULL), -1); /* 4 errors detected */

    /* PN(9,5) whitening = the widely published 20-byte whitening table */
    static const uint8_t WH[20] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
                                   0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8};
    uint8_t z[160] = {0}, zb[20];
    ysf_whiten(z, 160);
    dv_pack_msb(z, 160, zb);
    CHECK(memcmp(zb, WH, 20) == 0);

    /* sync levels: D4 -> 11 01 01 00 -> -3 +3 +3 +1 */
    uint8_t sb[40], sd[20];
    for (int i = 0; i < 40; i++)
        sb[i] = (uint8_t)((YSF_SYNC >> (39 - i)) & 1u);
    ysf_bits_to_dibits(sb, 40, sd);
    CHECK(ysf_dibit_to_level(sd[0]) == -3 && ysf_dibit_to_level(sd[1]) == 3 &&
          ysf_dibit_to_level(sd[2]) == 3 && ysf_dibit_to_level(sd[3]) == 1);
    CHECK_EQ(ysf_level_to_dibit(2.6f), 1);
    CHECK_EQ(ysf_level_to_dibit(-0.7f), 2);
    CHECK(ysf_c4fm_deviation_hz(1, false) == 2700.0f && ysf_c4fm_deviation_hz(3, true) == -1350.0f);

    /* FICH */
    ysf_fich_t f = {0}, g;
    f.fi = YSF_FI_COMM; f.cs = 2; f.cm = 0; f.bn = 1; f.bt = 3; f.fn = 5; f.ft = 7;
    f.dev = 1; f.mr = 2; f.voip = 1; f.dt = YSF_DT_VD2; f.sql = 0; f.sc = 42;
    uint32_t v = ysf_fich_pack(&f);
    CHECK_EQ(v >> 30, 1);
    CHECK_EQ((v >> 8) & 3, 2);
    CHECK_EQ(v & 0x7F, 42);
    ysf_fich_unpack(v, &g);
    CHECK(memcmp(&f, &g, sizeof(f)) == 0);
    uint8_t fb[200];
    int errs;
    ysf_fich_encode(&f, fb);
    CHECK(ysf_fich_decode(fb, &g, &errs));
    CHECK(memcmp(&f, &g, sizeof(f)) == 0);
    CHECK_EQ(errs, 0);
    for (int i = 0; i < 8; i++)
        fb[i * 25 + 4] ^= 1;
    CHECK(ysf_fich_decode(fb, &g, &errs));
    CHECK(memcmp(&f, &g, sizeof(f)) == 0);
    CHECK_EQ(errs, 8);
    CHECK_EQ(ysf_fich_dgid(&g), 42);

    /* DCH 20 / 10 bytes */
    uint8_t d20[20], o20[20], u360[360], u200[200];
    for (int i = 0; i < 20; i++)
        d20[i] = (uint8_t)rnd();
    ysf_dch_encode(d20, 20, u360);
    CHECK(ysf_dch_decode(u360, 20, o20, &errs));
    CHECK(memcmp(d20, o20, 20) == 0);
    for (int i = 0; i < 10; i++)
        u360[i * 36 + 1] ^= 1;
    CHECK(ysf_dch_decode(u360, 20, o20, &errs));
    CHECK(memcmp(d20, o20, 20) == 0);
    CHECK_EQ(errs, 10);
    ysf_dch_encode(d20, 10, u200);
    CHECK(ysf_dch_decode(u200, 10, o20, NULL));
    CHECK(memcmp(d20, o20, 10) == 0);
    u200[3] ^= 1; u200[4] ^= 1; u200[5] ^= 1; u200[6] ^= 1; /* burst of 4 air bits */
    CHECK(ysf_dch_decode(u200, 10, o20, NULL));
    CHECK(memcmp(d20, o20, 10) == 0);

    /* V/D 2 voice: one error in every triplet is out-voted */
    uint8_t vv[7], vo[7], vb[104];
    voice_pattern(7, vv, 7);
    ysf_vd2_voice_encode(vv, vb);
    CHECK_EQ(ysf_vd2_voice_decode(vb, vo), 0);
    CHECK(memcmp(vv, vo, 7) == 0);
    for (unsigned t = 0; t < 27; t++) {
        unsigned src = 3 * t + (t % 3); /* code bit index, invert air position */
        unsigned k = (src % 26u) * 4u + src / 26u;
        vb[k] ^= 1;
    }
    CHECK_EQ(ysf_vd2_voice_decode(vb, vo), 27);
    CHECK(memcmp(vv, vo, 7) == 0);
}

static void run_voice_session(uint8_t dt, unsigned nframes, const char *text, float noise)
{
    ysf_csd_t csd;
    ysf_tx_t tx;
    ysf_rx_t rx;
    ycap_t c;
    unsigned vlen = dt == YSF_DT_VD1 ? 9 : dt == YSF_DT_VD2 ? 7 : 18;
    uint8_t frames[5 * 18];

    ysf_csd_init(&csd, "W1ABC", NULL);
    ysf_field_set(csd.down, 10, "W1RPT");
    memcpy(csd.rem[3], "12345", 5);
    ysf_tx_init(&tx, dt, &csd, 17);
    if (text)
        ysf_tx_set_data(&tx, (const uint8_t *)text, (unsigned)strlen(text));

    g_nd = 0;
    for (int i = 0; i < 77; i++)
        g_dib[g_nd++] = (uint8_t)(rnd() & 3u);
    ysf_tx_header(&tx, g_dib + g_nd);
    g_nd += 480;
    int vi = 0;
    for (unsigned f = 0; f < nframes; f++) {
        for (int i = 0; i < 5; i++)
            voice_pattern(vi + i, frames + vlen * (unsigned)i, vlen);
        vi += 5;
        ysf_tx_voice(&tx, frames, g_dib + g_nd);
        g_nd += 480;
    }
    ysf_tx_terminator(&tx, g_dib + g_nd);
    g_nd += 480;
    for (int i = 0; i < 50; i++)
        g_dib[g_nd++] = (uint8_t)(rnd() & 3u);

    memset(&c, 0, sizeof(c));
    ysf_rx_init(&rx, &YCB, &c);
    through_slicer(&rx, g_dib, g_nd, 0.37f, -0.2f, noise);

    CHECK_EQ(c.headers, 1);
    CHECK_EQ(c.terms, 1);
    CHECK_EQ(c.lost, 0);
    CHECK_EQ(c.fich_bad, 0);
    CHECK_EQ(c.dch_bad, 0);
    CHECK_EQ(c.dgid, 17);
    CHECK(memcmp(c.csd.src, "W1ABC     ", 10) == 0);
    CHECK(memcmp(c.csd.dest, "ALL       ", 10) == 0);
    CHECK(memcmp(c.csd.down, "W1RPT     ", 10) == 0);
    unsigned expect = dt == YSF_DT_VOICE_FR ? 5 * nframes - 3 : 5 * nframes;
    CHECK_EQ(c.nvoice, expect);
    /* compare voice frames (Voice FR CC0 drops frames 0..2) */
    int vok = 1, skip = dt == YSF_DT_VOICE_FR ? 3 : 0;
    for (int i = 0; i < c.nvoice && i < 64; i++) {
        uint8_t ref[18];
        voice_pattern(i + skip, ref, vlen);
        if (memcmp(ref, c.voice[i], vlen) != 0)
            vok = 0;
    }
    CHECK(vok);
    if (dt != YSF_DT_VOICE_FR || nframes > 0)
        CHECK(memcmp(c.csd.rem[3], "12345", 5) == 0);
    if (text) {
        CHECK(c.data_ev >= 1);
        CHECK(memcmp(c.data, text, strlen(text)) == 0);
    }
    printf("  dt %u: %d voice frames, %d csd events, %d data events\n", dt, c.nvoice, c.csd_ev, c.data_ev);
}

static void test_data_fr(void)
{
    ysf_csd_t csd;
    ysf_tx_t tx;
    ysf_rx_t rx;
    ycap_t c;
    static uint8_t data[600];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)rnd();
    ysf_csd_init(&csd, "W1ABC", "W1XYZ");
    ysf_tx_init(&tx, YSF_DT_DATA_FR, &csd, 0);
    unsigned nf = ysf_tx_data_fr_frames(sizeof(data));
    CHECK_EQ(ysf_tx_data_fr_frames(20), 2);  /* Table 4-5: FT=1 */
    CHECK_EQ(ysf_tx_data_fr_frames(60), 3);  /* FT=2 */
    CHECK_EQ(ysf_tx_data_fr_frames(260), 8); /* FT=7 */
    CHECK_EQ(nf, 8 + 8 + 4);                 /* 260 + 260 + 80 */
    g_nd = 0;
    ysf_tx_header(&tx, g_dib);
    g_nd += 480;
    for (unsigned k = 0; k < nf; k++) {
        ysf_tx_data_fr(&tx, data, sizeof(data), k, g_dib + g_nd);
        g_nd += 480;
    }
    ysf_tx_terminator(&tx, g_dib + g_nd);
    g_nd += 480;
    memset(&c, 0, sizeof(c));
    ysf_rx_init(&rx, &YCB, &c);
    ysf_rx_push_dibits(&rx, g_dib, g_nd);
    CHECK_EQ(c.data_ev, 3);
    CHECK(c.data_len >= sizeof(data));
    /* blocks are 260 bytes of DT each */
    CHECK(memcmp(c.data, data, 260) == 0);
    CHECK(memcmp(c.data + 260, data + 260, 260) == 0);
    CHECK(memcmp(c.data + 520, data + 520, 80) == 0);
    CHECK(memcmp(c.csd.dest, "W1XYZ     ", 10) == 0);
    CHECK_EQ(c.terms, 1);
}

static void test_lost(void)
{
    ysf_csd_t csd;
    ysf_tx_t tx;
    ysf_rx_t rx;
    ycap_t c;
    uint8_t frames[35] = {0};
    ysf_csd_init(&csd, "W1ABC", NULL);
    ysf_tx_init(&tx, YSF_DT_VD2, &csd, 0);
    g_nd = 0;
    ysf_tx_header(&tx, g_dib);
    g_nd += 480;
    for (int i = 0; i < 3; i++) {
        ysf_tx_voice(&tx, frames, g_dib + g_nd);
        g_nd += 480;
    }
    for (int i = 0; i < 480 * 6; i++)
        g_dib[g_nd++] = (uint8_t)(rnd() & 3u);
    memset(&c, 0, sizeof(c));
    ysf_rx_init(&rx, &YCB, &c);
    ysf_rx_push_dibits(&rx, g_dib, g_nd);
    CHECK_EQ(c.lost, 1);
    CHECK_EQ(c.nvoice, 15 + 5 * 3); /* 3 flywheel frames of noise before giving up */
}

/* Interop facts cross-checked with MMDVMHost YSFFICH/YSFPayload/Golay24128
 * (tables used as test vectors only; our code computes them). */
static void test_facts(void)
{
    /* Golay(24,12) codewords, data in bits 23..12 */
    CHECK_EQ(ysf_golay24_encode(0x001), 0x0018EB);
    CHECK_EQ(ysf_golay24_encode(0x002), 0x00293E);
    CHECK_EQ(ysf_golay24_encode(0x003), 0x0031D5);
    CHECK_EQ(ysf_golay24_encode(0x004), 0x004A97);
    CHECK_EQ(ysf_golay24_encode(0x010), 0x010367);
    CHECK_EQ(ysf_golay24_encode(0x01D), 0x01DCDD);

    /* FICH byte layout: b0 FI CS CM BN, b1 BT FN FT, b2 rsv Dev MR VoIP DT, b3 SQL DG-ID */
    ysf_fich_t f = {0};
    f.dev = 1;
    CHECK_EQ(ysf_fich_pack(&f), 0x00004000u); /* byte2 0x40 */
    memset(&f, 0, sizeof(f));
    f.voip = 1;
    CHECK_EQ(ysf_fich_pack(&f), 0x00000400u); /* byte2 0x04 */
    memset(&f, 0, sizeof(f));
    f.mr = 2;
    CHECK_EQ(ysf_fich_pack(&f), 0x00001000u); /* byte2 bits 5..3 */
    memset(&f, 0, sizeof(f));
    f.fn = 5; f.ft = 3; f.bt = 2;
    CHECK_EQ(ysf_fich_pack(&f) >> 16 & 0xFF, (2u << 6) | (5u << 3) | 3u);
    memset(&f, 0, sizeof(f));
    f.fi = 2; f.bn = 1;
    CHECK_EQ(ysf_fich_pack(&f) >> 24, 0x81);
    memset(&f, 0, sizeof(f));
    ysf_fich_set_dgid(&f, 99);
    CHECK_EQ(ysf_fich_pack(&f), 99u); /* byte3 & 0x7F */

    /* dibit interleave: code dibit i -> air bit (i % M) * 40 + (i / M) * 2 */
    static uint8_t in[360], out[360];
    int ok5 = 1, ok9 = 1;
    for (unsigned i = 0; i < 100; i++) {
        memset(in, 0, 200);
        in[2 * i] = 1;
        ysf_interleave_dibits(in, out, 5, 20, 0);
        if (!out[(i % 5) * 40 + (i / 5) * 2])
            ok5 = 0;
    }
    for (unsigned i = 0; i < 180; i++) {
        memset(in, 0, 360);
        in[2 * i + 1] = 1;
        ysf_interleave_dibits(in, out, 9, 20, 0);
        if (!out[(i % 9) * 40 + (i / 9) * 2 + 1])
            ok9 = 0;
    }
    CHECK(ok5);
    CHECK(ok9);

    /* V/D 2 voice, independent reference: 27 bits x3 + 22 + 0, XOR with the
     * first 13 whitening bytes (MSB first), code bit i -> air (i%26)*4 + i/26 */
    static const uint8_t WH[13] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F, 0xF8, 0x3D, 0xF1};
    uint8_t v[7] = {0xA5, 0x3C, 0x0F, 0xF0, 0x5A, 0xC3, 0x80}, bits[49], a[104], ref[104], got[104];
    dv_unpack_msb(v, 49, bits);
    for (int i = 0; i < 27; i++)
        a[3 * i] = a[3 * i + 1] = a[3 * i + 2] = bits[i];
    memcpy(a + 81, bits + 27, 22);
    a[103] = 0;
    for (int i = 0; i < 104; i++)
        a[i] ^= (uint8_t)((WH[i / 8] >> (7 - i % 8)) & 1u);
    for (unsigned i = 0; i < 104; i++)
        ref[(i % 26) * 4 + i / 26] = a[i];
    ysf_vd2_voice_encode(v, got);
    CHECK(memcmp(ref, got, 104) == 0);
}

void test_ysf(void)
{
    test_facts();
    test_fec();
    run_voice_session(YSF_DT_VD2, 12, "Hello YSF DN", 0.3f);
    run_voice_session(YSF_DT_VD1, 10, "V/D mode 1 text data that is longer than twenty bytes", 0.3f);
    run_voice_session(YSF_DT_VOICE_FR, 6, NULL, 0.3f);
    run_voice_session(YSF_DT_VD2, 8, NULL, 0.0f);
    test_data_fr();
    test_lost();
}
