/* DMR rate 3/4 trellis code, TS 102 361-1 B.2.4 (tables B.7 - B.10). Viterbi decoder. */
#include "fec.h"
#include "bits.h"
#include <string.h>

/* Table B.7: constellation point for [state][input tribit]; next state = input */
static const uint8_t TRELLIS_FSM[8][8] = {
    { 0, 8, 4, 12, 2, 10, 6, 14 },
    { 4, 12, 2, 10, 6, 14, 0, 8 },
    { 1, 9, 5, 13, 3, 11, 7, 15 },
    { 5, 13, 3, 11, 7, 15, 1, 9 },
    { 3, 11, 7, 15, 1, 9, 5, 13 },
    { 7, 15, 1, 9, 5, 13, 3, 11 },
    { 2, 10, 6, 14, 0, 8, 4, 12 },
    { 6, 14, 0, 8, 4, 12, 2, 10 },
};

/* Table B.8 constellation -> dibit pair, symbols mapped to dibits (+3=01 +1=00 -1=10 -3=11) */
#define P3 1
#define P1 0
#define M1 2
#define M3 3
static const uint8_t TRELLIS_PT[16][2] = {
    { P1, M1 }, { M1, M1 }, { P3, M3 }, { M3, M3 }, { M3, M1 }, { P3, M1 }, { M1, M3 }, { P1, M3 },
    { M3, P3 }, { P3, P3 }, { M1, P1 }, { P1, P1 }, { P1, P3 }, { M1, P3 }, { P3, P1 }, { M3, P1 },
};

/* Table B.9 / B.10: transmit position i carries Enc_Dibit(TRELLIS_IL[i]) */
static void trellis_il(uint8_t il[98])
{
    unsigned n = 0;
    for (unsigned k = 0; k < 4; k++)
        for (unsigned e = 2 * k; e < 98; e += 8) { il[n++] = (uint8_t)e; il[n++] = (uint8_t)(e + 1); }
}

void trellis34_encode(const uint8_t in[18], uint8_t dibits[98])
{
    uint8_t enc[98], il[98];
    unsigned state = 0;
    for (unsigned t = 0; t < 49; t++) {
        unsigned tri = t < 48 ? bits_field(in, t * 3u, 3) : 0;   /* tribit 48 = flush 000 */
        unsigned pt = TRELLIS_FSM[state][tri];
        enc[2 * t] = TRELLIS_PT[pt][0];
        enc[2 * t + 1] = TRELLIS_PT[pt][1];
        state = tri;
    }
    trellis_il(il);
    for (unsigned i = 0; i < 98; i++) dibits[i] = enc[il[i]];
}

/* distance between two 4FSK symbols in levels (+3 +1 -1 -3): adjacent-level errors cost 1 */
static int dibit_dist(uint8_t a, uint8_t b)
{
    static const int8_t level[4] = { 2, 3, 1, 0 };      /* dibit 00=+1 01=+3 10=-1 11=-3 */
    int d = level[a & 3u] - level[b & 3u];
    return d < 0 ? -d : d;
}

int trellis34_decode(const uint8_t dibits[98], uint8_t out[18])
{
    uint8_t enc[98], il[98];
    uint8_t prev[49][8];
    int metric[8], nm[8];
    trellis_il(il);
    for (unsigned i = 0; i < 98; i++) enc[il[i]] = dibits[i] & 3u;
    for (int s = 0; s < 8; s++) metric[s] = s ? 1000 : 0;
    for (unsigned t = 0; t < 49; t++) {
        for (int s = 0; s < 8; s++) nm[s] = 1 << 20;
        for (int s = 0; s < 8; s++) {
            if (metric[s] >= 1000) continue;
            for (int in = 0; in < 8; in++) {
                if (t == 48 && in) break;                  /* flush tribit is 0 */
                unsigned pt = TRELLIS_FSM[s][in];
                int m = metric[s] + dibit_dist(enc[2 * t], TRELLIS_PT[pt][0]) +
                        dibit_dist(enc[2 * t + 1], TRELLIS_PT[pt][1]);
                if (m < nm[in]) { nm[in] = m; prev[t][in] = (uint8_t)s; }
            }
        }
        for (int s = 0; s < 8; s++) metric[s] = nm[s] >= (1 << 20) ? 1000 + (int)t : nm[s];
    }
    if (metric[0] >= 1000) return -1;
    /* trace back from state 0 at t = 48; the state after step t is that step's input tribit */
    uint8_t tri[49];
    int s = 0;
    for (int t = 48; t >= 0; t--) { tri[t] = (uint8_t)s; s = prev[t][s]; }
    memset(out, 0, 18);
    for (unsigned t = 0; t < 48; t++) bits_set_field(out, t * 3u, 3, tri[t]);
    return metric[0];
}
