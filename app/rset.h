/* Radio settings edited in the radio menu (Menu > Settings), mapped to the codeplug:
 *   - stock Optional Settings (codeplug section 10, table 21 = OptionalSetting, value = stock CSV value), the columns
 *     are the same on the D890UV and the D168UV for everything used here;
 *   - cfw settings record fields (PF key functions) and per-channel TX power.
 * Edits go into the codeplug's radio edits journal (cfwcp_journal_write) so the CPS reads them back; without a cfw
 * codeplug (or one written by a CPS without journal space) they are kept in the radio's settings blob instead.
 * Value lists = the stock radio's option lists (D168UV/emu/MENUS.md, index = stored value). */
#ifndef CFW_RSET_H
#define CFW_RSET_H
#include <stdint.h>
#include <stdbool.h>
#include "cfwcp.h"

typedef enum {
    RS_SQL, RS_TOT, RS_VOX, RS_BEEP, RS_MAXVOL, RS_BKL, RS_BKT, RS_PSAVE, RS_APO, RS_KEYLOCK, RS_TIMEDISP,
    RS_COUNT
} rs_id_t;

typedef struct {
    const char *label;
    uint8_t col;                     /* OptionalSetting column (stock CSV) */
    uint8_t n;                       /* number of values */
    const char *const *opt;
    const char *effect;              /* what the cfw does with it (Settings help line) */
} rs_def_t;

/* key functions (cfw settings record keys[], cfw_cps KEY_FUNCS) */
enum { KF_NONE, KF_MODE_NEXT, KF_MODE_PICKER, KF_VFO_MEM, KF_KEY_LOCK, KF_MONITOR, KF_SCAN, KF_TEST_TONE, KF_ZONE_NEXT,
       KF_TALKAROUND, KF_TX_POWER, KF_CALL_LOG, KF_GPS_INFO, KF_ALARM, KF_COUNT };
enum { RK_P1, RK_P2, RK_PF1, RK_PF2, RK_PF3, RK_PF3L, RK_COUNT };

void     rset_init(void);                     /* after the codeplug is checked (boot, CPS write) */
const rs_def_t *rset_def(rs_id_t id);
uint8_t  rset_get(rs_id_t id);
void     rset_set(rs_id_t id, uint8_t v);
const char *rset_text(rs_id_t id);
const char *rset_key_name(uint8_t fn);
const char *rset_key_slot_name(int slot);    /* board key name for RK_* (NULL = key not on this radio) */
uint8_t  rset_key(int slot);
void     rset_set_key(int slot, uint8_t fn);
bool     rset_channel_power(int ch, uint8_t *power);   /* radio edit of a channel's TX power */
void     rset_set_channel_power(int ch, uint8_t power);
bool     rset_dirty(void);
bool     rset_save(void);                     /* journal (or false: caller persists the edits in the settings blob) */
bool     rset_in_codeplug(void);              /* edits go into the codeplug journal */
int      rset_edits(const cp_edit_t **list);  /* for the settings blob */
void     rset_set_edits(const cp_edit_t *list, int n);

void     rset_set_opt(uint8_t col, uint32_t v);          /* generic stock Optional Settings edit (journal) */
bool     rset_opt_pending(uint8_t col, uint32_t *v);    /* edited in the radio (not yet in the codeplug) */
bool     rset_encryption(void);               /* privacy keys allowed (off by default; illegal on amateur bands) */
void     rset_set_encryption(bool on);
bool     rset_sms_mformat(void);              /* SMS TX format: true = M-SMS (Motorola TMS over IP), false = cfw */
void     rset_set_sms_mformat(bool on);

bool     rset_batt_percent(void);             /* status bar battery: true = percent, false = icon */
void     rset_set_batt_percent(bool on);

/* derived values */
uint16_t rset_volume_q8(void);                /* volume limit as a gain, 256 = full */
bool     rset_gps_on(void);                   /* stock Gps (codeplug; GPS menu not in the cfw) */
bool     rset_mandown(void);                  /* stock FailAlarm = man down on (codeplug only, as stock) */
uint8_t  rset_mandown_wait(void);             /* stock ManDownWait, seconds */
uint32_t rset_tot_s(void);                    /* 0 = off */
uint32_t rset_apo_s(void);
uint32_t rset_backlight_s(void);              /* 0 = always on */
uint8_t  rset_sql_level(void);                /* 0 = open .. 5 */

#endif
