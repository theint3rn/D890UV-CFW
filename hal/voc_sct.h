/* SCT3288 hardware vocoder + audio codec on the SPI2 baseband link (hal/voc_sct.c, docs/SCT3288_VOCODER.md). */
#ifndef CFW_VOC_SCT_H
#define CFW_VOC_SCT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* what the chip does right now: the voice path is the stock "vocoder loop" (no RF) with VOCODER_IO_SET (cmd 0x3E) */
typedef enum {
    SCT_OFF = 0,
    SCT_DEC,        /* channel frames from the MCU -> decoder -> speaker          (stock mode 1, IO 0x08) */
    SCT_ENC_MIC,    /* microphone -> encoder -> channel frames to the MCU         (stock mode 5, IO 0x10) */
    SCT_ENC_PCM,    /* PCM from the MCU -> encoder -> channel frames to the MCU   (IO 0x90 = 0x80 + 0x10) */
    SCT_MIC_PCM,    /* microphone -> PCM to the MCU                               (stock mode 0xD, IO 0x01) */
    SCT_PLAY_PCM,   /* PCM from the MCU -> speaker                                (stock mode 0xF, IO 0x80) */
    SCT_NSESS
} sct_sess_t;

typedef void (*sct_chan_cb)(const uint8_t *frames, unsigned n);   /* n frames of 9 bytes, from sct_poll() / bb_poll() */
typedef void (*sct_pcm_cb)(const uint8_t *be);                    /* 160 samples s16 big endian */

extern volatile uint8_t g_sct_tap;       /* 1 = SCT_DEC also returns the decoded PCM (IO 0x0A); HW builds 0 */
extern volatile uint8_t g_sct_force;     /* 0 auto, 1 chip modes only, 2 PCM from the MCU instead of the mic, 3 external AMBE */

void sct_set_sinks(sct_chan_cb chan, sct_pcm_cb pcm);
void sct_poll(void);                     /* main loop, never blocks: probe, command sequences, time-outs */
bool sct_chip_ok(void);                  /* the probe found the VOCODER_IO_SET interface (mode A / B) */
bool sct_can(sct_sess_t s);              /* usable: chip found, and that session has not failed in this power cycle */
void sct_session(sct_sess_t s);          /* ask for a session (the switch takes a few commands) */
sct_sess_t sct_session_now(void);        /* session that is up, SCT_OFF while switching */
bool sct_want_chan(void);                /* SCT_DEC: the chip takes the next 4 frames */
bool sct_put_chan(const uint8_t d[36]);
bool sct_want_pcm(void);                 /* SCT_ENC_PCM / SCT_PLAY_PCM: the chip takes the next 160 samples */
bool sct_put_pcm(const int16_t pcm[160]);
bool sct_raw49(void);                    /* the chip's frames turned out to be 49 bits without FEC */
void sct_note_format(int fec_errs, const uint8_t f[9]);     /* called per received frame */
const char *sct_status_text(void);       /* short text for "Baseband ..." on the screens */

/* external AMBE-3000 style packets (TYPE 0x80..0x82; needs an AMBE chip on the SCT3288's serial port) */
enum { SCT_RATE_NONE = 0, SCT_RATE_49 = 34, SCT_RATE_72 = 33, SCT_RATE_DSTAR = 0x100 };
typedef void (*sct_async_cb)(uint8_t b0, const uint8_t *data, uint16_t len);
bool sct_ext_available(void);            /* PRODID answered */
const char *sct_voc_prodid(void);
bool sct_async_busy(void);
bool sct_async_encode(int rate, const int16_t pcm[160], sct_async_cb cb);
bool sct_async_decode(int rate, const uint8_t *bits, int nbits, sct_async_cb cb);

#ifdef CFW_DEBUG
extern volatile uint8_t g_sct_bench;     /* write 1 (debugger / RTT key 'b'): run the bench check, log on RTT */
bool sct_bench_active(void);
#else
static inline bool sct_bench_active(void) { return false; }
#endif
#endif
