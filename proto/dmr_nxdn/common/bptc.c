/* DMR block product turbo codes, TS 102 361-1 B.1.1 / B.2.1 / B.2.2 / B.2.3. */
#include "fec.h"
#include "bits.h"
#include <string.h>

/* ---------------- BPTC (196,96) ----------------
 * Encode matrix: index 0 = R(3), then 13 rows x 15 columns row-major (index 1 + r*15 + c).
 * Rows 0..8: 11 data + Hamming(15,11); rows 9..12: column Hamming(13,9) parity.
 * Row 0 starts with R(2) R(1) R(0). Interleave index = index * 181 mod 196 = TX position
 * (position 0 = TX(195), first transmitted).
 */
#define BPTC_AT(m, r, c) (m)[1 + (r) * 15 + (c)]

void bptc196_encode(const uint8_t info[12], uint8_t bits[196])
{
    uint8_t m[196];
    memset(m, 0, sizeof m);
    unsigned ib = 0;
    for (int r = 0; r < 9; r++)
        for (int c = (r == 0 ? 3 : 0); c < 11; c++) BPTC_AT(m, r, c) = (uint8_t)bits_get(info, ib++);
    for (int r = 0; r < 9; r++) fec_hamming_15_11_enc(&BPTC_AT(m, r, 0));
    for (int c = 0; c < 15; c++) {
        uint8_t col[13];
        for (int r = 0; r < 9; r++) col[r] = BPTC_AT(m, r, c);
        fec_hamming_13_9_enc(col);
        for (int r = 9; r < 13; r++) BPTC_AT(m, r, c) = col[r];
    }
    for (unsigned i = 0; i < 196; i++) bits[(i * 181u) % 196u] = m[i];
}

int bptc196_decode(const uint8_t bits[196], uint8_t info[12])
{
    uint8_t m[196];
    int fixed = 0;
    for (unsigned i = 0; i < 196; i++) m[i] = bits[(i * 181u) % 196u] & 1u;
    for (int pass = 0; pass < 5; pass++) {
        int changed = 0;
        for (int c = 0; c < 15; c++) {
            uint8_t col[13];
            for (int r = 0; r < 13; r++) col[r] = BPTC_AT(m, r, c);
            if (fec_hamming_13_9_dec(col) == 1) {
                for (int r = 0; r < 13; r++) BPTC_AT(m, r, c) = col[r];
                changed++;
            }
        }
        for (int r = 0; r < 9; r++) if (fec_hamming_15_11_dec(&BPTC_AT(m, r, 0)) == 1) changed++;
        fixed += changed;
        if (!changed) break;
    }
    /* final check: every row and column must be a codeword */
    int bad = 0;
    for (int r = 0; r < 9; r++) {
        uint8_t row[15];
        memcpy(row, &BPTC_AT(m, r, 0), 15);
        if (fec_hamming_15_11_dec(row) != 0) bad = 1;
    }
    /* columns that stay inconsistent while every data row checks out only have errors in the
     * column parity rows 9..12 (no row code there): the data is still good unless many fail */
    int badcols = 0;
    for (int c = 0; c < 15; c++) {
        uint8_t col[13];
        for (int r = 0; r < 13; r++) col[r] = BPTC_AT(m, r, c);
        if (fec_hamming_13_9_dec(col) != 0) badcols++;
    }
    if (badcols > 2) bad = 1;
    memset(info, 0, 12);
    unsigned ib = 0;
    for (int r = 0; r < 9; r++)
        for (int c = (r == 0 ? 3 : 0); c < 11; c++) bits_put(info, ib++, BPTC_AT(m, r, c));
    return bad ? -1 : fixed;
}

/* ---------------- embedded LC (B.2.1, figure B.3) ----------------
 * 8 rows x 16: rows 0..6 = 11 bits + Hamming(16,11,4), row 7 = even column parity.
 * Rows 2..6 carry 10 LC bits + one checksum bit. TX bit k = matrix[k % 8][k / 8].
 */
void emblc_encode(const uint8_t lc[9], uint8_t bits[128])
{
    uint8_t m[8][16];
    memset(m, 0, sizeof m);
    uint8_t cs = cs5_dmr(lc);
    unsigned lb = 0;
    for (int r = 0; r < 7; r++) {
        int nlc = r < 2 ? 11 : 10;
        for (int c = 0; c < nlc; c++) m[r][c] = (uint8_t)bits_get(lc, lb++);
        if (r >= 2) m[r][10] = (uint8_t)((cs >> (6 - r)) & 1u);     /* CS(4) in row 2 .. CS(0) in row 6 */
        fec_hamming_16_11_enc(m[r]);
    }
    for (int c = 0; c < 16; c++) {
        uint8_t p = 0;
        for (int r = 0; r < 7; r++) p ^= m[r][c];
        m[7][c] = p;
    }
    for (unsigned k = 0; k < 128; k++) bits[k] = m[k % 8][k / 8];
}

