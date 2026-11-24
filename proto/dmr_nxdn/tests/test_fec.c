/* FEC / CRC tests. Vectors come from the spec text itself:
 *  - ETSI TS 102 361-1 annex D figure D.1: BPTC(196,96) encoded Idle message (all row/column codes)
 *  - table B.3 / B.5 / figure B.3 / B.6: interleave positions
 *  - table B.18 vs g(x) (B.10): RS generator matrix vs polynomial division
 *  - table B.19: GF(2^8) exponential table entries
 *  - minimum distance of every block code (catches a mistyped generator row)
 *  - CRC catalogue check values ("123456789") for the plain generator polynomials
 */
#include "test.h"
#include <stdio.h>
#include "../common/fec.h"
#include "../common/bits.h"
#include "../dmr/dmr.h"

/* figure D.1, 13 rows x 15 columns (row 0 starts with R(2) R(1) R(0)) */
static const char *FIG_D1[13] = {
    "000111111110100", "100000111101101", "111110001011101", "110011001000101",
    "000100101000111", "111011010001101", "111100111110010", "001101100011101",
    "010100100010101", "010011100100011", "101100111101100", "001000010000100",
    "010010101110110",
};

typedef void (*enc_fn)(uint8_t *);
typedef int (*dec_fn)(uint8_t *);

static void hamming_check(const char *name, int n, int k, int dmin, enc_fn enc, dec_fn dec)
{
    int minw = 99;
    uint8_t b[20];
    for (uint32_t d = 1; d < (1u << k); d++) {
        memset(b, 0, sizeof b);
        for (int i = 0; i < k; i++) b[i] = (uint8_t)((d >> (k - 1 - i)) & 1u);
        enc(b);
        int w = 0;
        for (int i = 0; i < n; i++) w += b[i];
        if (w < minw) minw = w;
        if ((d & 63u) == 1u) {      /* single error correction at every position */
            for (int e = 0; e < n; e++) {
                uint8_t c[20];
                memcpy(c, b, sizeof c);
                c[e] ^= 1u;
                int r = dec(c);
                if (r != 1 || memcmp(c, b, (size_t)n)) { printf("  %s: pos %d not corrected\n", name, e); g_fail++; }
                g_checks++;
            }
        }
    }
    if (minw != dmin) printf("  %s dmin %d\n", name, minw);
    CHECK_EQ(minw, dmin);
}

static uint8_t gf_pow(unsigned e) { uint8_t r = 1; while (e--) r = fec_gf256_mul(r, 2); return r; }

static void test_rs(void)
{
    /* table B.19 spot values */
    CHECK_EQ(gf_pow(8), 0x1D); CHECK_EQ(gf_pow(16), 0x4C); CHECK_EQ(gf_pow(25), 0x03);
    CHECK_EQ(gf_pow(100), 0x11); CHECK_EQ(gf_pow(200), 0x1C); CHECK_EQ(gf_pow(254), 0x8E);
    /* g(x) = x^3 + 0e x^2 + 38 x + 40 has roots alpha^1..3 */
    for (unsigned j = 1; j <= 3; j++) {
        uint8_t a = gf_pow(j), v = 1;
        v = (uint8_t)(fec_gf256_mul(v, a) ^ 0x0E);
        v = (uint8_t)(fec_gf256_mul(v, a) ^ 0x38);
        v = (uint8_t)(fec_gf256_mul(v, a) ^ 0x40);
        CHECK_EQ(v, 0);
    }
    for (int t = 0; t < 200; t++) {
        uint8_t m[12], par[3];
        for (int i = 0; i < 9; i++) m[i] = (uint8_t)trand();
        fec_rs129_enc(m, par);
        /* polynomial division: remainder of m(x) x^3 mod g(x) */
        uint8_t r[3] = { 0, 0, 0 };
        for (int i = 0; i < 9; i++) {
            uint8_t f = (uint8_t)(m[i] ^ r[0]);
            r[0] = (uint8_t)(r[1] ^ fec_gf256_mul(f, 0x0E));
            r[1] = (uint8_t)(r[2] ^ fec_gf256_mul(f, 0x38));
            r[2] = fec_gf256_mul(f, 0x40);
        }
        CHECK(!memcmp(r, par, 3));
        memcpy(m + 9, par, 3);
        uint8_t c[12];
        memcpy(c, m, 12);
        CHECK_EQ(fec_rs129_dec(c), 0);
        int pos = (int)(trand() % 12u);
        c[pos] ^= (uint8_t)(1u + trand() % 255u);
        CHECK_EQ(fec_rs129_dec(c), 1);
        CHECK(!memcmp(c, m, 12));
    }
}

