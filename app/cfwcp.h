/* cfw codeplug ("D8CP", format v1) in internal flash 0x08180000-0x081DFFFF (384 KB).
 * Written only by the CPS link (app/prog.c) through cfwcp_write_*; read in place (memory mapped).
 * Format: D890UV_src/cfw_cps/docs/CODEPLUG.md (the CPS, cfw_cps/cps/codeplug.py, is the reference encoder). */
#ifndef CFW_CFWCP_H
#define CFW_CFWCP_H
#include <stdint.h>
#include <stdbool.h>
#include "channels.h"
#if defined(__has_include)
#if __has_include("board_cfg.h")
#include "board_cfg.h"                     /* another board's flash layout (D168UV/cfw) */
#endif
#endif

#ifndef CFWCP_BASE
#define CFWCP_BASE        0x08180000u
#define CFWCP_SIZE        0x00060000u      /* 48 pages of 8 KB, bank 2 pages 64..111 */
#define CFWCP_PAGE        0x2000u
#define CFWCP_FIRST_PAGE  64u
#define CFWCP_D890_LAYOUT 1
#endif
#define CFWCP_MAGIC       0x50433844u      /* "D8CP" little endian */
#ifndef CFWCP_MODEL_ID
#define CFWCP_MODEL_ID    0u               /* header u16 at 18: 0 = D890UV, 1 = D168UV (board_cfg.h); other ids rejected */
#endif
#define CFWCP_VERSION     1u

enum {
    CP_SEC_SETTINGS = 1, CP_SEC_CHANNELS = 2, CP_SEC_ZONES = 3, CP_SEC_SCANLISTS = 4, CP_SEC_CONTACTS = 5,
    CP_SEC_TGLISTS = 6, CP_SEC_TRUNKSYS = 7, CP_SEC_POOL32 = 8, CP_SEC_POOL16 = 9,
    CP_SEC_STOCK = 10,          /* stock CPS data (byte stream, CPS only) */
    CP_SEC_CHEXT = 11, CP_SEC_KEYS = 12, CP_SEC_BANDPLAN = 13, CP_SEC_DSTAR_RPT = 14, CP_SEC_DIGISET = 15,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;      /* 32 */
    uint32_t total_len;
    uint32_t crc;              /* CRC-32 (IEEE) of bytes [16, total_len) */
    uint16_t n_sections;
    uint16_t flags;            /* radio model id (CFWCP_MODEL_ID): 0 = D890UV, 1 = D168UV */
    uint32_t generation;       /* CPS write counter */
    uint32_t created;          /* unix time, informational */
    uint32_t reserved;
} cp_header_t;

typedef struct {
    uint16_t id, rec_size;
    uint32_t count, offset, reserved;
} cp_section_t;

typedef struct {
    char     callsign[8];
    uint32_t dmr_id, p25_unit_id;
    uint16_t nxdn_unit_id, flags;          /* flags: b0 rx_invert, b1 key beep, b2 auto key lock, b3 boot in VFO, b4 encryption on,
                                            * b5 SMS format M-SMS (else cfw), b6 battery shown as percent (else icon) */
    uint32_t step_hz;
    uint16_t deemph_us, test_tone_hz;
    uint8_t  backlight_level, backlight_timeout_s, vfo_mode, keylock_timeout_s;
    uint16_t start_channel;                /* 0xFFFF = keep last used */
    uint8_t  keys[6];                      /* P1 P2 PF1 PF2 PF3 PF3-long: function codes (CODEPLUG.md) */
    uint8_t  dmr_cc, dmr_slot; uint16_t pad0;
    uint32_t dmr_tg;
    uint16_t p25_nac; uint8_t nxdn_ran, ysf_dgid;
    uint32_t p25_tg, nxdn_group, p25_wacn;
    uint16_t p25_sysid, pad1;
    char     ur[8], rpt1[8], rpt2[8], dstar_msg[20], radio_name[16];
} cp_settings_t;

typedef struct {
    char     name[16];
    uint32_t rx_hz, tx_hz;
    uint8_t  mode, flags, power, scanlist;  /* flags: b0 wide, b1 trunk, b2 scan skip */
    uint16_t rx_tone, tx_tone;              /* 0 none; CTCSS 0.1 Hz; 0x8000|code DCS (bit 14 inverted) */
    union {
        uint8_t raw[32];
        struct { uint8_t cc, slot, flags, pad; uint32_t tg; uint16_t contact, tglist, trunksys; } dmr;
        struct { uint16_t nac; uint8_t slot, pad; uint32_t tg; uint16_t contact, tglist, trunksys, sysid; uint32_t wacn; } p25;
        struct { uint8_t ran, pad[3]; uint32_t group; uint16_t contact, tglist, trunksys; } nxdn;
        struct { char ur[8], rpt1[8], rpt2[8]; } dstar;
        struct { uint8_t dgid; } ysf;
    } m;
} cp_channel_t;

