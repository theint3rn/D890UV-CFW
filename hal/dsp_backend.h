/* MCU-DSP demodulator backend (hal/dsp_backend.c), also used by the emulator sample path (hal/emu_syms.c). */
#ifndef CFW_DSP_BACKEND_H
#define CFW_DSP_BACKEND_H
#include <stdint.h>
#include <stdbool.h>
#include "modem.h"

bool     dspb_mode_for(const cfw_air_t *a, int *dsp_mode);   /* false = no demod for this air interface */
void     dspb_set_opts(bool invert, uint16_t deemph_tau_us);  /* applied at the next mode entry */
void     dspb_start(const cfw_air_t *a);
void     dspb_stop(void);
void     dspb_feed(const volatile uint16_t *adc, uint32_t n, uint32_t stride);   /* 12-bit counts at 48 kHz */
void     dspb_poll_load(void);
uint32_t dspb_load_x10(void);        /* CPU load of the demod, 0.1 % units, over the last second */
int      dspb_quality(void);         /* 0..100 */
bool     dspb_locked(void);
bool     dspb_active(void);
/* TX: modulator -> DAC ring (drivers/dac.c), RF chip keyed like stock; pulls symbols with modem_tx_pull() */
bool     dspb_tx_start(const cfw_air_t *a, uint32_t tx_hz);
void     dspb_tx_poll(void);
bool     dspb_tx_active(void);
bool     dspb_tx_dac_moving(void);
/* FM through the MCU (test path): RX audio tone meter on ADC1 ch1, TX test tone on the DAC baseband */
void     dspb_fm_rx(bool on);
bool     dspb_fm_tx(bool on, uint16_t tone_hz, uint16_t ctcss_dhz);   /* false = not built / no tone */
void     dspb_fm_poll(void);                                           /* main loop, every pass */       /* false = no DMA progress seen (emulator without DAC model): paced by time */

void     dspb_fm_set_source(float (*src)(void));   /* FM TX audio source (deviation Hz per 48 kHz sample), NULL = test tone */
float    dsp_sinf(float x);
extern volatile bool g_dspb_fm_mute;         /* FM squelch closed: no recovered audio */
#endif
