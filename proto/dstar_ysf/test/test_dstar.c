#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../common/dv_common.h"
#include "../dstar/dstar.h"
#include "../dstar/dstar_gmsk.h"
#include "test.h"

/* ---- capture of RX callbacks ---- */
typedef struct {
    int headers, header_ok, header_slow, late, ends, lost, texts, lines, csq, fast_blocks;
    dstar_header_t hdr;
    char text[32];
    char line[128];
    uint8_t voice[64][9];
    uint8_t seqs[64];
    int nvoice;
    uint8_t fast[1024];
    size_t fast_len;
    uint8_t csqv[2];
} cap_t;

static void cb_voice(void *u, const uint8_t ambe[9], uint8_t seq)
{
    cap_t *c = u;
    if (c->nvoice < 64) {
        memcpy(c->voice[c->nvoice], ambe, 9);
        c->seqs[c->nvoice] = seq;
    }
    c->nvoice++;
}

static void cb_event(void *u, const dstar_event_t *ev)
{
    cap_t *c = u;
    switch (ev->type) {
    case DSTAR_EV_HEADER:
        c->headers++;
        if (ev->u.header.crc_ok) {
            c->header_ok++;
            c->hdr = ev->u.header.hdr;
        }
        if (ev->u.header.from_slow_data)
            c->header_slow++;
        break;
    case DSTAR_EV_LATE_ENTRY: c->late++; break;
    case DSTAR_EV_END: c->ends++; break;
    case DSTAR_EV_LOST: c->lost++; break;
    case DSTAR_EV_TEXT:
        c->texts++;
        strcpy(c->text, ev->u.text);
        break;
    case DSTAR_EV_GPS_LINE:
        c->lines++;
        memcpy(c->line, ev->u.data.data, ev->u.data.len);
        c->line[ev->u.data.len] = '\0';
        break;
    case DSTAR_EV_CSQ:
        c->csq++;
        memcpy(c->csqv, ev->u.csq, 2);
        break;
    case DSTAR_EV_FAST_DATA:
        c->fast_blocks++;
        if (c->fast_len + ev->u.data.len <= sizeof(c->fast)) {
            memcpy(c->fast + c->fast_len, ev->u.data.data, ev->u.data.len);
            c->fast_len += ev->u.data.len;
        }
        break;
    default: break;
    }
}

static const dstar_rx_cb_t CB = {cb_voice, cb_event};

/* stream buffer */
static uint8_t g_bits[20000];
static size_t g_n;

static void make_ambe(int i, uint8_t a[9])
{
    for (int k = 0; k < 9; k++)
        a[k] = (uint8_t)(i * 31 + k * 7 + 1);
}

static void build_stream(dstar_tx_t *tx, int nframes, int idle_prefix, bool fast)
{
    g_n = 0;
    for (int i = 0; i < idle_prefix; i++)
        g_bits[g_n++] = (uint8_t)(rnd() & 1u);
    g_n += dstar_tx_header(tx, 64, g_bits + g_n);
    for (int f = 0; f < nframes; f++) {
        uint8_t a[9];
        make_ambe(f, a);
        g_n += dstar_tx_frame(tx, fast ? NULL : a, g_bits + g_n);
    }
    g_n += dstar_tx_end(tx, g_bits + g_n);
    for (int i = 0; i < 64; i++)
        g_bits[g_n++] = (uint8_t)(rnd() & 1u);
}

