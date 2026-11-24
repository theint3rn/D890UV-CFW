/*
 * dstar_header.c - D-STAR radio header: CRC, FEC, interleave, scramble, and
 * callsign routing helpers.
 *
 * Source: JARL STD6.0 (English) section 4 and Ap1/Ap2:
 *  - P_FCS: CRC-CCITT, generator 0x8408 (bit-reversed 0x1021); X.25 style
 *    (init FFFF, inverted), sent low byte first like every byte LSB first.
 *  - FEC: rate 1/2, K=3, G1 = 1+D+D^2, G2 = 1+D^2, registers zero, header fed
 *    LSB first, 2 zero tail bits: 330 -> 660 bits.
 *  - Interleave: 24-row matrix, written top-to-bottom column by column, read
 *    row by row; rows 0..11 have 28 entries, rows 12..23 have 27 (table in
 *    Ap2.2: row r, column c holds input bit 24*c + r).
 *  - Scrambler: S(x) = x^7 + x^4 + 1, initialised to all ones, over the
 *    header after bit/frame sync.
 */
#include "dstar.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../common/dv_common.h"

const uint8_t DSTAR_SLOW_SCRAMBLE[3] = {0x70, 0x4F, 0x93};
const uint8_t DSTAR_AMBE_SILENCE[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};

static const dv_conv_t DSTAR_CONV = {3, 0x7, 0x5};

uint16_t dstar_header_crc(const uint8_t raw[DSTAR_HEADER_BYTES - 2])
{
    return dv_crc16_x25(raw, DSTAR_HEADER_BYTES - 2);
}

void dstar_header_pack(const dstar_header_t *h, uint8_t out[DSTAR_HEADER_BYTES])
{
    memcpy(out, h->flag, 3);
    memcpy(out + 3, h->rpt2, 8);
    memcpy(out + 11, h->rpt1, 8);
    memcpy(out + 19, h->ur, 8);
    memcpy(out + 27, h->my, 8);
    memcpy(out + 35, h->my_sfx, 4);
    uint16_t crc = dstar_header_crc(out);
    out[39] = (uint8_t)(crc & 0xFFu);
    out[40] = (uint8_t)(crc >> 8);
}

bool dstar_header_unpack(const uint8_t in[DSTAR_HEADER_BYTES], dstar_header_t *h)
{
    memcpy(h->flag, in, 3);
    memcpy(h->rpt2, in + 3, 8);
    memcpy(h->rpt1, in + 11, 8);
    memcpy(h->ur, in + 19, 8);
    memcpy(h->my, in + 27, 8);
    memcpy(h->my_sfx, in + 35, 4);
    uint16_t crc = dstar_header_crc(in);
    return in[39] == (uint8_t)(crc & 0xFFu) && in[40] == (uint8_t)(crc >> 8);
}

/* Air position j <- code bit index. Rows 0..11: 28 columns, rows 12..23: 27. */
static uint16_t ilv_src(unsigned j)
{
    unsigned r, c;
    if (j < 12u * 28u) {
        r = j / 28u;
        c = j % 28u;
    } else {
        j -= 12u * 28u;
        r = 12u + j / 27u;
        c = j % 27u;
    }
    return (uint16_t)(24u * c + r);
}

void dstar_interleave(const uint8_t in[DSTAR_HEADER_BITS], uint8_t out[DSTAR_HEADER_BITS])
{
    for (unsigned j = 0; j < DSTAR_HEADER_BITS; j++)
        out[j] = in[ilv_src(j)];
}

void dstar_deinterleave(const uint8_t in[DSTAR_HEADER_BITS], uint8_t out[DSTAR_HEADER_BITS])
{
    for (unsigned j = 0; j < DSTAR_HEADER_BITS; j++)
        out[ilv_src(j)] = in[j];
}

void dstar_scramble(uint8_t *bits, size_t n)
{
    unsigned st = 0x7Fu;
    for (size_t i = 0; i < n; i++) {
        unsigned b = ((st >> 6) ^ (st >> 3)) & 1u;
        st = ((st << 1) | b) & 0x7Fu;
        bits[i] ^= (uint8_t)b;
    }
}

void dstar_header_encode(const uint8_t raw[DSTAR_HEADER_BYTES], uint8_t bits[DSTAR_HEADER_BITS])
{
    uint8_t in[DSTAR_HEADER_BYTES * 8 + 2];
    uint8_t code[DSTAR_HEADER_BITS];
    dv_unpack_lsb(raw, DSTAR_HEADER_BYTES * 8, in);
    in[328] = 0;
    in[329] = 0;
    dv_conv_encode(&DSTAR_CONV, in, 330, code);
    dstar_interleave(code, bits);
    dstar_scramble(bits, DSTAR_HEADER_BITS);
}

int dstar_header_decode(const uint8_t bits[DSTAR_HEADER_BITS], uint8_t raw[DSTAR_HEADER_BYTES])
{
    uint8_t tmp[DSTAR_HEADER_BITS];
    uint8_t code[DSTAR_HEADER_BITS];
    uint8_t out[330];
    memcpy(tmp, bits, DSTAR_HEADER_BITS);
    dstar_scramble(tmp, DSTAR_HEADER_BITS);
    dstar_deinterleave(tmp, code);
    int err = dv_viterbi_decode(&DSTAR_CONV, code, 330, 0, out);
    dv_pack_lsb(out, DSTAR_HEADER_BYTES * 8, raw);
    return err;
}

/* ---- callsigns ----------------------------------------------------------- */

