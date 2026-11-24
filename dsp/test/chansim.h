/* Host channel simulator for cfw/dsp tests: symbols -> transmitter pulse shaping -> FM (or H-DQPSK) complex
 * baseband at 48 kHz -> frequency offset + AWGN (C/N in 12.5 kHz) -> IF filter (+/-6.25 kHz) -> FM
 * discriminator -> [de-emphasis] -> 12-bit ADC counts (2048 + 0.25 count/Hz). */
#ifndef CHANSIM_H
#define CHANSIM_H
#include <stddef.h>
#include <stdint.h>
#include "dsp_demod.h"

typedef struct {
    float cnr_db;          /* carrier to noise in 12.5 kHz; >= 99 = no noise */
    float foff_hz;         /* frequency offset (DC at the discriminator) */
    float ppm;             /* transmitter symbol clock error */
    float dev_err;         /* deviation error, e.g. -0.1 = 10 % low */
    float deemph_tau_us;   /* radio audio de-emphasis, 0 = flat */
    uint32_t seed;
} chan_t;

#define SIM_FS 48000u

/* syms: one symbol per byte (dibits or bits). Writes ADC counts (uint16) to out, returns count. */
size_t chansim_run(dsp_mode_t mode, const uint8_t *syms, size_t n, const chan_t *ch, uint16_t *out, size_t max);
/* TX baseband (instantaneous frequency in Hz at fs_in, e.g. a cfw modulator / DAC capture) -> clock error
 * (resampled by ppm), deviation error, FM, offset, AWGN, IF filter, discriminator -> ADC counts at 48 kHz */
size_t chansim_fm(const float *hz, size_t n, uint32_t fs_in, const chan_t *ch, uint16_t *out, size_t max);
/* coherent reference for H-DQPSK: matched RRC filter on the complex signal + differential detection */
size_t chansim_dqpsk_coherent(const uint8_t *syms, size_t n, const chan_t *ch, uint8_t *out, size_t max);

#endif
