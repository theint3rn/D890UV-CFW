/* Host test builds: the SCT3288 hardware vocoder is not available (voice.c falls back to the sine test codec). */
#include "voc_sct.h"
bool sct_voc_available(void) { return false; }
const char *sct_voc_prodid(void) { return ""; }
bool sct_voc_encode(int rate, const int16_t pcm[160], uint8_t *bits, int *nbits) { (void)rate; (void)pcm; (void)bits; (void)nbits; return false; }
bool sct_voc_decode(int rate, const uint8_t *bits, int nbits, int16_t pcm[160]) { (void)rate; (void)bits; (void)nbits; (void)pcm; return false; }
bool sct_async_busy(void) { return false; }
bool sct_async_encode(int rate, const int16_t pcm[160], sct_async_cb cb) { (void)rate; (void)pcm; (void)cb; return false; }
bool sct_async_decode(int rate, const uint8_t *bits, int nbits, sct_async_cb cb) { (void)rate; (void)bits; (void)nbits; (void)cb; return false; }
