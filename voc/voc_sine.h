/* cfw "sine" vocoder: a small harmonic (MBE-style) sinusoidal coder written for this project.
 *
 * TEST ONLY - NOT INTEROPERABLE. It fills the payload of the 49-bit AMBE+2 / AMBE 2020 frame and of the
 * 88-bit IMBE frame so the cfw TX/RX chain can carry audio end to end (and a 440 Hz test tone survives with
 * < 0.5 Hz error), but its bit layout is its own: a real DMR / P25 / NXDN / D-STAR / YSF radio will play
 * garbage. Real AMBE+2 needs the SCT3288 hardware vocoder (or a licensed DVSI codec); a TIA-102.BABA
 * bit-exact IMBE needs the standard's quantiser tables, which this project does not have.
 *
 * Model (per 20 ms frame, 160 samples at 8 kHz):
 *   pitch f0 on the IMBE grid  f0 = 16000 / (b0 + 39.5) Hz, b0 = 0..207 (8 bits)  -> 65 .. 405 Hz
 *   all-voiced harmonics k = 1..L (L = 3800 / f0, max 58), amplitude per harmonic in dB below the frame
 *   peak, bits per harmonic = what the payload leaves (49: 5-bit gain + 36 bits, 88: 6-bit gain + 74 bits)
 * Analysis: Hann 256 + 512-point FFT, harmonic-sum pitch search, the dominant spectral peak (parabolic
 * interpolation) picks among equal-score pitches so a pure tone lands exactly on a harmonic.
 * Synthesis: phase-continuous harmonic oscillators, amplitudes interpolated across the frame. */
#ifndef CFW_VOC_SINE_H
#define CFW_VOC_SINE_H
#include <stdint.h>
#include <stddef.h>

#define VOC_FRAME   160
#define VOC_MAXH    64

typedef struct {
    float hist[96];            /* previous samples for the 256-sample window */
} voc_sine_enc_t;

typedef struct {
    float phase[VOC_MAXH];
    float amp[VOC_MAXH];       /* last frame's amplitudes (linear, 1.0 = full scale sine) */
    float f0;
} voc_sine_dec_t;

void voc_sine_enc_init(voc_sine_enc_t *e);
void voc_sine_dec_init(voc_sine_dec_t *d);
/* nbits = 49 or 88; bits packed MSB first (49 bits = 7 bytes, 88 bits = 11 bytes) */
void voc_sine_encode(voc_sine_enc_t *e, const int16_t pcm[VOC_FRAME], uint8_t *bits, int nbits);
void voc_sine_decode(voc_sine_dec_t *d, const uint8_t *bits, int nbits, int16_t pcm[VOC_FRAME]);

#endif