static void test_bptc(void)
{
    uint8_t bits[196], info[12];
    bptc196_encode(DMR_IDLE_INFO, bits);
    /* rebuild the encode matrix from the transmit order and compare with figure D.1 */
    int bad = 0;
    for (unsigned i = 1; i < 196; i++) {
        unsigned r = (i - 1) / 15, c = (i - 1) % 15;
        if (bits[(i * 181u) % 196u] != (uint8_t)(FIG_D1[r][c] - '0')) bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(bits[0], 0);                         /* TX(195) = R(3) */
    CHECK_EQ(bptc196_decode(bits, info), 0);
    CHECK(!memcmp(info, DMR_IDLE_INFO, 12));
    /* table B.3 positions: index 2 = I(77), 76 = I(91), 136 = I(95), 191 = I(0) */
    static const struct { int idx, ibit; } pos[] = { { 2, 77 }, { 3, 68 }, { 76, 91 }, { 136, 95 }, { 191, 0 }, { 70, 5 } };
    for (unsigned p = 0; p < sizeof pos / sizeof pos[0]; p++) {
        uint8_t in[12] = { 0 };
        bits_put(in, (unsigned)(95 - pos[p].ibit), 1);
        bptc196_encode(in, bits);
        CHECK_EQ(bits[pos[p].idx], 1);
    }
    /* error correction: up to 2 random bit errors (always correctable, columns first) */
    for (int t = 0; t < 300; t++) {
        uint8_t in[12], out[12];
        for (int i = 0; i < 12; i++) in[i] = (uint8_t)trand();
        bptc196_encode(in, bits);
        int ne = 1 + (int)(trand() % 2u);
        for (int e = 0; e < ne; e++) bits[trand() % 196u] ^= 1u;
        CHECK(bptc196_decode(bits, out) >= 0);
        /* R(2..0) are not part of info: compare the 96 info bits */
        CHECK(!memcmp(out, in, 12));
    }
}

static void test_emb_cach(void)
{
    uint8_t lc[9] = { 0 }, bits[128], out[9];
    bits_put(lc, 0, 1);                       /* LC(71) */
    emblc_encode(lc, bits);
    CHECK_EQ(bits[0], 1);                     /* burst 1 TX(31) = LC(71) (figure B.3) */
    memset(lc, 0, sizeof lc);
    bits_put(lc, 71 - 60, 1);                 /* LC(60) is TX(30) */
    emblc_encode(lc, bits);
    CHECK_EQ(bits[1], 1);
    for (int t = 0; t < 200; t++) {
        for (int i = 0; i < 9; i++) lc[i] = (uint8_t)trand();
        emblc_encode(lc, bits);
        CHECK_EQ(emblc_decode(bits, out), 0);
        CHECK(!memcmp(out, lc, 9));
        bits[trand() % 128u] ^= 1u;           /* one error is corrected by the row code */
        CHECK_EQ(emblc_decode(bits, out), 0);
        CHECK(!memcmp(out, lc, 9));
    }
    /* annex D.1: null embedded message is all zero */
    memset(lc, 0, 9);
    emblc_encode(lc, bits);
    int ones = 0;
    for (int i = 0; i < 128; i++) ones += bits[i];
    CHECK_EQ(ones, 0);

    uint8_t slc[4] = { 0x08, 0, 0, 0 }, sb[68], so[4];     /* LC(27) only */
    cach_slc_encode(slc, sb);
    CHECK_EQ(sb[0], 1);                       /* CACH 1 first bit = LC(27) (figure B.6) */
    for (int t = 0; t < 200; t++) {
        slc[0] = (uint8_t)(trand() & 0xFu); slc[1] = (uint8_t)trand(); slc[2] = (uint8_t)trand(); slc[3] = (uint8_t)trand();
        cach_slc_encode(slc, sb);
        sb[trand() % 68u] ^= 1u;
        CHECK_EQ(cach_slc_decode(sb, so), 0);
        CHECK(!memcmp(so, slc, 4));
    }
    /* single burst BPTC: table B.5 index 1 = PC(7) */
    uint8_t s32[32];
    uint16_t v;
    sbbptc_encode(0x400, 0, s32);             /* SB(10) only */
    CHECK_EQ(s32[0], 1);
    for (uint16_t x = 0; x < 2048; x += 7) {
        sbbptc_encode(x, 1, s32);
        CHECK(sbbptc_decode(s32, 1, &v) >= 0);
        CHECK_EQ(v, x);
    }
}

static void test_crc(void)
{
    const uint8_t s[] = "123456789";
    uint8_t b[72];
    CHECK_EQ(crc_ccitt_dmr(s, 9), 0x31C3 ^ 0xFFFF);           /* CRC-16/XMODEM check 0x31C3, then I(x) */
    bits_unpack(b, s, 72);
    CHECK_EQ(crc8_dmr(b, 72), 0xF4);                           /* CRC-8/SMBUS check */
    CHECK_EQ(crc_bits(b, 72, 0x1021, 16, 0xFFFF), 0x29B1);     /* CRC-16/CCITT-FALSE (NXDN init 1s) */
    /* CRC-32: appending the CRC in the linearised (word-swapped) domain leaves remainder 0 */
    uint8_t m[14];
    for (int t = 0; t < 50; t++) {
        for (int i = 0; i < 10; i++) m[i] = (uint8_t)trand();
        uint32_t c = crc32_dmr(m, 10);
        uint8_t lin[14];
        for (int i = 0; i < 10; i += 2) { lin[i] = m[i + 1]; lin[i + 1] = m[i]; }
        lin[10] = (uint8_t)(c >> 24); lin[11] = (uint8_t)(c >> 16); lin[12] = (uint8_t)(c >> 8); lin[13] = (uint8_t)c;
        uint8_t lb[112];
        bits_unpack(lb, lin, 112);
        CHECK_EQ(crc_bits(lb, 112, 0x04C11DB7u, 32, 0), 0);
    }
    /* CRC-32 on real over-the-air packets (user data + pad, then the 4 CRC octets as received);
     * data from the test suite of OK-DMR ok-dmrlib (test_crc32.py), used as test data only.
     * The CRC octets arrive least significant first (figure B.8C). */
    static const char *const CAP[3][2] = {
        { "d6790062620003bf000700000000000000000000", "210b9a3d" },
        { "45000038fb410000401125490c23380b0d0008fd0fa10fa10024276a0d1a22047fffffff694728710f0a522c2d82534e6c0048564770402b", "82616528" },
        { "0600fb4f3d3f82afc6d80b42ce88668afc7d8b1807e83c308d95bb8be5dd59e95b2837e795af87005ae2a743535ca421601d", "c76ae25c" },
    };
    for (int v = 0; v < 3; v++) {
        uint8_t pk[64], cr[4];
        size_t n = strlen(CAP[v][0]) / 2;
        for (size_t i = 0; i < n; i++) { unsigned x; sscanf(CAP[v][0] + 2 * i, "%2x", &x); pk[i] = (uint8_t)x; }
        for (int i = 0; i < 4; i++) { unsigned x; sscanf(CAP[v][1] + 2 * i, "%2x", &x); cr[i] = (uint8_t)x; }
        uint32_t c = crc32_dmr(pk, n);
        CHECK_EQ(c, ((uint32_t)cr[3] << 24) | ((uint32_t)cr[2] << 16) | ((uint32_t)cr[1] << 8) | cr[0]);
    }
    /* CRC-9: x^9 M(x) mod G + I, verified against bitwise reference */
    for (int t = 0; t < 50; t++) {
        uint8_t d[16], bits[135];
        for (int i = 0; i < 16; i++) d[i] = (uint8_t)trand();
        uint8_t sn = (uint8_t)(trand() & 0x7Fu);
        bits_unpack(bits, d, 128);
        for (int i = 0; i < 7; i++) bits[128 + i] = (uint8_t)((sn >> (6 - i)) & 1u);
        CHECK_EQ(crc9_dmr(d, 16, sn), crc_bits(bits, 135, 0x59, 9, 0) ^ 0x1FF);
    }
    uint8_t lc[9] = { 255, 255, 255, 255, 255, 255, 255, 255, 255 };
    CHECK_EQ(cs5_dmr(lc), (9 * 255) % 31);
}

static void test_trellis(void)
{
    uint8_t in[18], d[98], out[18];
    for (int t = 0; t < 200; t++) {
        for (int i = 0; i < 18; i++) in[i] = (uint8_t)trand();
        trellis34_encode(in, d);
        CHECK_EQ(trellis34_decode(d, out), 0);
        CHECK(!memcmp(in, out, 18));
        {   /* one adjacent-level symbol error (the usual 4FSK error); 2 errors: ~95 %, see README */
            static const uint8_t adj[4][2] = { { 1, 2 }, { 0, 0 }, { 0, 3 }, { 2, 2 } };
            unsigned k = trand() % 98u;
            d[k] = adj[d[k]][trand() & 1u];
        }
        CHECK(trellis34_decode(d, out) > 0);
        CHECK(!memcmp(in, out, 18));
    }
    /* all-zero input: state 0, tribit 0 -> constellation 0 = (+1, -1) = dibits 00, 10 */
    memset(in, 0, 18);
    trellis34_encode(in, d);
    CHECK_EQ(d[0], 0); CHECK_EQ(d[1], 2);
}

static void test_conv(void)
{
    static const uint8_t keep[14] = { 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1 };
    conv_punct_t p = { keep, 14 };
    uint8_t in[175], enc[400], out[175];
    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < 171; i++) in[i] = (uint8_t)(trand() & 1u);
        memset(in + 171, 0, 4);
        unsigned n = conv_k5_encode(in, 175, enc, &p);
        CHECK_EQ(n, 300);
        for (int e = 0; e < 4; e++) enc[(unsigned)(e * 70) + trand() % 60u] ^= 1u;
        int m = conv_k5_decode(enc, n, out, 175, &p);
        CHECK(m >= 1 && m <= 4);
        CHECK(!memcmp(in, out, 175));
    }
    /* spec 4.5.4 example: X4 and X12 erased in each period of 14 */
    memset(in, 0, sizeof in);
    in[0] = 1;
    uint8_t full[28];
    conv_k5_encode(in, 14, full, NULL);
    /* impulse response: G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4 */
    static const uint8_t g1[5] = { 1, 0, 0, 1, 1 }, g2[5] = { 1, 1, 1, 0, 1 };
    for (int i = 0; i < 5; i++) { CHECK_EQ(full[2 * i], g1[i]); CHECK_EQ(full[2 * i + 1], g2[i]); }
}