void dstar_callsign_set(char *field, size_t n, const char *s)
{
    size_t i = 0;
    if (s)
        for (; i < n && s[i]; i++)
            field[i] = (char)toupper((unsigned char)s[i]);
    for (; i < n; i++)
        field[i] = ' ';
}

void dstar_callsign_get(const char *field, size_t n, char *out)
{
    size_t len = n;
    while (len > 0 && (field[len - 1] == ' ' || field[len - 1] == '\0'))
        len--;
    memcpy(out, field, len);
    out[len] = '\0';
}

static void hdr_common(dstar_header_t *h, const char *my, const char *sfx, const char *ur)
{
    memset(h, 0, sizeof(*h));
    dstar_callsign_set(h->my, 8, my);
    dstar_callsign_set(h->my_sfx, 4, sfx);
    if (ur)
        dstar_callsign_set(h->ur, 8, ur);
    else
        dstar_ur_cq(h->ur);
}

void dstar_header_simplex(dstar_header_t *h, const char *my, const char *sfx, const char *ur)
{
    hdr_common(h, my, sfx, ur);
    dstar_callsign_set(h->rpt1, 8, "DIRECT");
    dstar_callsign_set(h->rpt2, 8, "DIRECT");
}

void dstar_header_repeater(dstar_header_t *h, const char *my, const char *sfx, const char *ur,
                           const char *rpt, bool via_gateway)
{
    hdr_common(h, my, sfx, ur);
    h->flag[0] = DSTAR_F1_REPEATER;
    dstar_callsign_set(h->rpt1, 8, rpt);
    dstar_callsign_set(h->rpt2, 8, rpt);
    if (via_gateway)
        h->rpt2[7] = 'G';
}

void dstar_ur_cq(char ur[8]) { dstar_callsign_set(ur, 8, "CQCQCQ"); }
void dstar_ur_call(char ur[8], const char *call) { dstar_callsign_set(ur, 8, call); }

void dstar_ur_zone(char ur[8], const char *rpt)
{
    /* "/" + first 7 chars of the 8-char repeater field; module letter kept
     * in the last position: "W1ABC  B" -> "/W1ABC B" */
    char f[8];
    dstar_callsign_set(f, 8, rpt);
    ur[0] = '/';
    memcpy(ur + 1, f, 6);
    ur[7] = f[7];
}

void dstar_ur_link(char ur[8], const char *reflector, char module)
{
    dstar_callsign_set(ur, 8, reflector);
    ur[6] = (char)toupper((unsigned char)module);
    ur[7] = 'L';
}

static void ur_cmd(char ur[8], char c)
{
    memset(ur, ' ', 8);
    ur[7] = c;
}

void dstar_ur_unlink(char ur[8]) { ur_cmd(ur, 'U'); }
void dstar_ur_info(char ur[8]) { ur_cmd(ur, 'I'); }
void dstar_ur_echo(char ur[8]) { ur_cmd(ur, 'E'); }

dstar_ur_kind_t dstar_ur_classify(const char ur[8])
{
    if (memcmp(ur, "CQCQCQ", 6) == 0)
        return DSTAR_UR_CQ;
    if (ur[0] == '/')
        return DSTAR_UR_ZONE;
    bool blank7 = true;
    for (int i = 0; i < 7; i++)
        if (ur[i] != ' ')
            blank7 = false;
    if (blank7) {
        switch (ur[7]) {
        case 'U': return DSTAR_UR_UNLINK;
        case 'I': return DSTAR_UR_INFO;
        case 'E': return DSTAR_UR_ECHO;
        default: return DSTAR_UR_OTHER;
        }
    }
    if (ur[7] == 'L' && ur[6] >= 'A' && ur[6] <= 'Z')
        return DSTAR_UR_LINK;
    return DSTAR_UR_CALLSIGN;
}

bool dstar_header_is_direct(const dstar_header_t *h)
{
    return !(h->flag[0] & DSTAR_F1_REPEATER) || memcmp(h->rpt1, "DIRECT", 6) == 0;
}

bool dstar_header_via_gateway(const dstar_header_t *h) { return h->rpt2[7] == 'G'; }

void dstar_header_turnaround(const dstar_header_t *in, dstar_header_t *out)
{
    dstar_header_t t = *in;
    memcpy(t.rpt1, in->rpt2, 8);
    memcpy(t.rpt2, in->rpt1, 8);
    t.flag[0] |= DSTAR_F1_REPEATER;
    *out = t;
}

/* ---- D-PRS ---------------------------------------------------------------- */

size_t dstar_dprs_build(char *out, size_t cap, const char *aprs)
{
    size_t n = strlen(aprs);
    if (cap < n + 12)
        return 0;
    uint16_t crc = dv_crc16_x25((const uint8_t *)aprs, n);
    int k = snprintf(out, cap, "$$CRC%04X,%s\r", (unsigned)crc, aprs);
    return (k > 0 && (size_t)k < cap) ? (size_t)k : 0;
}

bool dstar_dprs_check(const char *line, size_t len)
{
    if (len < 11 || memcmp(line, "$$CRC", 5) != 0 || line[9] != ',')
        return false;
    unsigned v = 0;
    for (int i = 5; i < 9; i++) {
        char c = (char)toupper((unsigned char)line[i]);
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return false;
    }
    size_t end = len;
    while (end > 10 && (line[end - 1] == '\r' || line[end - 1] == '\n'))
        end--;
    return dv_crc16_x25((const uint8_t *)line + 10, end - 10) == v;
}
