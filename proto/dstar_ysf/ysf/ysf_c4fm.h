/*
 * ysf_c4fm.h - C4FM dibit slicer helper (symbol rate in, dibits out).
 *
 * Input: one soft value per symbol, taken at the symbol centre after the
 * receive filter (RRC, alpha 0.2 per spec 3.2) and timing recovery, any gain
 * and DC offset. The slicer keeps the last 20 symbols, correlates them with
 * the YSF sync (all four levels occur in D471C9634D), and on a match fits
 * gain and offset by least squares. Between syncs it slices with the last
 * fit. Before the first sync it tracks peaks as a rough fallback.
 *
 * Also: dibit -> deviation helper for a transmitter (wide: +/-900 / 2700 Hz,
 * narrow: half). Pulse shaping is the platform's job.
 */
#ifndef YSF_C4FM_H
#define YSF_C4FM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float hist[20];
    unsigned pos;
    float gain;     /* symbol units per input unit */
    float offset;   /* input value of symbol 0 */
    bool locked;    /* gain/offset from a sync fit */
    float peak_pos, peak_neg; /* fallback tracker */
    float min_corr; /* normalised correlation threshold (default 0.8) */
} ysf_c4fm_slicer_t;

void ysf_c4fm_slicer_init(ysf_c4fm_slicer_t *s);
/* Slice one symbol. *sync (may be NULL) is set when this symbol completed a
 * sync match (the gain/offset were refit). */
uint8_t ysf_c4fm_slice(ysf_c4fm_slicer_t *s, float x, bool *sync);
/* Symbol level after normalisation of the last call (for diagnostics). */
float ysf_c4fm_norm(const ysf_c4fm_slicer_t *s, float x);

/* Frequency deviation in Hz for a dibit. */
float ysf_c4fm_deviation_hz(uint8_t dibit, bool narrow);

#ifdef __cplusplus
}
#endif
#endif
