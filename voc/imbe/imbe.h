/*
 * imbe.h - IMBE 7200x4400 vocoder (P25 Phase 1 full rate, YSF VW): 88 info bits per 20 ms frame,
 * 8 kHz 16-bit PCM, 160 samples per frame.
 *
 * Frame bits: 88 bits packed MSB first in 11 bytes, in u-vector order u0..u7 (the "imbe_d" order of
 * mbelib and the output of p25_imbe_decode() in cfw/proto/p25). FEC (Golay/Hamming/PN/interleave) is
 * not part of the vocoder; see p25_imbe_encode()/p25_imbe_decode().
 *
 * Parameters per frame: fundamental w0 (b0, 8 bits), L harmonics (9..56), K voicing bands (b1),
 * log2 spectral amplitudes coded as a predicted residual: gain vector (b2..b7, 6-point DCT of the
 * block means) and higher-order DCT coefficients of 6 blocks (b8..b_L+1).
 *
 * - Dequantisation (imbe_unpack): follows the IMBE description; tables and the decode procedure
 *   come from mbelib (ISC licence, see imbe_tables.c / imbe_dec.c).
 * - Synthesis: own (phasor oscillators with crossfade for voiced harmonics, random-phase FFT noise
 *   with power-complementary overlap-add for unvoiced bands). Not bit-exact to TIA-102.BABA
 *   synthesis (the standard leaves no bit-exact requirement on synthesis).
 * - Encoder (analysis + quantisation): own; quantises with the same tables, closed loop with the
 *   decoder's predictor so a standard decoder reconstructs the same amplitudes.
 * Amplitude scale: PCM = 7 * synthesised value (mbelib convention). Unvoiced bands: the band noise
 * power is (IMBE_UV_GAIN * M)^2 / 2, i.e. the loudness mbelib's multisine unvoiced synthesis gives
 * (rms 1.657 M); the encoder uses the same convention, so cfw and mbelib decoders agree.
 */
#define IMBE_UV_GAIN 2.3434f
#ifndef IMBE_H
#define IMBE_H
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMBE_FRAME   160        /* samples per 20 ms frame */
#define IMBE_BYTES   11         /* 88 bits */
#define IMBE_MAXL    56

typedef struct {
    float   w0;                 /* fundamental, rad/sample */
    int     L;                  /* harmonics */
    int     K;                  /* voicing bands */
    uint8_t vl[IMBE_MAXL + 2];  /* voiced flag per harmonic 1..L */
    float   ml[IMBE_MAXL + 2];  /* spectral amplitudes 1..L */
    float   log2ml[IMBE_MAXL + 2];
} imbe_params_t;

void imbe_params_init(imbe_params_t *p);
/* 88 bits -> parameters, using prev (the previous frame's decoded parameters, unenhanced) for the
 * amplitude prediction. Returns 0, or 1 for an invalid frame (b0 > 207 / bad L): cur untouched. */
int  imbe_unpack(const uint8_t bits[IMBE_BYTES], const imbe_params_t *prev, imbe_params_t *cur);
/* b0 of a frame (8 bits) */
unsigned imbe_b0(const uint8_t bits[IMBE_BYTES]);
float    imbe_w0_of_b0(unsigned b0);
int      imbe_l_of_w0(float w0);

/* ------------------------------------------------------------------ decoder */
typedef struct {
    imbe_params_t pred;         /* previous frame, unenhanced (amplitude predictor state) */
    imbe_params_t syn;          /* previous frame, enhanced (synthesis state) */
    float    psi[IMBE_MAXL + 2];/* phase at the frame boundary per harmonic */
    float    phi[IMBE_MAXL + 2];
    float    noise_tail[IMBE_FRAME];   /* second half of the last unvoiced noise grain */
    uint32_t rng;
    int      repeat;            /* consecutive repeated (bad) frames */
} imbe_dec_t;

void imbe_dec_init(imbe_dec_t *d);
/* errs = corrected FEC bit errors of the frame (or -1 = unusable); frames with errs > 5, an invalid
 * b0 or errs < 0 repeat the last parameters; after 3 repeats the output mutes. Returns 0 = normal,
 * 1 = repeated, 2 = muted. */
int  imbe_decode(imbe_dec_t *d, const uint8_t bits[IMBE_BYTES], int errs, int16_t pcm[IMBE_FRAME]);
/* parameters -> 160 samples (used by imbe_decode; exposed for tests) */
void imbe_synth(imbe_dec_t *d, imbe_params_t *cur, int16_t pcm[IMBE_FRAME]);

/* ------------------------------------------------------------------ encoder */
#define IMBE_ENC_BUF 384        /* analysis buffer: 1 frame look-ahead (encoder delay 20 ms) */
typedef struct {
    float    buf[IMBE_ENC_BUF];
    float    hp_x, hp_y;        /* DC blocker */
    float    prev_period;       /* samples, 0 = none */
    uint8_t  prev_vband[13];
    imbe_params_t pred;         /* mirror of the decoder predictor */
    int      frames;
} imbe_enc_t;

void imbe_enc_init(imbe_enc_t *e);
/* 160 samples in -> 88 bits out (frame of the previous 20 ms: the encoder looks one frame ahead) */
void imbe_encode(imbe_enc_t *e, const int16_t pcm[IMBE_FRAME], uint8_t bits[IMBE_BYTES]);

/* internal helpers shared by encoder/decoder */
void imbe_fft512(float *re, float *im, bool inverse);
/* real 512-point transforms via one 256-point complex FFT (unnormalised, same scaling as fft512):
 * rfft: x[512] -> re/im[0..256];  irfft: re/im[0..256] (Hermitian half) -> x[512] */
void imbe_rfft512(const float *x, float *re, float *im);
void imbe_irfft512(const float *re, const float *im, float *x);
/* DCT basis cos(pi*k*(j+0.5)/J), k, j = 0..J-1 (row k), J = 1..12 */
const float *imbe_dct_basis(int J);
/* cos/sin of 2*pi*i/512, i = 0..511 */
float imbe_cos512(unsigned i);
float imbe_sin512(unsigned i);

#ifdef __cplusplus
}
#endif
#endif