static void test_blocks(void)
{
    /* scrambler start = slow data XOR pattern 70 4F 93 */
    uint8_t z[24] = {0}, b[3];
    dstar_scramble(z, 24);
    dv_pack_lsb(z, 24, b);
    CHECK(b[0] == 0x70 && b[1] == 0x4F && b[2] == 0x93);
    /* sequence period 127 */
    uint8_t z2[254] = {0};
    dstar_scramble(z2, 254);
    CHECK(memcmp(z2, z2 + 127, 127) == 0);

    /* interleave matrix from JARL Ap2.2 */
    uint8_t in[DSTAR_HEADER_BITS], out[DSTAR_HEADER_BITS], back[DSTAR_HEADER_BITS];
    uint16_t idx[DSTAR_HEADER_BITS];
    for (unsigned i = 0; i < DSTAR_HEADER_BITS; i++)
        in[i] = (uint8_t)(rnd() & 1u);
    /* trace positions with unique values: interleave index array via bytes */
    for (unsigned j = 0; j < DSTAR_HEADER_BITS; j++)
        idx[j] = 0xFFFF;
    for (unsigned i = 0; i < DSTAR_HEADER_BITS; i += 1) {
        static uint8_t one[DSTAR_HEADER_BITS], o2[DSTAR_HEADER_BITS];
        memset(one, 0, sizeof(one));
        one[i] = 1;
        dstar_interleave(one, o2);
        for (unsigned j = 0; j < DSTAR_HEADER_BITS; j++)
            if (o2[j])
                idx[j] = (uint16_t)i;
    }
    int bij = 1;
    for (unsigned j = 0; j < DSTAR_HEADER_BITS; j++)
        if (idx[j] == 0xFFFF)
            bij = 0;
    CHECK(bij);
    CHECK_EQ(idx[0], 0);
    CHECK_EQ(idx[1], 24);
    CHECK_EQ(idx[27], 648);  /* row 0, column 27 */
    CHECK_EQ(idx[28], 1);    /* row 1 */
    CHECK_EQ(idx[335], 659); /* row 11, column 27 */
    CHECK_EQ(idx[336], 12);  /* row 12 has 27 columns */
    CHECK_EQ(idx[362], 636); /* row 12, column 26 */
    CHECK_EQ(idx[659], 647); /* row 23, column 26 */
    dstar_interleave(in, out);
    dstar_deinterleave(out, back);
    CHECK(memcmp(in, back, sizeof(in)) == 0);
}

static void test_header(void)
{
    dstar_header_t h, h2;
    uint8_t raw[41], raw2[41], bits[660];
    dstar_header_repeater(&h, "w1abc", "ID51", NULL, "W1XYZ  B", true);
    CHECK(memcmp(h.rpt1, "W1XYZ  B", 8) == 0);
    CHECK(memcmp(h.rpt2, "W1XYZ  G", 8) == 0);
    CHECK(memcmp(h.ur, "CQCQCQ  ", 8) == 0);
    CHECK(memcmp(h.my, "W1ABC   ", 8) == 0);
    CHECK(memcmp(h.my_sfx, "ID51", 4) == 0);
    CHECK_EQ(h.flag[0], 0x40);
    dstar_header_pack(&h, raw);
    /* layout: flags, RPT2, RPT1, UR, MY, MY2, CRC low byte first */
    CHECK(memcmp(raw + 3, "W1XYZ  G", 8) == 0);
    CHECK(memcmp(raw + 11, "W1XYZ  B", 8) == 0);
    uint16_t crc = dv_crc16_x25(raw, 39);
    CHECK_EQ(raw[39], crc & 0xFF);
    CHECK_EQ(raw[40], crc >> 8);
    /* property of the X.25 FCS: CRC over data + FCS (low byte first) gives
     * the fixed residue 0x0F47 (before inversion 0xF0B8) */
    CHECK_EQ(dv_crc16_x25(raw, 41), 0x0F47);
    CHECK(dstar_header_unpack(raw, &h2));
    CHECK(memcmp(&h, &h2, sizeof(h)) == 0);
    raw[20] ^= 1;
    CHECK(!dstar_header_unpack(raw, &h2));
    raw[20] ^= 1;

    dstar_header_encode(raw, bits);
    CHECK_EQ(dstar_header_decode(bits, raw2), 0);
    CHECK(memcmp(raw, raw2, 41) == 0);
    /* 13 channel errors spread over the header */
    for (int i = 0; i < 13; i++)
        bits[i * 50 + 3] ^= 1;
    int e = dstar_header_decode(bits, raw2);
    CHECK_EQ(e, 13);
    CHECK(memcmp(raw, raw2, 41) == 0);
    /* a burst of 8 adjacent air bits is spread by the interleaver */
    dstar_header_encode(raw, bits);
    for (int i = 0; i < 8; i++)
        bits[300 + i] ^= 1;
    dstar_header_decode(bits, raw2);
    CHECK(memcmp(raw, raw2, 41) == 0);

    /* routing helpers */
    char ur[8];
    dstar_ur_link(ur, "REF001", 'c');
    CHECK(memcmp(ur, "REF001CL", 8) == 0);
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_LINK);
    dstar_ur_unlink(ur);
    CHECK(memcmp(ur, "       U", 8) == 0);
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_UNLINK);
    dstar_ur_zone(ur, "W1XYZ  B");
    CHECK(memcmp(ur, "/W1XYZ B", 8) == 0);
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_ZONE);
    dstar_ur_echo(ur);
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_ECHO);
    dstar_ur_call(ur, "K1ABC");
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_CALLSIGN);
    dstar_ur_cq(ur);
    CHECK_EQ(dstar_ur_classify(ur), DSTAR_UR_CQ);
    CHECK(dstar_header_via_gateway(&h));
    CHECK(!dstar_header_is_direct(&h));
    dstar_header_turnaround(&h, &h2);
    CHECK(memcmp(h2.rpt1, "W1XYZ  G", 8) == 0 && memcmp(h2.rpt2, "W1XYZ  B", 8) == 0);
    dstar_header_simplex(&h, "W1ABC", "", "CQCQCQ");
    CHECK(dstar_header_is_direct(&h));
    CHECK(memcmp(h.rpt1, "DIRECT  ", 8) == 0);
    char s[9];
    dstar_callsign_get(h.my, 8, s);
    CHECK(strcmp(s, "W1ABC") == 0);

    /* D-PRS */
    char line[128];
    size_t n = dstar_dprs_build(line, sizeof(line), "W1ABC-7>API51,DSTAR*:!4224.30N/07112.31W>test");
    CHECK(n > 20 && memcmp(line, "$$CRC", 5) == 0 && line[n - 1] == '\r');
    CHECK(dstar_dprs_check(line, n));
    line[20] ^= 1;
    CHECK(!dstar_dprs_check(line, n));
}

