/* Stock CPS dialogs that exist only in the .rdt (cfw_cps: codeplug section 10, table ids 64..72; columns, defaults and
 * units in cfw_cps/schema/rdt_dialogs.json). One flash walk at boot / after a CPS write caches where each cell is, so the
 * readers are cheap. Same ids and columns on both radios. An absent cell = the stock default (pass it as `dflt`). */
#ifndef CFW_STOCKDLG_H
#define CFW_STOCKDLG_H
#include <stdint.h>
#include <stdbool.h>

enum { SD_T_ALARM = 64, SD_T_DTMF = 65, SD_T_2TONE = 66, SD_T_5TONE = 67, SD_T_5TINFO = 68, SD_T_ALIAS = 70,
       SD_T_FRIENDS = 71, SD_T_ENC = 72 };        /* 68, 71, 72 = lists, the others are records */

void     stockdlg_init(void);                     /* rset_init() calls it */
uint32_t stockdlg_num(int table, int col, uint32_t dflt);          /* record cell as a decimal number */
bool     stockdlg_text(int table, int col, char *buf, int n);      /* record cell text; false = absent (default) */
int      stockdlg_rows(int table);                                 /* list: rows */
bool     stockdlg_row_text(int table, int row, int col, char *buf, int n);   /* list cell; false = absent / empty */

/* Alarm Setting columns (rdt_dialogs.json order) */
enum { AL_A_TYPE, AL_A_ENITYPE, AL_A_TIME, AL_A_TX, AL_A_RX, AL_A_CHAN, AL_A_SEND, AL_A_CYCLE,
       AL_D_TYPE, AL_D_CHAN, AL_D_TIME, AL_D_TX, AL_D_RX, AL_D_SEND, AL_D_CYCLE, AL_D_ID, AL_D_CALL, AL_D_RECV,
       AL_Q_KIND, AL_Q_GROUP, AL_Q_PRIV, AL_SIGTYPE, AL_W_RESP, AL_W_WARN, AL_W_MODE };
/* Talk Alias (D168UV) / AIR Alias (D890UV) */
enum { TA_TYPE, TA_PRIO, TA_FORMAT };
enum { TA_PRIO_OFF, TA_PRIO_CONTACT, TA_PRIO_ALIAS };   /* stock "Alias RX Dis": Off, Contact First, Alias First */
uint8_t  stockdlg_alias_prio(void);               /* TA_PRIO_*; no codeplug: alias first (the cfw's own behaviour) */
uint8_t  stockdlg_alias_format(void);             /* TX alias form: 0 ISO 8 bit, 1 ISO 7 bit, 2 Unicode (D890UV) */
#endif
