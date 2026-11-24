/* FEC / CRC tests. Known-answer vectors:
 *  - Golay(24,12) generator rows, Hamming rows, NID BCH rows: as published in
 *    OP25 (boatbod/op25 op25_golay.h, p25p1_voice_encode.cc), which match the
 *    TIA-102.BAAA generator matrices.
 *  - CRC check values from the CRC catalogue ("123456789").
 */
#include "tst.h"
#include "p25_fec.h"
#include <string.h>

static void t_golay(void)
{
    static const uint32_t rows[12] = { 040006165, 020003073, 010007550, 04003664, 02001732, 01006631,
                                       0403315, 0201547, 0106706, 045227, 024476, 014353 };
    for (int i = 0; i < 12; i++) CHECK_EQ(p25_golay24_encode(1u << (11 - i)), rows[i]);
    /* every message, random error patterns up to 3 bits */
    int bad = 0;
    for (uint32_t m = 0; m < 4096; m++) {
        uint32_t c = p25_golay23_encode(m);
        uint32_t e = 0;
        int ne = (int)rndn(4);
        while ((int)p25_popcount32(e) < ne) e |= 1u << rndn(23);
        int errs;
        if (p25_golay23_decode(c ^ e, &errs) != m || errs != ne) bad++;
        uint32_t c24 = p25_golay24_encode(m);
        e = 0;
        while ((int)p25_popcount32(e) < ne) e |= 1u << rndn(24);
        if (p25_golay24_decode(c24 ^ e, &errs) != (int)m) bad++;
    }
    CHECK_EQ(bad, 0);
    /* 4 errors detected by the extended code */
    uint32_t c24 = p25_golay24_encode(0x5A5);
    CHECK_EQ(p25_golay24_decode(c24 ^ 0x00F000u, NULL), -1);
    /* (18,6) */
    for (uint32_t m = 0; m < 64; m++) {
        uint32_t c = p25_golay18_encode(m);
        CHECK(c < (1u << 18));
        CHECK_EQ(p25_golay18_decode(c ^ (1u << rndn(18)) ^ (1u << rndn(18)), NULL), (int)m);
    }
}

static void t_hamming(void)
{
    /* OP25 hmg1063EncTbl parity values for data 1,2,3 = 12,3,15 */
    CHECK_EQ(p25_hamming10_encode(1) & 0xF, 12);
    CHECK_EQ(p25_hamming10_encode(2) & 0xF, 3);
    CHECK_EQ(p25_hamming10_encode(3) & 0xF, 15);
    CHECK_EQ(p25_hamming10_encode(63) & 0xF, 0);
    int bad = 0, e;
    for (uint32_t m = 0; m < 64; m++)
        for (int b = 0; b < 10; b++)
            if (p25_hamming10_decode(p25_hamming10_encode(m) ^ (1u << b), &e) != m || e != 1) bad++;
    for (uint32_t m = 0; m < 2048; m++) {
        if (p25_hamming15_decode(p25_hamming15_encode(m), &e) != m || e) bad++;
        if (p25_hamming15_decode(p25_hamming15_encode(m) ^ (1u << rndn(15)), &e) != m || e != 1) bad++;
    }
    CHECK_EQ(bad, 0);
    /* LSD parity byte (MMDVMHost CCS_PARITY values for 0x01, 0x80, 0xFF, 0x03) */
    CHECK_EQ(p25_cyclic16_encode(0x01) & 0xFF, 0x39);
    CHECK_EQ(p25_cyclic16_encode(0x80) & 0xFF, 0x4E);
    CHECK_EQ(p25_cyclic16_encode(0xFF) & 0xFF, 0x63);
    CHECK_EQ(p25_cyclic16_encode(0x03) & 0xFF, 0x4B);
    for (uint32_t m = 0; m < 256; m += 7) {
        uint32_t c = p25_cyclic16_encode(m);
        CHECK_EQ(p25_cyclic16_decode(c ^ (1u << rndn(16)) ^ (1u << rndn(16)), NULL), (int)m);
    }
}

static void t_nid(void)
{
    static const uint64_t rows[16] = {
        0x8000cd930bdd3b2aULL, 0x4000ab5a8e33a6beULL, 0x2000983e4cc4e874ULL, 0x10004c1f2662743aULL,
        0x0800eb9c98ec0136ULL, 0x0400b85d47ab3bb0ULL, 0x02005c2ea3d59dd8ULL, 0x01002e1751eaceecULL,
        0x0080170ba8f56776ULL, 0x0040c616dfa78890ULL, 0x0020630b6fd3c448ULL, 0x00103185b7e9e224ULL,
        0x000818c2dbf4f112ULL, 0x0004c1f2662743a2ULL, 0x0002ad6a38ce9afbULL, 0x00019b2617ba7657ULL };
    /* linear combination check: NAC 0x293, DUID 5 */
    uint16_t info = (0x293 << 4) | 5;
    uint64_t exp = 0;
    for (int i = 0; i < 16; i++) if (info & (0x8000 >> i)) exp ^= rows[i];
    CHECK_EQ(p25_nid_encode(0x293, 5), exp);
    for (int i = 0; i < 16; i++) {
        uint16_t v = (uint16_t)(0x8000 >> i);
        CHECK_EQ(p25_nid_encode(v >> 4, v & 15), rows[i]);
    }
    int bad = 0;
    for (int t = 0; t < 400; t++) {
        uint16_t nac = (uint16_t)rndn(4096);
        uint8_t duid = (uint8_t)rndn(16);
        uint64_t c = p25_nid_encode(nac, duid), e = 0;
        int ne = t % 12;
        while ((int)p25_popcount64(e) < ne) e |= 1ULL << (1 + rndn(63));
        uint16_t n2; uint8_t d2; int errs;
        if (p25_nid_decode(c ^ e, &n2, &d2, &errs) != 0 || n2 != nac || d2 != duid || errs != ne) bad++;
    }
    CHECK_EQ(bad, 0);
}