typedef struct { char name[16]; uint32_t id; uint8_t kind, mode; uint16_t reserved; } cp_contact_t;
/* zone / TG list (24 B): members are pool16 entries (zone: channel indices, TG list: contact indices) */
typedef struct { char name[16]; uint32_t first; uint16_t count, reserved; } cp_list_t;
/* scan list (28 B): members in pool16, priority channels 0xFFFF = none, dwell in 100 ms units */
typedef struct { char name[16]; uint32_t first; uint16_t count, pri1, pri2; uint8_t dwell, flags; } cp_scanlist_t;
/* per-channel digital extensions (only channels that differ from the defaults have one) */
typedef struct {
    uint16_t channel;
    uint8_t  vocoder, flags;          /* flags: b0 talkaround (TX on the RX frequency), b1 Wires-X, b2 DMR TDMA direct */
    uint8_t  key;                     /* index into keys, 0xFF none */
    uint8_t  nxdn_rate;               /* 0 NXDN48, 1 NXDN96 */
    uint8_t  dgid_tx;                 /* YSF TX DG-ID, 0xFF = RX DG-ID */
    uint8_t  ysf_mode;                /* 0 DN, 1 VW, 2 DATA */
    uint16_t dstar_rpt;               /* index into the D-STAR repeater list, 0xFFFF none */
    uint8_t  reserved[22];
} cp_chext_t;
/* user-entered encryption key (kind: 0 DMR BP, 1 DMR ARC4, 2 DMR AES128, 3 DMR AES256, 4 P25 DES, 5 P25 AES256,
 * 6 P25 ADP, 7 NXDN scrambler, 8 NXDN DES, 9 NXDN AES) */
typedef struct { char name[16]; uint8_t kind, algid; uint16_t key_id; uint8_t len, pad[3]; uint8_t key[32]; } cp_key_t;
/* P25 IDEN_UP / NXDN channel plan entry of a trunk system */
typedef struct { uint16_t trunksys; uint8_t iden, flags; uint32_t base_hz, spacing_hz; int32_t tx_offset_hz;
                 uint32_t bw_hz; uint8_t slots, pad[3]; } cp_bandplan_t;
typedef struct {
    char name[16]; uint8_t type, flags; uint16_t nac_cc; uint32_t wacn; uint16_t sysid, tglist;
    uint32_t cc_first; uint16_t cc_count, reserved; uint32_t chan_step_hz, base_hz, reserved2;
} cp_trunksys_t;

_Static_assert(CFWCP_SIZE / CFWCP_PAGE <= 64, "s_erased in cfwcp.c is a 64-bit page mask");
#ifdef CFWCP_D890_LAYOUT
_Static_assert(CFWCP_BASE == 0x08100000u + CFWCP_FIRST_PAGE * CFWCP_PAGE, "first page = base (bank 2)");
_Static_assert(CFWCP_BASE >= 0x08180000u && CFWCP_BASE + CFWCP_SIZE <= 0x081E0000u,
               "codeplug must stay between the image end (ld ASSERT) and the settings page / stock cache");
#endif
_Static_assert(sizeof(cp_header_t) == 32, "header");
_Static_assert(sizeof(cp_section_t) == 16, "section");
_Static_assert(sizeof(cp_settings_t) == 128, "settings");
_Static_assert(sizeof(cp_channel_t) == 64, "channel");
_Static_assert(sizeof(cp_contact_t) == 24, "contact");
_Static_assert(sizeof(cp_list_t) == 24, "zone / tg list");
_Static_assert(sizeof(cp_scanlist_t) == 28, "scan list");
_Static_assert(sizeof(cp_trunksys_t) == 48, "trunksys");
_Static_assert(sizeof(cp_chext_t) == 32, "chext");
_Static_assert(sizeof(cp_key_t) == 56, "key");
_Static_assert(sizeof(cp_bandplan_t) == 24, "bandplan");

