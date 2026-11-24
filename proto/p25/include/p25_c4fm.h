/*
 * p25_c4fm.h - symbol-level helpers for the "MCU-DSP" path (FM discriminator
 * samples in, FM modulator baseband out). Not needed if the radio hardware
 * delivers/accepts dibits itself.
 *
 * C4FM (TIA-102.BAAA 8): 4800 sym/s, dibit 01=+3 00=+1 10=-1 11=-3
 * (+/-1800 Hz, +/-600 Hz deviation). The modulator applies a raised-cosine
 * (alpha 0.2) pulse; the inverse-sinc shaping of the standard's transmit
 * filter is left to the FM modulator path. Phase 2 outbound (H-DQPSK) uses the
 * same 4-level discriminator view at 6000 sym/s, so the same slicer serves a
 * discriminator-based H-DQPSK receiver (sps chosen accordingly).
 *
 * Demod timing: no loop. Each sample phase keeps E[y^2] and E[y^4]; the eye
 * centre is the phase with the lowest kurtosis E[y^4]/E[y^2]^2 (4-level data
 * gives 1.64, ISI pushes it towards 3). The phase moves one step at a time
 * with hysteresis; wrap-arounds insert/drop one symbol so the stream never
 * slips. P25 clocks are +/-2 ppm, so this tracks drift easily.
 */
#ifndef P25_C4FM_H
#define P25_C4FM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25_C4FM_MAX_SPS  20
#define P25_C4FM_SPAN     8      /* RC filter span in symbols */

static inline int p25_dibit_level(uint8_t d)     /* +3 +1 -1 -3 */
{
    static const int8_t lv[4] = { 1, 3, -1, -3 };
    return lv[d & 3];
}
static inline uint8_t p25_level_dibit(int level) /* nearest */
{
    return level >= 2 ? 1 : level >= 0 ? 0 : level >= -2 ? 2 : 3;
}

typedef struct {
    unsigned sps;
    float    taps[P25_C4FM_SPAN * P25_C4FM_MAX_SPS];
    float    hist[P25_C4FM_SPAN];
} p25_c4fm_mod_t;

/* sps = samples per symbol (e.g. 10 at 48 kHz for 4800 sym/s). */
int    p25_c4fm_mod_init(p25_c4fm_mod_t *m, unsigned sps);
/* Writes n*sps samples; `scale` = sample value of a +1 level. */
size_t p25_c4fm_mod(p25_c4fm_mod_t *m, const uint8_t *dibits, size_t n, int16_t *out, float scale);

typedef struct {
    unsigned sps;
    unsigned ph;            /* sample phase within the symbol, 0..sps-1 */
    unsigned best;          /* sampling phase in use */
    unsigned skip;
    float    energy[P25_C4FM_MAX_SPS + 10];  /* per-phase mean y^2 */
    float    energy4[P25_C4FM_MAX_SPS + 10]; /* per-phase mean y^4 */
    float    dc;            /* DC (frequency offset) estimate */
    float    amp;           /* mean |y| = 2 * level unit */
    float    timing_gain;   /* per-phase averaging constant */
    float    level_gain;
} p25_c4fm_demod_t;

int    p25_c4fm_demod_init(p25_c4fm_demod_t *d, unsigned sps);
/* Feeds n discriminator samples; writes recovered dibits to out (room for
 * n/sps + 2) and optional symbol values (soft, in level units) to soft.
 * Returns number of dibits. */
size_t p25_c4fm_demod(p25_c4fm_demod_t *d, const int16_t *in, size_t n, uint8_t *out, float *soft);
/* Re-centre the slicer from a found frame sync: pass the 24 soft values the
 * demodulator produced for the P25_SYNC_P1 symbols (least-squares DC/level). */
void   p25_c4fm_demod_train(p25_c4fm_demod_t *d, const float *sync_soft);

#ifdef __cplusplus
}
#endif
#endif
