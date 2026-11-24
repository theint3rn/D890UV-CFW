/* cfw-platform name for the IMBE vocoder API: everything is in imbe.h.
 *   void imbe_enc_init(imbe_enc_t *e);  void imbe_dec_init(imbe_dec_t *d);
 *   void imbe_encode(imbe_enc_t *e, const int16_t pcm[160], uint8_t u[11]);
 *   int  imbe_decode(imbe_dec_t *d, const uint8_t u[11], int errs, int16_t pcm[160]);  (0 ok, 1 repeat, 2 mute)
 * u = 88 info bits u0..u7 packed MSB first (p25_p1_voice_tx_push / on_voice layout). */
#ifndef IMBE_VOCODER_H
#define IMBE_VOCODER_H
#include "imbe.h"
#endif