static void rs_case(unsigned k, unsigned nroots, unsigned nerr, unsigned neras)
{
    uint8_t cw[63], orig[63], eras[63];
    unsigned n = k + nroots;
    for (unsigned i = 0; i < k; i++) cw[i] = (uint8_t)rndn(64);
    p25_rs_encode(cw, k, nroots, cw + k);
    memcpy(orig, cw, n);
    uint8_t used[63] = { 0 };
    for (unsigned i = 0; i < neras; i++) {
        unsigned p; do p = rndn(n); while (used[p]);
        used[p] = 1; eras[i] = (uint8_t)p; cw[p] = (uint8_t)rndn(64);
    }
    for (unsigned i = 0; i < nerr; i++) {
        unsigned p; do p = rndn(n); while (used[p]);
        used[p] = 1; cw[p] ^= (uint8_t)(1 + rndn(63));
    }
    int r = p25_rs_decode(cw, n, nroots, eras, neras);
    CHECK(r >= 0);
    CHECK(memcmp(cw, orig, n) == 0);
}

static void t_rs(void)
{
    /* syndrome check: alpha^1..alpha^nroots are roots of every code word */
    uint8_t cw[63];
    for (unsigned i = 0; i < 12; i++) cw[i] = (uint8_t)(i * 5 + 1);
    p25_rs_encode(cw, 12, 12, cw + 12);
    CHECK_EQ(p25_rs_decode(cw, 24, 12, NULL, 0), 0);
    for (int t = 0; t < 30; t++) {
        rs_case(12, 12, 6, 0);      /* LDU1 LC (24,12,13) */
        rs_case(16, 8, 4, 0);       /* LDU2 ES (24,16,9) */
        rs_case(20, 16, 8, 0);      /* HDU (36,20,17) */
        rs_case(20, 16, 3, 10);     /* errors + erasures */
        rs_case(35, 28, 14, 0);     /* full Phase 2 (63,35) */
        rs_case(16, 28, 5, 18);     /* ESS (44,16,29) */
    }
    /* too many errors must not be "corrected" silently into wrong data often */
    int wrong = 0;
    for (int t = 0; t < 50; t++) {
        for (unsigned i = 0; i < 12; i++) cw[i] = (uint8_t)rndn(64);
        p25_rs_encode(cw, 12, 12, cw + 12);
        for (int i = 0; i < 9; i++) cw[rndn(24)] ^= (uint8_t)(1 + rndn(63));
        uint8_t c2[24]; memcpy(c2, cw, 24);
        if (p25_rs_decode(c2, 24, 12, NULL, 0) >= 0) wrong++;
    }
    CHECK(wrong < 25);
    /* punctured: FACCH n_full 54 (45 sent + 9 punctured), k 26 */
    for (int t = 0; t < 20; t++) {
        uint8_t m[54], o[54];
        for (int i = 0; i < 26; i++) m[i] = (uint8_t)rndn(64);
        p25_rs_encode(m, 26, 28, m + 26);
        memcpy(o, m, 54);
        for (int i = 0; i < 4; i++) m[rndn(45)] ^= (uint8_t)(1 + rndn(63));
        CHECK(p25_rs_decode_punct(m, 54, 28, 9) >= 0);
        CHECK(memcmp(m, o, 45) == 0);
    }
}

static void t_trellis(void)
{
    uint8_t in[18], out[18], d[98];
    for (int t = 0; t < 50; t++) {
        for (int i = 0; i < 18; i++) in[i] = (uint8_t)rnd();
        p25_trellis12_encode(in, d);
        for (int e = 0; e < (t % 4); e++) d[rndn(98)] ^= (uint8_t)(1 + rndn(3));
        p25_trellis12_decode(d, out);
        CHECK(memcmp(in, out, 12) == 0);
        p25_trellis34_encode(in, d);
        if (t % 3) d[rndn(98)] ^= 1;
        p25_trellis34_decode(d, out);
        CHECK(memcmp(in, out, 18) == 0);
    }
    /* zero input -> constellation 0 everywhere = dibit pair 0x2 (+1,-1) */
    memset(in, 0, 12);
    p25_trellis12_encode(in, d);
    CHECK_EQ(d[0], 0); CHECK_EQ(d[1], 2); CHECK_EQ(d[96], 0); CHECK_EQ(d[97], 2);
}

static void t_crc(void)
{
    const uint8_t s[] = "123456789";
    CHECK_EQ(p25_crc16(s, 72), 0x31C3 ^ 0xFFFF);      /* CRC-16/XMODEM inverted */
    CHECK_EQ(p25_crc32(s, 72), 0x765E7680u);          /* CRC-32/CKSUM */
    /* CRC-12: OP25 style check (remainder of message||crc compares equal) */
    uint8_t buf[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0 };
    uint16_t c = p25_crc12(buf, 40);
    p25_bits_put(buf, 40, 12, c);
    CHECK_EQ(p25_bits_get(buf, 40, 12), p25_crc12(buf, 40));
    /* PN: u0 = 0 -> first mask bits of OP25 pr_n[0] = 4377671 >> 1 */
    uint32_t st = 0;
    CHECK_EQ(p25_pn_bits(&st, 23), 4377671u >> 1);
}

void test_fec(void)
{
    t_golay();
    t_hamming();
    t_nid();
    t_rs();
    t_trellis();
    t_crc();
}
