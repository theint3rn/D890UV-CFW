#ifndef CFW_RADIO_H
#define CFW_RADIO_H
#include <stdint.h>
#include <stdbool.h>
#include "channels.h"

typedef struct {
    bool      vfo;            /* VFO mode (else memory channel) */
    int       channel;        /* memory channel index */
    channel_t mem;            /* current memory channel (loaded from codeplug / built-ins + override) */
    channel_t vfo_ch;         /* VFO settings */
    bool      tx;             /* transmitting */
    bool      rf_ok[2];       /* RF chip self-test */
    bool      mode_ok;        /* digital mode has proto + modem (FM always ok) */
    uint32_t  step_hz;
    uint16_t  rssi;           /* RF chip 0 reg 0x02 raw, read every 100 ms in RX */
    bool      carrier;        /* reg 0x0E bit 14 */
    uint32_t  my_id;          /* own DMR / P25 / NXDN id */
    char      my_call[9];     /* own callsign (D-STAR MY, YSF) */
    char      alias[17];      /* DMR talker alias sent with calls ("" = none): codeplug radio name, else my call */
    bool      rx_invert;      /* MCU-DSP: discriminator polarity inverted */
    uint16_t  deemph_us;      /* MCU-DSP: inverse de-emphasis (0 = flat audio path, 750 = compensate) */
    int16_t   zone;           /* -1 = all channels */
    bool      monitor;        /* squelch held open */
    bool      sq_open;        /* FM squelch state (radio_tick) */
    bool      tot_hit;        /* TX stopped by the timeout timer; PTT must be released first */
    uint32_t  tx_since;       /* hal_millis() at key-up */
    uint32_t  last_activity;  /* hal_millis() of the last key / PTT / received call (auto power off) */
    uint8_t enc_tx;            /* last TX: 1 encrypted, 2 key set but amateur band (sent in clear) */
    const char *tx_note[2];    /* TX refused / stopped by hal/safety.c: popup text for the UI (seq changes) */
    uint8_t   tx_note_seq;
} radio_t;

extern radio_t g_radio;
uint8_t radio_tx_power(void);
void radio_boot_ptt_lock(void);  /* boot: PTT already held -> TX locked until released */   /* power level used for TX (HW=1: Low until a level is picked in this session) */

void radio_init(void);             /* after channels_init() */
const channel_t *radio_current(void);
channel_t *radio_edit(void);       /* current channel for editing; call radio_commit() after changing it */
void radio_commit(bool retune);    /* store the edit (memory: as an override) and pass it to RF / proto */
void radio_apply(void);            /* program RF / modem for the current channel */
void radio_step(int dir);          /* channel +/- or VFO step */
void radio_goto(int channel);
void radio_toggle_vfo(void);
void radio_set_vfo_freq(uint32_t hz);
void radio_set_mode(cfw_mode_t m); /* works in memory and VFO mode */
void radio_cycle_mode(int dir);
void radio_chan_cfg(cfw_chan_cfg_t *cfg);
uint32_t radio_tx_hz(const channel_t *c);  /* effective TX frequency (talkaround = RX frequency) */
bool radio_is_simplex(const channel_t *c);
void radio_toggle_talkaround(void);
void radio_ptt(bool down);
void radio_save(void);
void radio_tick(void);             /* 10 ms */
void radio_set_zone(int z);        /* -1 = all; goes to the zone's first channel unless the current one is in it */
void radio_set_target(uint32_t id, bool priv);   /* TX target of the current channel (contact / manual dial / call back) */
void radio_set_power(uint8_t p);   /* current channel TX power (memory: codeplug journal) */
void radio_activity(void);
void radio_update_alias(void);
bool radio_send_sms(uint32_t dst, bool group, const char *text);   /* DMR: starts the data TX */
const char *radio_sms_error(void);

bool cfw_ham_band(uint32_t hz);
void radio_rx_power(bool on);     /* power save: receiver off / on */   /* amateur allocation: encrypted TX refused */
#endif
