/* SCT3288 hardware vocoder (AMBE-3000-style packets on the SPI2 baseband link), see cfw/docs/SCT3288_VOCODER.md. */
#ifndef CFW_VOC_SCT_H
#define CFW_VOC_SCT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum { SCT_RATE_NONE = 0, SCT_RATE_49 = 34, SCT_RATE_72 = 33, SCT_RATE_DSTAR = 0x100 };

bool sct_voc_available(void);                     /* baseband ready and PRODID answered (probed once) */
const char *sct_voc_prodid(void);
bool sct_voc_encode(int rate, const int16_t pcm[160], uint8_t *bits, int *nbits);
bool sct_voc_decode(int rate, const uint8_t *bits, int nbits, int16_t pcm[160]);

/* asynchronous (non-blocking) variants: false = busy / not available; cb(b0, data, len) runs from bb_poll() */
typedef void (*sct_async_cb)(uint8_t b0, const uint8_t *data, uint16_t len);
bool sct_async_busy(void);
bool sct_async_encode(int rate, const int16_t pcm[160], sct_async_cb cb);
bool sct_async_decode(int rate, const uint8_t *bits, int nbits, sct_async_cb cb);

#endif
