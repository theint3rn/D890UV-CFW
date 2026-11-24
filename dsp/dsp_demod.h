/* cfw/dsp: digital-voice demodulators that work on the FM discriminator output (the only RX input of the
 * D890UV: ADC1 ch1, see docs/FEASIBILITY.md). Portable C99 + float (Cortex-M33 FPU), no malloc.
 *
 * Input: real samples proportional to the instantaneous frequency (any scale, any DC offset), at fs_in.
 * Output: hard symbols (dibits 0..3 with the C4FM/4FSK mapping +3->01 +1->00 -1->10 -3->11, or GMSK bits
 * 0/1 with '1' = +deviation) plus a signed confidence per symbol, ready for modem_rx_push().
 *
 * Chain per mode:
 *   decimate fs_in -> fs (2:1 when fs_in = 2 fs)
 *   [inverse de-emphasis]            switchable, for an audio path with 1st-order de-emphasis (tau)
 *   matched filter                   RRC (DMR, NXDN), integrate-and-dump (C4FM, H-DQPSK), Gaussian-ish (GMSK)
 *   Gardner timing recovery          fractional (linear) interpolation, PI loop: tracks +/-100 ppm clocks
 *   coarse AGC / DC                  mean |x| and slow mean, until the first sync
 *   sync correlator                  normalised correlation against the mode's sync words, both polarities:
 *                                    frame lock, polarity, and a least-squares DC + level fit on the sync symbols
 *   decision-directed tracking       DC (frequency offset) and level (deviation error) LMS after lock
 *   slicer                           4 or 2 levels, soft = distance to the nearest threshold
 * H-DQPSK (P25 Phase 2 outbound): the integral of the discriminator over one symbol is the phase step
 * (+/-pi/4, +/-3pi/4), i.e. a 4-level signal with the same 1:3 ratio, so the same chain applies. */
#ifndef CFW_DSP_DEMOD_H
#define CFW_DSP_DEMOD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    DSP_MODE_DMR = 0,      /* 4FSK 4800 sym/s, RRC 0.2, +/-1944 / +/-648 Hz */
    DSP_MODE_P25P1,        /* C4FM 4800 sym/s, RC 0.2 + inverse sinc TX, I&D RX, +/-1800 / +/-600 Hz */
    DSP_MODE_YSF,          /* C4FM 4800 sym/s like P25 (YSF sync) */
    DSP_MODE_NXDN48,       /* 4FSK 2400 sym/s, RRC 0.2, +/-1050 / +/-350 Hz */
    DSP_MODE_NXDN96,       /* 4FSK 4800 sym/s, RRC 0.2, +/-2400 / +/-800 Hz */
    DSP_MODE_DSTAR,        /* GMSK 4800 bit/s BT 0.5, h 0.5, +/-1200 Hz */
    DSP_MODE_P25P2,        /* H-DQPSK 6000 sym/s, discriminator integrate-and-dump */
    DSP_MODE_COUNT
} dsp_mode_t;

#define DSP_MAX_TAPS   97
#define DSP_HIST       32
#define DSP_MAX_SYNC   48

typedef struct {
    const char *name;
    uint32_t sym_rate;
    uint8_t  levels;            /* 4 or 2 */
    uint8_t  filter;            /* DSP_F_* */
    float    outer_hz;          /* nominal outer deviation (only for the info / test signals) */
    uint8_t  nsync;
    bool     pol_fixed;         /* sync words come in inverse pairs (DMR voice/data): polarity from opts only */
    struct { uint64_t bits; uint8_t len; } sync[8];   /* len in symbols; dibits / bits MSB first */
} dsp_mode_info_t;

enum { DSP_F_RRC = 0, DSP_F_ID = 1, DSP_F_GAUSS = 2 };

const dsp_mode_info_t *dsp_mode_info(dsp_mode_t m);

typedef struct {
    float tau_us;               /* inverse de-emphasis time constant, 0 = off (e.g. 750) */
    bool  invert;               /* start with inverted polarity (sync detection corrects it anyway) */
} dsp_opts_t;

typedef struct {
    dsp_mode_t mode;
    const dsp_mode_info_t *mi;
    uint32_t fs_in, fs;
    uint8_t  dec;               /* fs_in / fs */
    float    sps;
    /* decimator / de-emphasis */
    float    dacc; uint8_t dcnt;
    float    de_a, de_prev; bool de_on;
    /* matched filter */
    float    taps[DSP_MAX_TAPS];
    uint16_t ntaps;
    float    xr[DSP_MAX_TAPS];  /* FIR delay line (circular) */
    uint16_t xi;
    /* timing */
    float    yr[DSP_HIST];      /* MF output history */
    uint32_t n;                 /* MF samples so far */
    float    t_next;            /* next strobe time (in MF samples) */
    float    t_int;             /* loop integrator (samples / symbol correction) */
    float    prev_sym;
    float    kp, ki;
    /* levels */
    float    pol;               /* +1 / -1 configured polarity */
    float    dc, gain;          /* x = dc + gain * level, gain > 0 normal polarity, < 0 inverted */
    float    mabs, mavg;        /* coarse estimates before lock */
    bool     locked;
    uint32_t since_sync;        /* symbols since the last sync */
    float    sr[DSP_MAX_SYNC];  /* last strobes (raw) for the correlator */
    uint8_t  sri;
    /* stats */
    float    evm;               /* mean square error to the nearest level (level units^2) */
    uint32_t syms, syncs;
    float    last_corr;
    uint8_t  quiet;             /* consecutive strobes at ~zero deviation (TDMA gap) */
    uint8_t  post_quiet;        /* strobes since the last gap */
} dsp_demod_t;

void   dsp_init(dsp_demod_t *d, dsp_mode_t mode, uint32_t fs_in, const dsp_opts_t *opts);
/* Process n input samples; writes up to max symbols. Returns the number of symbols written. */
size_t dsp_process(dsp_demod_t *d, const float *x, size_t n, uint8_t *sym, int8_t *soft, size_t max);
/* 0..100: 100 = clean (EVM based), 0 = noise. */
int    dsp_quality(const dsp_demod_t *d);
/* modulation index helpers for the test signal generator */
float  dsp_level_of(uint8_t dibit);     /* 0->+1 1->+3 2->-1 3->-3 */

#endif
