/* Analog in-band signalling (FM channels): DTMF, 2-tone, 5-tone (selective call), MDC1200 / QDC1200, alert tones,
 * analog and MDC/QDC address books - the stock CPS "Analog" section, read from codeplug section 10 (stock CSV text).
 *
 * TX: tones / MDC1200 FSK are made by the MCU as FM audio on the TX audio output (D890UV TIM4_CH4, D168UV TMR4_CH4
 *     PWM on PB9, 48 kHz ring, hal/dsp_backend.c dspb_fm_set_source) - the path the stock firmwares play their own
 *     signalling through (docs/AUDIO_PATHS.md). PTT ID at the start (BOT) / end (EOT) of a transmission as set per
 *     channel; manual sends from Menu > Signalling; DTMF digits keyed while PTT is held.
 * RX: decoders on the 8 kHz FM audio tap (dsp_backend fm_rx_poll -> sig_rx_sample): DTMF (Goertzel), single tones
 *     (2-tone / 5-tone: zero-crossing frequency estimate), MDC1200 (1200 bd, 1200/1800 Hz, differential).
 *     Decodes show a popup, go into Menu > Signalling > Heard and play the alert tone for calls to this radio. */
#ifndef CFW_SIG_H
#define CFW_SIG_H
#include <stdint.h>
#include <stdbool.h>
#include "channels.h"

typedef enum { SIG_OFF = 0, SIG_DTMF, SIG_2TONE, SIG_5TONE, SIG_MDC, SIG_KIND_N } sig_kind_t;
enum { SIG_PTTID_OFF = 0, SIG_PTTID_BOT = 1, SIG_PTTID_EOT = 2, SIG_PTTID_BOTH = 3 };

/* per-channel signalling (stock Channel columns: Optional Signal, DTMF ID, 2Tone ID, 5Tone ID, PTT ID, 2TONE Decode,
 * R5toneBot, R5ToneEot, Rpga_Mdc) */
typedef struct {
    uint8_t kind;            /* sig_kind_t: Optional Signal = what this channel sends as PTT ID and decodes */
    uint8_t dtmf_id, tone2_id, tone5_id, mdc_id;   /* entry numbers in the encode lists (1-based, 0 = none) */
    uint8_t pttid;           /* SIG_PTTID_* */
    uint8_t tone2_dec;       /* 2-tone decode: entry number of the pair to listen for (0 = off) */
    bool    r5_bot, r5_eot;  /* 5-tone at the start / end of every transmission (stock R5toneBot / R5ToneEot) */
} sig_chan_t;

/* encode list entries (stock CSV values parsed) */
#define SIG_SEQ_MAX 24
typedef struct { char seq[SIG_SEQ_MAX + 1]; char name[17]; uint8_t std; uint16_t tone_ms; } sig_5t_t;
typedef struct { uint16_t f1, f2; char name[17]; } sig_2t_t;
typedef struct { uint8_t op, arg; uint16_t unit; bool group, ack; char name[17]; uint8_t type; } sig_mdc_t;
enum { MDC_T_PTTID, MDC_T_CALL, MDC_T_SELCALL, MDC_T_EMERG, MDC_T_CHECK, MDC_T_STATUS, MDC_T_N };

/* 5-tone standards (stock CPS order: ZVEI1, ZVEI2, ZVEI3, PZVEI, DZVEI, PDZVEI, CCIR1, CCIR2, PCCIR, EEA,
 * EURO SIGNAL, NATEL, MODAT, CCITT) */
#define SIG_5T_STD_N 14
const char *sig_5t_std_name(int std);

/* stock DTMF / 2Tone / 5Tone setting dialogs (codeplug section 10 tables 65-68, cfw_cps/schema/rdt_dialogs.json; an
 * absent cell = the stock default). Times in ms. Only what the cfw uses; meanings are read from the CPS labels and the
 * user manual, not traced in the stock firmware (docs/AUDIO_PATHS.md). */
