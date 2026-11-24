/* cfw vocoder backend "imbe": IMBE 7200x4400 (P25 Phase 1 full rate, YSF VW), 88 bits / 20 ms.
 * Interoperable bitstream (dequantisation tables of TIA-102.BABA via mbelib, cross-decoded with mbelib).
 * Single instance (one encoder, one decoder), not reentrant. */
#ifndef CFW_VOC_IMBE_H
#define CFW_VOC_IMBE_H
#include <stdint.h>
#include <stddef.h>
#include "modem.h"

void   voc_imbe_init(void);                                   /* resets encoder and decoder state */
void   voc_imbe_reset_rx(void);                               /* new call: decoder state only */
void   voc_imbe_reset_tx(void);                               /* new transmission: encoder state only */
/* 160 samples 8 kHz -> 88 bits packed MSB first (u0..u7). Returns 88. Encoder delay: 20 ms. */
size_t voc_imbe_encode(const int16_t pcm[160], uint8_t bits[11]);
/* 88 bits -> 160 samples. errs = corrected FEC bits of the frame (p25_imbe_decode), -1 = unusable:
 * frames with errs > 5 repeat the previous frame, 4 in a row mute. Returns 0 ok, 1 repeated, 2 muted. */
int    voc_imbe_decode(const uint8_t bits[11], int errs, int16_t pcm[160]);

/* the same as a cfw_vocoder_ops_t (decode() without an error count assumes a clean frame) */
extern const cfw_vocoder_ops_t g_voc_imbe;

#endif
