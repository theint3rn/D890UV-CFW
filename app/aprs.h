/* APRS position beacon: AX.25 UI frame (source = my call, dest APZCFW, path WIDE1-1), Bell 202 AFSK 1200 baud, sent
 * on the current channel's TX frequency with the analog FM TX path (radio_ptt + the dsp_backend FM modulator). */
#ifndef CFW_APRS_H
#define CFW_APRS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gps.h"

const char *aprs_send_position(const gps_fix_t *f);     /* NULL = started */
size_t aprs_build_info(char *out, size_t max, const gps_fix_t *f);   /* "!DDMM.mmN/DDDMM.mmW>..." */
size_t aprs_build_frame(uint8_t *out, size_t max, const char *src, const char *info);   /* AX.25 without flags/FCS, + FCS */
bool aprs_busy(void);
bool aprs_done(void);                       /* all bits sent */
void aprs_end(void);

/* receive (app/aprs_rx.c): AFSK 1200 on the analog FM receive audio -> heard stations */
#define APRS_HEARD_MAX 8
typedef struct { char call[10]; char info[44]; uint32_t t; uint16_t count; } aprs_heard_t;
void aprs_rx_sample(int16_t x);             /* 8 kHz FM audio (dsp_backend) */
int  aprs_heard_count(void);
const aprs_heard_t *aprs_heard(int i);      /* 0 = most recent */
uint32_t aprs_rx_frames(void);
#endif
