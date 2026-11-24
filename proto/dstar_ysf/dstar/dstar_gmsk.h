/*
 * dstar_gmsk.h - reference GMSK modem for D-STAR (4800 bit/s, BT = 0.5,
 * modulation index 0.5 -> +/-1200 Hz peak, '1' = positive deviation, JARL
 * STD6.0 Ap1.5).
 *
 * Works on "frequency" samples: the modulator outputs the instantaneous
 * frequency normalised to +/-1.0 (= +/-1200 Hz) for an FM modulator input;
 * the demodulator takes FM discriminator samples (any scale, DC is tracked).
 * Separate from the protocol code so a platform with a hardware GMSK modem
 * can skip it. Single precision float (Cortex-M33 has an FPU), no malloc.
 */
#ifndef DSTAR_GMSK_H
#define DSTAR_GMSK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSTAR_GMSK_MAX_SPS   16u
#define DSTAR_GMSK_SPAN      4u   /* Gaussian filter span in bits */
#define DSTAR_GMSK_MAX_TAPS  (DSTAR_GMSK_SPAN * DSTAR_GMSK_MAX_SPS + 1u)

typedef struct {
    unsigned sps;    /* samples per bit, 2..16 (e.g. 10 at 48 kHz) */
    unsigned ntaps;
    float taps[DSTAR_GMSK_MAX_TAPS];
    float hist[DSTAR_GMSK_MAX_TAPS];
    unsigned pos;
} dstar_gmsk_mod_t;

/* bt = 0.5 for D-STAR. Returns 0 on success. */
int dstar_gmsk_mod_init(dstar_gmsk_mod_t *m, unsigned sps, float bt);
/* n bits in -> n*sps samples out (+/-1.0 = full deviation). The output is
 * delayed by DSTAR_GMSK_SPAN/2 bits (filter group delay). */
size_t dstar_gmsk_mod(dstar_gmsk_mod_t *m, const uint8_t *bits, size_t n, float *out);

typedef struct {
    unsigned sps;
    int invert;      /* 1 if the discriminator polarity is reversed */
    float phase;     /* bit clock phase 0..1, decision when it wraps */
    float gain;      /* clock loop gain (default 0.05) */
    float dc;        /* DC estimate */
    float dc_alpha;
    float prev;      /* previous filtered sample */
    float ma[DSTAR_GMSK_MAX_SPS];
    float ma_sum;
    unsigned ma_pos;
} dstar_gmsk_demod_t;

int dstar_gmsk_demod_init(dstar_gmsk_demod_t *d, unsigned sps);
/* Discriminator samples in, hard bits out (one per byte). Returns the number
 * of bits written (at most n/sps + 1, never more than cap). */
size_t dstar_gmsk_demod(dstar_gmsk_demod_t *d, const float *in, size_t n, uint8_t *bits, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
