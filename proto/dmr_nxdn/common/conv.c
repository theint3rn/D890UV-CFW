/* Rate 1/2, K = 5 convolutional code with puncturing (NXDN TS 1-A 4.5.1 / 4.5.2).
 * G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4. Hard-decision Viterbi, erasures for
 * punctured positions. Stack use ~ nout * 2 bytes (decisions) + small.
 */
#include "fec.h"
#include <string.h>

#define CONV_MAX_STEPS 256

/* state s: bit0 = D^1 (previous input), bit1 = D^2, bit2 = D^3, bit3 = D^4 */
static inline void conv_out(unsigned s, unsigned b, unsigned *g1, unsigned *g2)
{
    unsigned d1 = s & 1u, d2 = (s >> 1) & 1u, d3 = (s >> 2) & 1u, d4 = (s >> 3) & 1u;
    *g1 = b ^ d3 ^ d4;
    *g2 = b ^ d1 ^ d2 ^ d4;
}

unsigned conv_k5_encode(const uint8_t *in, unsigned nin, uint8_t *out, const conv_punct_t *p)
{
    unsigned s = 0, n = 0, k = 0;
    for (unsigned i = 0; i < nin; i++) {
        unsigned b = in[i] & 1u, g[2];
        conv_out(s, b, &g[0], &g[1]);
        for (int j = 0; j < 2; j++, k++) {
            if (!p || p->keep[k % p->period]) out[n++] = (uint8_t)g[j];
        }
        s = ((s << 1) | b) & 0xFu;
    }
    return n;
}

int conv_k5_decode(const uint8_t *in, unsigned nin, uint8_t *out, unsigned nout, const conv_punct_t *p)
{
    uint16_t dec[CONV_MAX_STEPS];     /* bit s set: survivor into state s came from the "high" predecessor */
    int metric[16], nm[16];
    unsigned pos = 0, k = 0;
    if (nout > CONV_MAX_STEPS) return -1;
    for (int s = 0; s < 16; s++) metric[s] = s ? 10000 : 0;
    for (unsigned t = 0; t < nout; t++) {
        int rx[2];        /* -1 = erased */
        for (int j = 0; j < 2; j++, k++) {
            if (!p || p->keep[k % p->period]) rx[j] = pos < nin ? (in[pos++] & 1) : -1;
            else rx[j] = -1;
        }
        dec[t] = 0;
        for (unsigned ns = 0; ns < 16; ns++) {
            /* ns = ((s << 1) | b) & 0xF: b = ns & 1, predecessors s = ns >> 1 | {0, 8} */
            unsigned b = ns & 1u;
            int best = 1 << 30, bsel = 0;
            for (unsigned hi = 0; hi < 2; hi++) {
                unsigned s = (ns >> 1) | (hi << 3), g1, g2;
                conv_out(s, b, &g1, &g2);
                int m = metric[s];
                if (rx[0] >= 0 && (unsigned)rx[0] != g1) m++;
                if (rx[1] >= 0 && (unsigned)rx[1] != g2) m++;
                if (m < best) { best = m; bsel = (int)hi; }
            }
            nm[ns] = best;
            if (bsel) dec[t] |= (uint16_t)(1u << ns);
        }
        memcpy(metric, nm, sizeof metric);
    }
    /* tail bits force the final state to 0 */
    unsigned s = 0;
    int result = metric[0];
    for (int t = (int)nout - 1; t >= 0; t--) {
        out[t] = (uint8_t)(s & 1u);
        unsigned hi = (dec[t] >> s) & 1u;
        s = (s >> 1) | (hi << 3);
    }
    return result;
}
