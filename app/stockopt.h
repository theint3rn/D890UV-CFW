/* Stock CPS "common" sections used by the cfw (codeplug section 10, stock CSV text written by cfw_cps), for the
 * radios that have them (D890UV, D168UV; AM air D890UV only). See cfw_cps/docs/CODEPLUG.md "Stock sections used by
 * the firmware" and the cfw README "Stock CPS sections".
 *
 *   Optional Settings  -> VFO step, time zone (GPS clock), power-on text / startup sound / startup channel, SMS and
 *                         call ring, talk permit tone, TOT pre-alert, menu exit time, auto repeater (VFO), FM / AM
 *                         broadcast state, roaming timers (stockopt_opt_*)
 *   Auto Repeater Offset Frequencies -> VFO TX offset (stockopt_auto_rpt_tx)
 *   Channel columns    -> busy lock, squelch mode, PTT prohibit, reverse, correct frequency, exclude from roaming
 *   Hot Key / Hot Key Quick Call / Hot Key State -> long press of a digit key (stockopt_hotkey)
 *   Roaming Channel / Roaming Zone / GPS Roaming -> Menu > Roaming (app/roam.c)
 *   FM (broadcast memories), AM Air + AM Zone -> Menu > FM radio / AM air (app/bcast_ui.c, drivers/bcast.c)
 *   APRS               -> beacon TOCALL, callsign / SSID, path, symbol, comment, fixed position, auto interval,
 *                         TX frequency (app/aprs.c, app/gps.c)
 * List tables that equal the stock CPS default are not stored by the CPS: the stock default rows apply then. */
#ifndef CFW_STOCKOPT_H
#define CFW_STOCKOPT_H
#include <stdint.h>
#include <stdbool.h>
#include "cfwcp.h"
#include "channels.h"

/* stock table ids (positions in the stock .LST order, same on both radios for these tables) */
enum { SO_T_PREFAB = 6, SO_T_FM = 7, SO_T_QCALL = 12, SO_T_HSTATE = 13, SO_T_HOTKEY = 14, SO_T_ARPT = 16,
       SO_T_ROAMCH = 17, SO_T_ROAMZ = 18, SO_T_APRS = 19, SO_T_GPSROAM = 20, SO_T_OPT = 21,
       SO_T_AMAIR = 27, SO_T_AMZONE = 30,                     /* D890UV only */
       SO_T_CHANNEL = 0xF0 };                                 /* per-channel extras (row = codeplug channel) */

void     stockopt_init(void);                  /* after cfwcp_check() (boot, CPS write) */

/* ---- Optional Settings (record) ---- */
uint32_t stockopt_opt(int col, uint32_t dflt);         /* numeric value of an OptionalSetting column */
bool     stockopt_opt_text(int col, char *buf, int n); /* text value (false = default / empty) */
uint32_t stockopt_step_hz(void);              /* Step: 0 = not set (cfw keeps its own step) */
int      stockopt_tz_count(void);
int      stockopt_tz_at(int i);                /* minutes east of UTC of list entry i */
int32_t  stockopt_tz_min(void);               /* TmZone: minutes east of UTC */
bool     stockopt_boot_text(char *l1, char *l2);   /* Power-on Interface = Character: the two text lines */
bool     stockopt_boot_sound(void);
bool     stockopt_sms_ring(void);
bool     stockopt_call_ring(void);
uint8_t  stockopt_talk_permit(void);           /* 0 off, 1 digital, 2 analog, 3 both */
bool     stockopt_tot_predict(void);           /* beep 10 s before the TX timeout */
uint32_t stockopt_menu_exit_s(void);           /* 0 = never */
bool     stockopt_start_channel(int *zone, int *ch);   /* Default Startup Channel on: zone index, channel index */

/* ---- auto repeater (VFO): TX frequency for a VFO RX frequency (= rx when off / outside the ranges) ---- */
uint32_t stockopt_auto_rpt_tx(uint32_t rx_hz);
int      stockopt_rpt_offset_count(void);
uint32_t stockopt_rpt_offset(int i);           /* Hz */

