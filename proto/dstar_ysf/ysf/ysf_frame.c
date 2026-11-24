/*
 * ysf_frame.c - YSF frame assembly helpers, callsign data, C4FM mapping.
 */
#include <ctype.h>
#include <string.h>

#include "../common/dv_common.h"
#include "ysf.h"

void ysf_bits_to_dibits(const uint8_t *bits, size_t nbits, uint8_t *dibits)
{
    for (size_t i = 0; i + 1 < nbits; i += 2)
        dibits[i / 2] = (uint8_t)(((bits[i] & 1u) << 1) | (bits[i + 1] & 1u));
}

void ysf_dibits_to_bits(const uint8_t *dibits, size_t ndibits, uint8_t *bits)
{
    for (size_t i = 0; i < ndibits; i++) {
        bits[2 * i] = (uint8_t)((dibits[i] >> 1) & 1u);
        bits[2 * i + 1] = (uint8_t)(dibits[i] & 1u);
    }
}

/* Table 3-1: 00 -> +1, 01 -> +3, 10 -> -1, 11 -> -3 */
int ysf_dibit_to_level(uint8_t dibit)
{
    static const int8_t L[4] = {1, 3, -1, -3};
    return L[dibit & 3u];
}

uint8_t ysf_level_to_dibit(float level)
{
    if (level >= 2.0f)
        return 1;
    if (level >= 0.0f)
        return 0;
    if (level >= -2.0f)
        return 2;
    return 3;
}

void ysf_build_frame(const ysf_fich_t *f, const uint8_t payload[720], uint8_t dibits[YSF_FRAME_DIBITS])
{
    uint8_t bits[YSF_FRAME_BITS];
    for (int i = 0; i < 40; i++)
        bits[i] = (uint8_t)((YSF_SYNC >> (39 - i)) & 1u);
    ysf_fich_encode(f, bits + 40);
    memcpy(bits + 240, payload, 720);
    ysf_bits_to_dibits(bits, YSF_FRAME_BITS, dibits);
}

void ysf_field_set(char *field, size_t n, const char *s)
{
    size_t i = 0;
    if (s)
        for (; i < n && s[i]; i++)
            field[i] = (char)toupper((unsigned char)s[i]);
    for (; i < n; i++)
        field[i] = ' ';
}

void ysf_csd_init(ysf_csd_t *c, const char *src, const char *dest)
{
    memset(c, 0, sizeof(*c));
    ysf_field_set(c->dest, 10, dest ? dest : "ALL");
    ysf_field_set(c->src, 10, src);
    ysf_field_set(c->down, 10, NULL);
    ysf_field_set(c->up, 10, NULL);
    memset(c->rem, ' ', sizeof(c->rem));
    c->valid = YSF_CSD_ALL;
}

void ysf_csd1_pack(const ysf_csd_t *c, uint8_t out[20])
{
    memcpy(out, c->dest, 10);
    memcpy(out + 10, c->src, 10);
}

void ysf_csd2_pack(const ysf_csd_t *c, uint8_t out[20])
{
    memcpy(out, c->down, 10);
    memcpy(out + 10, c->up, 10);
}

void ysf_csd3_pack(const ysf_csd_t *c, uint8_t out[20])
{
    memcpy(out, c->rem, 20);
}
