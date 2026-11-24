#include <string.h>

#include "../common/dv_common.h"
#include "test.h"

/* bitwise reference: reflected CRC fed bit by bit (independent of the
 * byte loop in dv_common.c) */
static uint16_t ref_x25(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            unsigned in = (d[i] >> b) & 1u;
            unsigned fb = (crc ^ in) & 1u;
            crc >>= 1;
            if (fb)
                crc ^= 0x8408;
        }
    return (uint16_t)~crc;
}

static void conv_roundtrip(const dv_conv_t *c, size_t n, int nerr)
{
    uint8_t in[DV_VITERBI_MAX_STEPS], code[2 * DV_VITERBI_MAX_STEPS], out[DV_VITERBI_MAX_STEPS];
    for (size_t i = 0; i < n; i++)
        in[i] = (uint8_t)(rnd() & 1u);
    for (size_t i = n - (c->k - 1u); i < n; i++)
        in[i] = 0;
    dv_conv_encode(c, in, n, code);
    /* spread errors far apart */
    for (int e = 0; e < nerr; e++)
        code[(size_t)e * (2 * n / (size_t)(nerr + 1)) + 7] ^= 1u;
    int m = dv_viterbi_decode(c, code, n, 0, out);
    CHECK(memcmp(in, out, n) == 0);
    CHECK_EQ(m, nerr);
}

void test_common(void)
{
    const uint8_t chk[] = "123456789";
    CHECK_EQ(dv_crc16_x25(chk, 9), 0x906E);       /* CRC-16/X-25 check value */
    CHECK_EQ(dv_crc16_ysf(chk, 9), 0xCE3C);       /* ~XMODEM(0x31C3) */
    CHECK_EQ(dv_crc16_ysf(chk, 9), (uint16_t)~0x31C3);

    uint8_t buf[64];
    for (int i = 0; i < 64; i++)
        buf[i] = (uint8_t)rnd();
    CHECK_EQ(dv_crc16_x25(buf, 64), ref_x25(buf, 64));

    uint8_t bits[64 * 8], back[64];
    dv_unpack_lsb(buf, 512, bits);
    CHECK_EQ(bits[0], buf[0] & 1u);
    dv_pack_lsb(bits, 512, back);
    CHECK(memcmp(buf, back, 64) == 0);
    dv_unpack_msb(buf, 512, bits);
    CHECK_EQ(bits[0], buf[0] >> 7);
    dv_pack_msb(bits, 512, back);
    CHECK(memcmp(buf, back, 64) == 0);

    const dv_conv_t ds = {3, 0x7, 0x5}, ys = {5, 0x19, 0x17};
    conv_roundtrip(&ds, 330, 0);
    conv_roundtrip(&ds, 330, 8);
    conv_roundtrip(&ys, 100, 0);
    conv_roundtrip(&ys, 180, 6);

    /* known small encode: D-STAR code for input 1,0,0 (+tail) = 11 10 11 */
    uint8_t in3[3] = {1, 0, 0}, c3[6];
    dv_conv_encode(&ds, in3, 3, c3);
    CHECK(c3[0] == 1 && c3[1] == 1 && c3[2] == 1 && c3[3] == 0 && c3[4] == 1 && c3[5] == 1);
}