static void test_stream(void)
{
    dstar_header_t h;
    dstar_tx_t tx;
    dstar_rx_t rx;
    cap_t c;
    const char *gps = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";

    dstar_header_repeater(&h, "W1ABC", "D890", "CQCQCQ", "W1XYZ  B", false);
    dstar_tx_init(&tx, &h);
    dstar_tx_set_text(&tx, "Hello from D890UV");
    dstar_tx_set_csq(&tx, true, 0x12, 0x34);
    dstar_tx_queue_data(&tx, (const uint8_t *)gps, strlen(gps));
    build_stream(&tx, 63, 300, false);

    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits, g_n);
    CHECK_EQ(c.header_ok, 1);
    CHECK(memcmp(&c.hdr, &h, sizeof(h)) == 0);
    CHECK_EQ(c.nvoice, 63);
    int vok = 1;
    for (int i = 0; i < 63; i++) {
        uint8_t a[9];
        make_ambe(i, a);
        if (memcmp(a, c.voice[i], 9) != 0 || c.seqs[i] != i % 21)
            vok = 0;
    }
    CHECK(vok);
    CHECK_EQ(c.texts, 1);
    CHECK(strcmp(c.text, "Hello from D890UV   ") == 0);
    CHECK_EQ(c.lines, 1);
    CHECK(strncmp(c.line, "$GPGGA,123519", 13) == 0);
    CHECK(c.csq >= 2);
    CHECK(c.csqv[0] == 0x12 && c.csqv[1] == 0x34);
    CHECK_EQ(c.ends, 1);
    CHECK_EQ(c.lost, 0);
    CHECK_EQ(c.header_slow, 0); /* same header resent: no duplicate event */
    CHECK_EQ(dstar_rx_state(&rx), DSTAR_RX_SEARCH);

    /* same stream with random bit errors (1 in 200) */
    size_t hdr_start = 300 + 64 + 15;
    for (size_t i = hdr_start; i < g_n - 64; i += 150 + (rnd() % 100))
        g_bits[i] ^= 1;
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits, g_n);
    CHECK_EQ(c.header_ok, 1);
    CHECK_EQ(c.nvoice, 63);
    CHECK_EQ(c.ends, 1);

    /* late entry: start receiving in the middle (frame 25) */
    dstar_tx_init(&tx, &h);
    dstar_tx_set_text(&tx, "late entry test");
    build_stream(&tx, 90, 0, false);
    size_t start = 64 + 15 + 660 + 25 * 96 + 40;
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits + start, g_n - start);
    CHECK_EQ(c.late, 1);
    CHECK_EQ(c.header_slow, 1);            /* rebuilt from slow data (odd superframe) */
    CHECK(memcmp(&c.hdr, &h, sizeof(h)) == 0);
    CHECK_EQ(c.nvoice, 90 - 43);           /* frames 43..89 after the sync in frame 42 */
    CHECK_EQ(c.seqs[0], 1);
    CHECK_EQ(c.ends, 1);

    /* lost: stream cut without end pattern */
    build_stream(&tx, 30, 0, false);
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits, 64 + 15 + 660 + 30 * 96);
    for (int i = 0; i < 96 * 70; i++)
        dstar_rx_push_bit(&rx, (uint8_t)(rnd() & 1u));
    CHECK_EQ(c.lost, 1);
}