typedef struct {
    uint16_t dt_ms, dt_first, dt_pre, dt_lapse, dt_dpause;   /* DTMF digit, first digit, pretime, after code, 'D' pause */
    uint8_t  dt_resp, t5_resp;                               /* Decoding Response: 0 none, 1 beep, 2 beep (+ respond: not done) */
    char     dt_self[4], dt_sep, dt_group;                   /* Self ID, interval character, group code (0 = off) */
    char     dt_bot[SIG_SEQ_MAX + 1], dt_eot[SIG_SEQ_MAX + 1];
    uint16_t t2_1, t2_2, t2_long, t2_gap;
    char     t5_self[SIG_SEQ_MAX + 1];
    uint8_t  t5_std, t5_dec, t5_any;                         /* decode standard, decode time (10 ms blocks), Disp AnyID */
    uint16_t t5_pre, t5_lapse, t5_pause;                     /* pretime, after code (ms), BOT pause (s) */
    sig_5t_t t5_bot, t5_eot;
} sig_cfg_t;
const sig_cfg_t *sig_cfg(void);

/* heard list (decoded signalling, newest first) */
typedef struct { uint8_t kind; bool to_me; uint8_t op, arg; uint16_t unit; char text[25]; char name[17]; uint32_t t;
                 uint16_t target; uint8_t msg_op, msg_arg; } sig_heard_t;   /* MDC: unit = caller; double packet: message op / arg */
int  sig_heard_count(void);
const sig_heard_t *sig_heard(int i);

/* codeplug */
void sig_reload(void);                         /* after a codeplug write / at boot (lazy otherwise) */
const sig_chan_t *sig_chan(void);              /* current channel (VFO: all off) */
int  sig_dtmf_count(void);
bool sig_dtmf(int i, char *seq, int n);        /* entry i (0-based): digits */
int  sig_2t_count(void);
bool sig_2t(int i, sig_2t_t *e);
int  sig_5t_count(void);
bool sig_5t(int i, sig_5t_t *e);
int  sig_mdc_count(void);
bool sig_mdc(int i, sig_mdc_t *e);
int  sig_abook_count(void);                    /* Analog Address Book: number (DTMF) + name */
bool sig_abook(int i, char *num, int nn, char *name, int nm);
int  sig_mbook_count(void);                    /* MDC1200 / QDC1200 address book */
bool sig_mbook(int i, sig_mdc_t *e);
const char *sig_mdc_label(void);               /* "MDC1200" (D890UV CPS name) / "QDC1200" (D168UV) */

/* TX */
const char *sig_send(sig_kind_t kind, int entry);          /* manual send of encode list entry (0-based); NULL = started */
const char *sig_send_dtmf(const char *digits);              /* NULL = started */
const char *sig_send_mdc(uint8_t op, uint8_t arg, uint16_t unit);
const char *sig_call_mdc(uint16_t target);                  /* MDC / QDC call alert (double packet) */
bool sig_busy(void);                                         /* a burst is on air (or pending) */
void sig_tx_begin(const channel_t *c);                       /* radio.c: FM PTT down, before the audio stream starts */
bool sig_tx_end(const channel_t *c);                         /* radio.c: FM PTT up; true = EOT burst started, keep TX */
void sig_dtmf_key(char d);                                   /* DTMF digit while PTT is held (FM) */
void sig_tick(void);                                         /* 10 ms */
void sig_alert(int n);                                       /* stock AlertTone pattern n (1..5) on the speaker */

/* RX (8 kHz FM audio, squelch ignored; hal/dsp_backend.c) */
void sig_rx_sample(int16_t x);
void sig_rx_reset(void);

/* popup for the UI: returns the seq number of the newest event (changes when a new one arrives) */
uint8_t sig_note(const char **l1, const char **l2);

/* internals shared by sig.c / sig_dec.c / sig_mdc.c */
#define MDC_PACKET_BYTES 26
#define MDC_PACKET2_BYTES 40
uint16_t mdc_crc(const uint8_t *d, int n);
int  mdc_build(uint8_t op, uint8_t arg, uint16_t unit, uint8_t out[MDC_PACKET_BYTES]);
int  mdc_build2(uint8_t op, uint8_t arg, uint16_t unit, const uint8_t ext[4], uint8_t out[MDC_PACKET2_BYTES]);
bool mdc_decode112(const uint8_t bits[112], uint8_t *op, uint8_t *arg, uint16_t *unit);
/* MDC double packet: ext = the second block (message op, arg, caller id high, low), unit = caller, target = first
 * block's unit; single packets: ext NULL, target 0 */
void sig_decoded(sig_kind_t kind, const char *text, uint8_t op, uint8_t arg, uint16_t unit, uint16_t target, const uint8_t *ext);
const uint16_t *sig_5t_tones(int std);                       /* 16 tones (0-9, A-F) in Hz, 0 = none; E = repeat */
uint16_t sig_5t_default_ms(int std);
#endif
