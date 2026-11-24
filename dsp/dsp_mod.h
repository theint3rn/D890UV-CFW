/* cfw/dsp modulators: symbols -> instantaneous frequency (Hz) at fs, i.e. the baseband an FM transmitter
 * (the RF chip's modulation input) needs. Float, no malloc.
 *   DMR, NXDN48/96   4FSK, root-raised-cosine 0.2 frequency pulse
 *   P25 Ph1, YSF     C4FM: raised cosine 0.2 x inverse sinc (TIA-102.BAAA shaping), numeric inverse Fourier
 *   D-STAR           GMSK BT 0.5, h 0.5 (+/-1200 Hz), '1' = +deviation
 *   P25 Ph2          4-level CPM, h = 1/4, 1RC frequency pulse confined to one symbol: the phase step per symbol
 *                    is exactly +/-pi/4 or +/-3pi/4, which is what the cfw H-DQPSK discriminator receiver measures.
 *                    This is an H-CPM-like inbound modulation (TIA-102.BBAB H-CPM uses its own partial-response
 *                    pulse, not reproduced here) carrying OUTBOUND-format slots - test only, not interoperable.
 * Output scale: DSP_MOD_DAC(hz) = 2048 + 0.25 count/Hz (same as the cfw .adc16 discriminator files). */
#ifndef CFW_DSP_MOD_H
#define CFW_DSP_MOD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "dsp_demod.h"

#define DSP_MOD_RES   16                 /* pulse table points per symbol */
#define DSP_MOD_SPAN  4                  /* +/- symbols */
#define DSP_MOD_TAB   (2 * DSP_MOD_SPAN * DSP_MOD_RES + 1)
#define DSP_MOD_DAC(hz) (2048.0f + 0.25f * (hz))
#define DSP_SYM_GAP 0xFFu                /* TX symbol value: no modulation (DMR direct-mode / inbound slot gaps) */

typedef size_t (*dsp_mod_pull_t)(void *user, uint8_t *sym, size_t max);

typedef struct {
    dsp_mode_t mode;
    float    fs, step;                   /* symbols per output sample */
    float    unit;                       /* Hz per level unit (outer / 3, or outer for GMSK) */
    float    tab[DSP_MOD_TAB];
    float    u;                          /* time in symbols, relative to sym index base */
    float    lv[2 * DSP_MOD_SPAN + 2];   /* levels of symbols base-SPAN .. base+SPAN+1 */
    int      ended;                      /* symbols flushed after the source ended (0 = running) */
    bool     done;
} dsp_mod_t;

void   dsp_mod_init(dsp_mod_t *m, dsp_mode_t mode, uint32_t fs);
/* n output samples (Hz); pulls symbols as needed. Returns samples written (< n when the burst is over). */
size_t dsp_mod_run(dsp_mod_t *m, float *hz, size_t n, dsp_mod_pull_t pull, void *user);

#endif