static void test_fast(void)
{
    dstar_header_t h;
    dstar_tx_t tx;
    dstar_rx_t rx;
    cap_t c;
    static uint8_t data[400];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(rnd() >> 8);
    dstar_header_simplex(&h, "W1ABC", "", "CQCQCQ");
    dstar_tx_init(&tx, &h);
    dstar_tx_set_fast_data(&tx, data, sizeof(data));
    build_stream(&tx, 63, 0, true);
    CHECK(!dstar_tx_fast_busy(&tx));
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits, g_n);
    CHECK_EQ(c.fast_len, sizeof(data));
    CHECK(memcmp(c.fast, data, sizeof(data)) == 0);
    /* superframe 1 has the header resend (9 blocks), superframe 0/2 carry
     * fast data in all 10 blocks */
    CHECK(c.nvoice > 0);
}

static void test_gmsk(void)
{
    dstar_header_t h;
    dstar_tx_t tx;
    dstar_rx_t rx;
    cap_t c;
    dstar_gmsk_mod_t m;
    dstar_gmsk_demod_t d;
    const unsigned sps = 10;
    static float smp[20000 * 10];
    static uint8_t rb[20000 + 64];

    /* raw bit round trip with DC offset, noise and a 0.2 % clock error */
    CHECK_EQ(dstar_gmsk_mod_init(&m, sps, 0.5f), 0);
    uint8_t bits[2000];
    for (int i = 0; i < 2000; i++)
        bits[i] = (uint8_t)(i < 200 ? (i & 1) == 0 : (rnd() & 1u));
    size_t ns = dstar_gmsk_mod(&m, bits, 2000, smp);
    CHECK_EQ(ns, 2000u * sps);
    /* resample by linear interpolation to simulate clock error */
    static float rs[2000 * 10 + 100];
    size_t nr = 0;
    for (float t = 0.0f; t < (float)(ns - 1); t += 1.002f) {
        size_t i0 = (size_t)t;
        float fr = t - (float)i0;
        float v = smp[i0] * (1.0f - fr) + smp[i0 + 1] * fr;
        float noise = ((float)(rnd() % 1000) / 1000.0f - 0.5f) * 0.3f;
        rs[nr++] = 0.8f * v + 0.3f + noise;
    }
    dstar_gmsk_demod_init(&d, sps);
    size_t nb = dstar_gmsk_demod(&d, rs, nr, rb, sizeof(rb));
    CHECK(nb > 1900);
    /* align: find offset with fewest errors over bits 400..1800 */
    int best = 100000, bo = 0;
    for (int off = -10; off <= 10; off++) {
        int err = 0;
        for (int i = 400; i < 1800; i++) {
            int j = i + off;
            if (j < 0 || (size_t)j >= nb) {
                err++;
                continue;
            }
            err += rb[j] != bits[i];
        }
        if (err < best) {
            best = err;
            bo = off;
        }
    }
    (void)bo;
    CHECK(best <= 2);
    printf("  gmsk raw: %d bit errors in 1400 (offset %d)\n", best, bo);

    /* full D-STAR stream through the reference modem into the receiver */
    dstar_header_simplex(&h, "W1ABC", "", "CQCQCQ");
    dstar_tx_init(&tx, &h);
    dstar_tx_set_text(&tx, "via gmsk");
    build_stream(&tx, 42, 100, false);
    dstar_gmsk_mod_init(&m, sps, 0.5f);
    ns = dstar_gmsk_mod(&m, g_bits, g_n, smp);
    for (size_t i = 0; i < ns; i++)
        smp[i] = -smp[i] * 1.5f + ((float)(rnd() % 1000) / 1000.0f - 0.5f) * 0.4f; /* inverted discriminator */
    dstar_gmsk_demod_init(&d, sps);
    d.invert = 1;
    nb = dstar_gmsk_demod(&d, smp, ns, rb, sizeof(rb));
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, rb, nb);
    CHECK_EQ(c.header_ok, 1);
    CHECK_EQ(c.nvoice, 42);
    CHECK_EQ(c.texts, 1);
    CHECK_EQ(c.ends, 1);
}