/* ---- per-channel stock columns (codeplug channel index) ---- */
enum { SO_BUSY_OFF = 0, SO_BUSY_FREE = 1 };     /* Busy Lock/TX Permit: Always = off, others = channel must be free */
typedef struct {
    uint8_t busy_lock;
    bool    sql_tone;           /* Squelch Mode CTCSS/DCS: open only with the RX tone (when the channel has one) */
    bool    ptt_prohibit;
    bool    reverse;            /* RX and TX frequencies swapped */
    int32_t correct_hz;         /* Correct Frequency[Hz]: added to RX and TX */
    bool    no_roam;            /* Exclude channel from roaming */
    bool    alias;              /* Send Talker Alias */
    bool    auto_scan;          /* Auto Scan */
    bool    aprs_end;           /* APRS Report Type Analog + Analog APRS PTT Mode = end of transmission */
    bool    work_alone;         /* Work Alone */
    uint8_t dmr_mode;           /* DMR MODE: 0 simplex, 1 repeater, 2 / 3 dual capacity direct */
} so_chan_t;
void     stockopt_channel(int ch, so_chan_t *o);   /* all off for built-in / VFO channels */
void     stockopt_channel_apply(int ch, channel_t *c); /* channel load: Reverse + Correct Frequency on rx / tx */

/* ---- hot keys (long press of digit key 1..9, 0 = 10) ---- */
enum { SO_HK_OFF, SO_HK_MENU, SO_HK_CALL };
typedef struct {
    uint8_t mode;               /* SO_HK_* */
    char    menu[20];           /* Menu: stock menu name (SMS, New SMS, Hot Text, Contact List, Call Log, Zone ...) */
    bool    digital;            /* Call: Call Type Digital (else Analog) */
    char    object[20];         /* Call: contact name (digital) / quick call entry (analog) */
    bool    priv;               /* Call: private call */
    char    content[20];        /* Call: message (Pre-defined SMS text name / "Off") */
} so_hotkey_t;
bool     stockopt_hotkey(int n, so_hotkey_t *o);    /* n = 1..18 */
typedef struct { uint8_t kind; uint8_t entry; } so_qcall_t;   /* sig_kind_t + encode list entry (1-based, 0 none) */
bool     stockopt_quickcall(int n, so_qcall_t *o);  /* Hot Key Quick Call row n (1..4) */
int      stockopt_hstate_count(void);
bool     stockopt_hstate(int i, char *buf, int n);  /* Hot Key State text i (0-based) */

/* ---- roaming ---- */
typedef struct { uint32_t rx_hz, tx_hz; uint8_t cc, slot; char name[17]; } so_roamch_t;
int      stockopt_roamch_count(void);
bool     stockopt_roamch(int i, so_roamch_t *o);
int      stockopt_roamzone_count(void);
bool     stockopt_roamzone(int z, char *name, int n);
int      stockopt_roamzone_members(int z, uint8_t *idx, int max);   /* roaming channel indices */
typedef struct { bool on; int16_t zone; int32_t lat_e7, lon_e7; uint32_t radius_m; } so_gpsroam_t;
int      stockopt_gpsroam_count(void);         /* always 32 rows */
bool     stockopt_gpsroam(int i, so_gpsroam_t *o);

/* ---- broadcast memories ---- */
typedef struct { uint32_t hz; bool scan; char name[17]; } so_bmem_t;
int      stockopt_fm_count(void);              /* memory channels (VFO row excluded) */
bool     stockopt_fm(int i, so_bmem_t *o);
bool     stockopt_fm_vfo(uint32_t *hz);
int      stockopt_am_count(void);
bool     stockopt_am(int i, so_bmem_t *o);
bool     stockopt_am_vfo(uint32_t *hz);
int      stockopt_amzone_count(void);
bool     stockopt_amzone(int z, char *name, int n);
int      stockopt_amzone_members(int z, uint16_t *idx, int max);    /* AM memory indices */

/* ---- APRS ---- */
typedef struct {
    uint16_t manual_s, auto_s;  /* Manual TX Interval / APRS Auto TX Interval [s] (0 = off) */
    bool     fixed;             /* Fixed Location Beacon */
    int32_t  lat_e7, lon_e7;
    char     tocall[7]; uint8_t tocall_ssid;   /* "" = not set (cfw: APZCFW) */
    char     call[7];   uint8_t ssid;          /* "" = use the radio callsign */
    char     sym_table, sym;                   /* 0 = not set (cfw: "/>") */
    char     path[24];          /* "WIDE1-1,WIDE2-1", "" = not set (cfw: WIDE1-1) */
    char     text[48];          /* comment, "" = not set (cfw: "cfw CALL") */
    uint32_t tx_hz;             /* Transmission Frequency (0 = the current channel) */
} so_aprs_t;
const so_aprs_t *stockopt_aprs(void);

/* ---- helpers ---- */
uint32_t stockopt_mhz(const char *t);          /* "145.390" -> Hz */
#endif