/* reading (memory mapped) */
bool     cfwcp_check(void);              /* re-validate (magic, version, length, CRC, section bounds) */
bool     cfwcp_valid(void);              /* last cfwcp_check() result (checked once at boot if never called) */
int      cfwcp_channel_count(void);
bool     cfwcp_channel(int i, channel_t *out);
const cp_settings_t *cfwcp_settings(void);
uint32_t cfwcp_len(void);
uint32_t cfwcp_crc(void);
uint32_t cfwcp_generation(void);
void     cfwcp_apply_radio(void);        /* codeplug radio settings -> g_radio (defaults; persisted edits win) */
const cp_chext_t *cfwcp_chext(int channel);                 /* NULL = all defaults */
int      cfwcp_key_count(void);                             /* section 12 keys + the stock key tables (stockcp.h) */
const cp_key_t *cfwcp_key(int i);                           /* user-entered keys (P25 / DMR / NXDN), then stock ones */
int      cfwcp_own_key_count(void);                         /* section 12 only */
/* any section by id (NULL if absent or the record size differs) */
const void *cfwcp_section(uint16_t id, uint16_t rec_size, uint32_t *count);
/* walk the stock section 10 entries (u8 table, u8 col, u16 row, u16 len, text), journal included; fn false = stop */
typedef bool (*cfwcp_walk_fn)(const uint8_t *e, uint32_t pos, void *u);
void     cfwcp_stock_walk(cfwcp_walk_fn fn, void *u);
int      cfwcp_bandplans(int trunksys, const cp_bandplan_t **first, int max);   /* copies pointers, returns count */
/* stock CPS setting stored by the CPS (section 10, CSV text): table = index in the stock .LST order (21 = OptionalSetting,
 * 34 = NXSetting, 19 = APRS, 22 = AlertTone), col = CSV column (cfw_cps/schema/stock_d890uv.json). Record tables only hold
 * values that differ from the stock default: returns false when the value is the default (or absent). */
bool     cfwcp_stock_value(int table, int col, char *buf, int n);   /* the last entry wins (radio edits follow) */
bool     cfwcp_stock_value_row(int table, int col, int row, char *buf, int n);   /* list tables (col 0xFF row 0 = rows) */
/* zones, scan lists, contacts, TG lists (NULL / 0 without a codeplug) */
int      cfwcp_zone_count(void);
const cp_list_t *cfwcp_zone(int i);
int      cfwcp_tglist_count(void);
const cp_list_t *cfwcp_tglist(int i);
int      cfwcp_scanlist_count(void);
const cp_scanlist_t *cfwcp_scanlist(int i);
int      cfwcp_contact_count(void);
const cp_contact_t *cfwcp_contact(int i);
uint16_t cfwcp_member(uint32_t first, int k);                 /* pool16[first + k], 0xFFFF if outside */

/* Radio edits journal (settings changed in the radio menu, read back by the CPS). It lives in the spare zero bytes
 * the CPS leaves at the end of the stock section 10 (cfw_cps: STOCK_SPARE): a marker entry (table 0xEE), then the
 * edit entries (same format as the stock entries: u8 table, u8 col, u16 row, u16 len, text), then a 4-byte CRC fix
 * entry (table 0xEF) that keeps the codeplug CRC in the header unchanged, so only the flash page(s) holding the
 * journal are rewritten. Tables: 21 = stock OptionalSetting (stock CSV value), 0xE0 = cfw settings record (col =
 * CP_SET_* field, decimal text), 0xE2 = cfw channel field (col = CP_CH_*, row = channel index, decimal text). */
#define CP_JT_OPTIONAL  21u
#define CP_JT_SETTINGS  0xE0u
#define CP_JT_CHANNEL   0xE2u
#define CP_JT_MARK      0xEEu
#define CP_JT_CRCFIX    0xEFu
enum { CP_SET_KEY_P1 = 0, CP_SET_KEY_P2, CP_SET_KEY_PF1, CP_SET_KEY_PF2, CP_SET_KEY_PF3, CP_SET_KEY_PF3L,
       CP_SET_ENCRYPTION = 6, CP_SET_SMS_FORMAT = 7, CP_SET_START_ZONE = 8, CP_SET_START_CH = 9, CP_SET_BATT_PCT = 10 };
enum { CP_CH_POWER = 0 };
typedef struct { uint8_t table, col; uint16_t row; char text[8]; } cp_edit_t;
int      cfwcp_journal_spare(void);       /* bytes the journal may use (0 = no room / no codeplug) */
int      cfwcp_journal_read(cp_edit_t *out, int max);             /* edits stored in the journal now */
bool     cfwcp_journal_write(const cp_edit_t *e, int n);          /* replace the journal; false = no room / HW build */
uint32_t cfwcp_crc32(const void *p, uint32_t n);
bool     cfwcp_recover(void);            /* boot: finish a journal rewrite cut by power loss (redo copy) */

/* writing (only app/prog.c). Offsets are relative to CFWCP_BASE; 16-byte aligned, multiple of 16. */
bool     cfwcp_write_allowed(void);      /* false in HW=1 builds */
void     cfwcp_write_begin(void);        /* forget which pages were erased in this session */
bool     cfwcp_write(uint32_t off, const uint8_t *data, uint32_t len);   /* erases a page on first touch */
void     cfwcp_erase_page_off(uint32_t off);
#endif