void test_fec(void)
{
    hamming_check("h15_11", 15, 11, 3, fec_hamming_15_11_enc, fec_hamming_15_11_dec);
    hamming_check("h13_9", 13, 9, 3, fec_hamming_13_9_enc, fec_hamming_13_9_dec);
    hamming_check("h16_11", 16, 11, 4, fec_hamming_16_11_enc, fec_hamming_16_11_dec);
    hamming_check("h17_12", 17, 12, 3, fec_hamming_17_12_enc, fec_hamming_17_12_dec);
    hamming_check("h7_4", 7, 4, 3, fec_hamming_7_4_enc, fec_hamming_7_4_dec);
    int minw = 99;
    for (int d = 1; d < 256; d++) { int w = popcount32(fec_golay2087_enc((uint8_t)d)); if (w < minw) minw = w; }
    CHECK_EQ(minw, 8);
    minw = 99;
    for (int d = 1; d < 128; d++) { int w = popcount32(fec_qr1676_enc((uint8_t)d)); if (w < minw) minw = w; }
    CHECK_EQ(minw, 6);
    for (int t = 0; t < 300; t++) {
        uint8_t d = (uint8_t)trand();
        uint32_t cw = fec_golay2087_enc(d);
        for (int e = 0; e < 3; e++) cw ^= 1u << (trand() % 20u);
        int errs;
        CHECK_EQ(fec_golay2087_dec(cw, &errs), d);
        uint8_t q = (uint8_t)(trand() & 0x7Fu);
        uint16_t qc = fec_qr1676_enc(q);
        qc ^= (uint16_t)(1u << (trand() % 16u));
        qc ^= (uint16_t)(1u << (trand() % 16u));
        CHECK_EQ(fec_qr1676_dec(qc, &errs), q);
    }
    test_rs();
    test_bptc();
    test_emb_cach();
    test_crc();
    test_trellis();
    test_conv();
}
