/* Platform voice path (hal/voice.c): TX audio source + vocoder encode, RX vocoder decode + PCM ring + tone meter. */
#ifndef CFW_VOICE_H
#define CFW_VOICE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "modem.h"

/* emulator / debug: decoded 8 kHz voice, ring of 8192 samples, wr = total written (any build) */
extern int16_t g_emu_pcm[8192];
extern volatile uint32_t g_emu_pcm_wr;
extern volatile uint8_t g_voc_select;          /* 0 auto, 1 sine, 2 imbe, 3 sct3288 */

void   voice_init(void);
void   voice_set_test_tone(uint16_t hz);                /* 0 = off (mic not wired yet: silence) */
uint16_t voice_test_tone(void);
size_t voice_tx_frame(cfw_vocoder_t voc, uint8_t *bits, size_t nbits);
void   voice_rx_frame(cfw_vocoder_t voc, const uint8_t *bits, size_t nbits, int errs);
void   voice_tick(void);
void   voice_poll(void);                                  /* main loop: SCT3288 async encode/decode */
void   voice_tx_prepare(cfw_vocoder_t voc);              /* PTT: start encoding ahead */
void   voice_rx_pcm(const int16_t *pcm, size_t n);       /* FM audio (8 kHz) -> ring + meter */                                 /* every 10 ms: tone meter */
const char *voice_codec_name(void);                      /* active codec (AMBE modes) */
const char *voice_codec_name_for(cfw_vocoder_t voc);
bool   voice_codec_interoperable_for(cfw_vocoder_t voc);
bool   voice_codec_interoperable(void);

typedef struct {
    bool     valid;                                      /* a measurement from the last 1.5 s */
    float    freq, sinad_db;
    uint32_t frames, bad;                                /* decoded frames / frames with FEC failure */
} voice_meter_t;
const voice_meter_t *voice_meter(void);
void   voice_meter_reset(void);

void voice_set_gain(uint16_t q8);          /* volume limit, 256 = full */
#endif