/* Interop facts cross-checked with MMDVMHost DStarDefines.h (values only). */
static void test_facts(void)
{
    static const uint8_t PN9[9] = {0x70, 0x4F, 0x93, 0x40, 0x64, 0x74, 0x6D, 0x30, 0x2B};
    static const uint8_t NULL_AMBE_SCR[9] = {0xEE, 0xC2, 0xA1, 0xC8, 0x42, 0x6E, 0x52, 0x51, 0xC3};
    uint8_t z[72] = {0}, b[9];
    dstar_scramble(z, 72);
    dv_pack_lsb(z, 72, b);
    CHECK(memcmp(b, PN9, 9) == 0);
    int nok = 1;
    for (int i = 0; i < 9; i++)
        if ((DSTAR_AMBE_SILENCE[i] ^ PN9[i]) != NULL_AMBE_SCR[i])
            nok = 0;
    CHECK(nok);

    dstar_header_t h;
    dstar_tx_t tx;
    uint8_t f[96], v[9], d[3];
    dstar_header_simplex(&h, "W1ABC", "", NULL);
    /* filler block on air = 66 66 66 ^ 70 4F 93 = 16 29 F5 */
    dstar_tx_init(&tx, &h);
    dstar_tx_frame(&tx, DSTAR_AMBE_SILENCE, f);
    dstar_tx_frame(&tx, DSTAR_AMBE_SILENCE, f);
    dv_pack_lsb(f + 72, 24, d);
    CHECK(d[0] == 0x16 && d[1] == 0x29 && d[2] == 0xF5);
    /* fast data: voice area scrambled with all 9 scrambler bytes */
    static uint8_t fd[28];
    for (int i = 0; i < 28; i++)
        fd[i] = (uint8_t)(0xA0 + i);
    dstar_tx_init(&tx, &h);
    dstar_tx_set_fast_data(&tx, fd, 28);
    dstar_tx_frame(&tx, NULL, f); /* seq 0 carries D20..D27 */
    dv_pack_lsb(f, 72, v);
    const uint8_t plain0[9] = {fd[20], fd[21], fd[22], fd[23], 0x02, fd[24], fd[25], fd[26], fd[27]};
    int fok = 1;
    for (int i = 0; i < 9; i++)
        if ((plain0[i] ^ PN9[i]) != v[i])
            fok = 0;
    CHECK(fok);
    dstar_tx_frame(&tx, NULL, f); /* seq 1: mini header 0x9C = 28 bytes */
    dv_pack_lsb(f + 72, 24, d);
    CHECK_EQ(d[0] ^ 0x70, 0x9C);

    /* mini header 0x9D is not fast data (max 28 bytes): voice stays voice */
    dstar_rx_t rx;
    cap_t c;
    dstar_tx_init(&tx, &h);
    build_stream(&tx, 21, 0, false);
    size_t f1 = 64 + 15 + 660 + 96 + 72; /* data part of frame seq 1 */
    uint8_t mh[3] = {0x9D ^ 0x70, 0x00 ^ 0x4F, 0x00 ^ 0x93}, mb[24];
    dv_unpack_lsb(mh, 24, mb);
    memcpy(g_bits + f1, mb, 24);
    memset(&c, 0, sizeof(c));
    dstar_rx_init(&rx, &CB, &c);
    dstar_rx_push_bits(&rx, g_bits, g_n);
    CHECK_EQ(c.fast_blocks, 0);
    CHECK_EQ(c.nvoice, 21);
}

void test_dstar(void)
{
    test_facts();
    test_blocks();
    test_header();
    test_stream();
    test_fast();
    test_gmsk();
}