int emblc_decode(const uint8_t bits[128], uint8_t lc[9])
{
    uint8_t m[8][16];
    for (unsigned k = 0; k < 128; k++) m[k % 8][k / 8] = bits[k] & 1u;
    for (int r = 0; r < 7; r++) if (fec_hamming_16_11_dec(m[r]) < 0) return -1;
    int badcol = 0;
    for (int c = 0; c < 16; c++) {
        uint8_t p = 0;
        for (int r = 0; r < 8; r++) p ^= m[r][c];
        badcol += p;
    }
    if (badcol > 1) return -1;           /* one bad column = error in the parity row itself */
    memset(lc, 0, 9);
    unsigned lb = 0;
    uint8_t cs = 0;
    for (int r = 0; r < 7; r++) {
        int nlc = r < 2 ? 11 : 10;
        for (int c = 0; c < nlc; c++) bits_put(lc, lb++, m[r][c]);
        if (r >= 2) cs = (uint8_t)((cs << 1) | m[r][10]);
    }
    return cs == cs5_dmr(lc) ? 0 : -1;
}

/* ---------------- CACH short LC (B.2.3, figure B.6) ----------------
 * 4 rows x 17: rows 0..2 = 12 bits + Hamming(17,12,3), row 3 = even column parity.
 * Info = LC(27..0) then CRC8. TX bit k = matrix[k % 4][k / 4].
 */
void cach_slc_encode(const uint8_t slc[4], uint8_t bits[68])
{
    uint8_t info[36], m[4][17];
    for (int i = 0; i < 28; i++) info[i] = (uint8_t)bits_get(slc, 4u + (unsigned)i);
    uint8_t crc = crc8_dmr(info, 28);
    for (int i = 0; i < 8; i++) info[28 + i] = (uint8_t)((crc >> (7 - i)) & 1u);
    memset(m, 0, sizeof m);
    for (int r = 0; r < 3; r++) {
        memcpy(m[r], &info[r * 12], 12);
        fec_hamming_17_12_enc(m[r]);
    }
    for (int c = 0; c < 17; c++) m[3][c] = (uint8_t)(m[0][c] ^ m[1][c] ^ m[2][c]);
    for (unsigned k = 0; k < 68; k++) bits[k] = m[k % 4][k / 4];
}

int cach_slc_decode(const uint8_t bits[68], uint8_t slc[4])
{
    uint8_t m[4][17], info[36];
    for (unsigned k = 0; k < 68; k++) m[k % 4][k / 4] = bits[k] & 1u;
    for (int r = 0; r < 3; r++) if (fec_hamming_17_12_dec(m[r]) < 0) return -1;
    int badcol = 0;
    for (int c = 0; c < 17; c++) badcol += m[0][c] ^ m[1][c] ^ m[2][c] ^ m[3][c];
    if (badcol > 1) return -1;           /* the CRC-8 still guards the result */
    for (int r = 0; r < 3; r++) memcpy(&info[r * 12], m[r], 12);
    uint8_t crc = (uint8_t)bits_arr_val(&info[28], 8);
    if (crc != crc8_dmr(info, 28)) return -1;
    memset(slc, 0, 4);
    for (int i = 0; i < 28; i++) bits_put(slc, 4u + (unsigned)i, info[i]);
    return 0;
}

/* ---------------- single burst BPTC (B.2.2) ----------------
 * 2 rows x 16: row 0 = SB/RC(10..0) + H(16,11,4); row 1 = parity (even: copy, RC: odd = inverted).
 * index = col * 2 + row; interleave index = index * 17 mod 32 = TX position.
 */
void sbbptc_encode(uint16_t info11, int rc, uint8_t bits[32])
{
    uint8_t row[16];
    for (int i = 0; i < 11; i++) row[i] = (uint8_t)((info11 >> (10 - i)) & 1u);
    fec_hamming_16_11_enc(row);
    for (unsigned idx = 0; idx < 32; idx++) {
        unsigned c = idx / 2, r = idx % 2;
        uint8_t v = row[c];
        if (r && rc) v ^= 1u;
        bits[(idx * 17u) % 32u] = v;
    }
}

int sbbptc_decode(const uint8_t bits[32], int rc, uint16_t *info11)
{
    uint8_t row[16], par[16];
    for (unsigned idx = 0; idx < 32; idx++) {
        unsigned c = idx / 2, r = idx % 2;
        uint8_t v = bits[(idx * 17u) % 32u] & 1u;
        if (r) par[c] = (uint8_t)(rc ? v ^ 1u : v); else row[c] = v;
    }
    int e = fec_hamming_16_11_dec(row);
    if (e < 0) return -1;
    int diff = 0;
    for (int c = 0; c < 16; c++) diff += row[c] != par[c];
    if (diff > 2) return -1;
    *info11 = (uint16_t)bits_arr_val(row, 11);
    return e + diff;
}
