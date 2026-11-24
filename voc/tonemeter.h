/* Tone measurement on 8 kHz PCM: frequency (zero crossings, interpolated) and SINAD (least-squares sine fit). */
#ifndef CFW_TONEMETER_H
#define CFW_TONEMETER_H
#include <stdint.h>
#include <stddef.h>
typedef struct { float freq, sinad_db, amp; } tone_meas_t;
tone_meas_t tone_measure(const int16_t *pcm, size_t n, float fs);
#endif
