/*
 * dstar_gmsk.c - reference GMSK modulator/demodulator (see dstar_gmsk.h).
 *
 * Modulator: NRZ (+/-1) held for sps samples, filtered by a Gaussian FIR
 * (sigma = sqrt(ln 2) / (2 pi BT) bit periods, span 4 bits, taps summed to
 * 1), so a long run of ones gives +1.0. Integrating this frequency gives the
 * GMSK phase; an FM modulator does that integration itself.
 *
 * Demodulator: DC tracking, moving average over one bit (matched to the
 * rectangular part of the pulse), sign decision at the bit centre, bit clock
 * from zero crossings (a crossing should fall half a bit away from the
 * decision instant).
 */
#include "dstar_gmsk.h"

#include <math.h>
#include <string.h>

int dstar_gmsk_mod_init(dstar_gmsk_mod_t *m, unsigned sps, float bt)
{
    if (sps < 2 || sps > DSTAR_GMSK_MAX_SPS || bt <= 0.0f)
        return -1;
    memset(m, 0, sizeof(*m));
    m->sps = sps;
    m->ntaps = DSTAR_GMSK_SPAN * sps + 1u;
    const float sigma = sqrtf(logf(2.0f)) / (2.0f * 3.14159265f * bt); /* in bits */
    float sum = 0.0f;
    for (unsigned i = 0; i < m->ntaps; i++) {
        float t = ((float)i - (float)(m->ntaps - 1u) / 2.0f) / (float)sps;
        m->taps[i] = expf(-t * t / (2.0f * sigma * sigma));
        sum += m->taps[i];
    }
    for (unsigned i = 0; i < m->ntaps; i++)
        m->taps[i] /= sum;
    for (unsigned i = 0; i < m->ntaps; i++)
        m->hist[i] = -1.0f; /* idle as a run of zeros */
    return 0;
}

size_t dstar_gmsk_mod(dstar_gmsk_mod_t *m, const uint8_t *bits, size_t n, float *out)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        float x = bits[i] ? 1.0f : -1.0f;
        for (unsigned s = 0; s < m->sps; s++) {
            m->hist[m->pos] = x;
            m->pos = (m->pos + 1u) % m->ntaps;
            float acc = 0.0f;
            unsigned p = m->pos; /* oldest sample */
            for (unsigned t = 0; t < m->ntaps; t++) {
                acc += m->taps[t] * m->hist[p];
                if (++p == m->ntaps)
                    p = 0;
            }
            out[k++] = acc;
        }
    }
    return k;
}

int dstar_gmsk_demod_init(dstar_gmsk_demod_t *d, unsigned sps)
{
    if (sps < 2 || sps > DSTAR_GMSK_MAX_SPS)
        return -1;
    memset(d, 0, sizeof(*d));
    d->sps = sps;
    d->gain = 0.05f;
    d->dc_alpha = 1.0f / (64.0f * (float)sps); /* ~64 bit time constant */
    return 0;
}

size_t dstar_gmsk_demod(dstar_gmsk_demod_t *d, const float *in, size_t n, uint8_t *bits, size_t cap)
{
    size_t nb = 0;
    const float step = 1.0f / (float)d->sps;
    for (size_t i = 0; i < n; i++) {
        float x = in[i];
        d->dc += d->dc_alpha * (x - d->dc);
        x -= d->dc;
        d->ma_sum += x - d->ma[d->ma_pos];
        d->ma[d->ma_pos] = x;
        d->ma_pos = (d->ma_pos + 1u) % d->sps;
        float y = d->ma_sum;

        float ph = d->phase + step;
        if ((y >= 0.0f) != (d->prev >= 0.0f)) {
            /* zero crossing between previous and current sample */
            float den = d->prev - y;
            float frac = (den != 0.0f) ? d->prev / den : 0.5f; /* 0..1 */
            float cross = ph - (1.0f - frac) * step;
            if (cross < 0.0f)
                cross += 1.0f;
            float err = cross - 0.5f; /* ideal crossing at mid-way */
            ph -= d->gain * err;
        }
        d->prev = y;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            if (nb < cap) {
                int b = y > 0.0f;
                bits[nb++] = (uint8_t)(d->invert ? !b : b);
            }
        } else if (ph < 0.0f) {
            ph += 1.0f;
        }
        d->phase = ph;
    }
    return nb;
}
