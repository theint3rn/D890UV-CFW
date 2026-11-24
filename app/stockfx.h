/* Stock CPS settings that act at run time, part 2 (app/stockopt.c reads the codeplug):
 *   tone squelch  channel Squelch Mode CTCSS/DCS + the channel's RX tone: CTCSS / DCS decoders on the 8 kHz FM audio
 *   call tones    Digi Idle Channel Tone, Ana Idle Channel Tone
 *   TBST          1000 / 1450 / 1750 / 2100 Hz repeater access burst (Up key while PTT is held on FM)
 *   FM / AM       AM/FM Function at power on, FM Work Channel, FM Monitor, AM squelch level
 *   NOAA          weather channels (receive only) and the 1050 Hz alert
 *   Auto Scan     channel column; Analog APRS PTT Mode (beacon after PTT, not in HW=1 builds)
 *   Bluetooth     module gains / status (drivers/bt.c) */
#ifndef CFW_STOCKFX_H
#define CFW_STOCKFX_H
#include <stdint.h>
#include <stdbool.h>
#include "channels.h"

void stockfx_tick(void);                       /* radio_tick (10 ms) */
void stockfx_channel(void);                    /* radio_apply: the current channel changed */
bool stockfx_tone_ok(void);                    /* tone squelch satisfied (or not in use) */
bool stockfx_tone_used(void);
void tone_rx_sample(int16_t x);                /* 8 kHz FM audio tap (hal/dsp_backend.c) */
uint32_t stockfx_dcs_word(uint16_t code);      /* 23-bit DCS word of a code (binary value of the octal code) */
void stockfx_ptt_end(void);                    /* FM PTT released after a transmission */
bool stockfx_tbst(void);                       /* start the burst; false = not now */
bool stockfx_alias_on(const channel_t *c);     /* Send Talker Alias of this channel */
uint16_t stockfx_color(uint8_t which);         /* 0 channel name, 1 standby characters (RGB565) */
/* power-on password (Optional Settings Power-on Password + characters) */
bool stockfx_locked(void);                     /* the radio waits for the password: no TX */
void stockfx_lock_key(int digit, bool enter, bool clear);
int  stockfx_lock_len(void);
/* NOAA */
#define NOAA_N 7
uint32_t stockfx_noaa_hz(int i);
void stockfx_noaa(int i);                      /* tune weather channel i; -1 = leave */
int  stockfx_noaa_now(void);
/* Bluetooth (drivers/bt.c; weak no-op default) */
typedef struct { bool on, present, connected; uint8_t mic, spk; char ver[16], name[20]; } bt_status_t;
void bt_config(bool on, uint8_t mic, uint8_t spk);
void bt_tick(void);
const bt_status_t *bt_status(void);
extern volatile uint32_t g_stockfx_dbg[8];     /* tests: idle tones, TBST, NOAA alerts, tone opens, tone closes, auto scans, PTT beacons, fm pauses */
#endif
