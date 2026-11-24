/* Stock CPS DMR / NXDN sections used by the cfw (codeplug section 10, CSV text written by cfw_cps; plus the user
 * database, section 16). See cfw_cps/docs/CODEPLUG.md "Stock sections used by the firmware".
 *
 *   Radio ID List + channel "Radio ID"         -> per-channel DMR source ID (stockcp_channel_radio_id)
 *   Encryption Code (basic privacy; D168UV: table 72 EncryptionCodeRdt) -> keys of kind 0 (16-bit), cfw scheme DMR_PRIV_CFW_BP
 *   ARC4 Encryption Code                       -> kind 1 (40-bit), DMR_PRIV_CFW_RC4
 *   AES Encryption Code                        -> kind 2 (AES-128) / 3 (AES-256), DMR_PRIV_CFW_AES128 / _AES256
 *     channel "Digital Encryption" N + "AES Digital Encryption" (Normal = Encryption Code row N, Enhanced = AES
 *     key id N) + "ARC4" N (ARC4 key id N, wins)  -> the channel's key (cfwcp_channel: when section 11 sets none)
 *   NX Encryption Code (D890UV)                -> kind 7 NXDN scrambler (15-bit code), channel "NxdnEncry" N = row N
 *   NX Setting (D890UV)                        -> NXDN unit ID (OwnId), monitor (NxdnMoni + NxdnMoniRan any RAN)
 *   NX Talk Group (D890UV)                     -> group names on RX, TxForbid refuses TX to that group
 *   DMR / NX Digital Contact List              -> section 16 (user database): callsign + name of the caller on RX
 *
 * The stock key tables become extra entries of the codeplug key list (cfwcp_key(): section 12 keys first, then these),
 * so every user of keys (RX keyrings, cfw_crypt_tx with its amateur-band gate, Settings > Encryption) sees them. */
#ifndef CFW_STOCKCP_H
#define CFW_STOCKCP_H
#include <stdint.h>
#include <stdbool.h>
#include "cfwcp.h"

void     stockcp_init(void);                   /* (re)build the caches; cfwcp_check() calls it */
int      stockcp_key_count(void);
const cp_key_t *stockcp_key(int i);              /* shared buffer, valid until the next call: copy it */
/* per channel (codeplug channel index): DMR source ID from the Radio ID List (0 = none), key from the stock columns
 * (index into stockcp_key(), 0xFF none; cfwcp_channel() adds the section 12 count) for the channel mode
 * (CFW_MODE_DMR / CFW_MODE_NXDN) */
uint32_t stockcp_channel_radio_id(int ch);
uint8_t  stockcp_channel_key(int ch, uint8_t mode);
/* NX Setting */
uint16_t stockcp_nxdn_own_id(void);           /* 0 = not set (stock default 1 is "not set" too) */
enum { STOCKCP_NXMONI_ON = 1, STOCKCP_NXMONI_ANY_RAN = 2, STOCKCP_NXMONI_ANY_ID = 4 };
uint8_t  stockcp_nxdn_moni(void);
/* NX Talk Group */
bool     stockcp_nxdn_tg(uint32_t id, char *name, int n, bool *tx_forbid);
/* names: user database (section 16) for units, cfw contacts / NX Talk Group for groups. mode = CFW_MODE_*. */
bool     stockcp_id_name(uint8_t mode, uint32_t id, bool group, char *out, int n);
bool     stockcp_call_line(uint8_t mode, uint32_t src, uint32_t dst, bool private_call, char *out, int n);
/* user database, section 16 (24-byte records sorted by key = mode << 24 | id) */
typedef struct { uint32_t key; char call[8]; char name[12]; } cp_user_t;
_Static_assert(sizeof(cp_user_t) == 24, "user record");
#define CP_SEC_USERDB 16
#define CP_USER_DMR   1u
#define CP_USER_NXDN  5u
int      stockcp_user_count(void);
const cp_user_t *stockcp_user_find(uint8_t mode, uint32_t id);

/* model-specific stock table / channel column numbers (cfw_cps/schema/stock_<model>.json order) */
typedef struct {
    uint8_t t_radioid, t_bp, t_aes, t_arc4, t_nxenc, t_nxtg, t_nxset;     /* 0xFF = the model has no such table */
    uint8_t c_rid, c_enctype, c_digenc, c_arc4, c_nxenc;                  /* channel columns, 0xFF none */
} stockcp_map_t;
const stockcp_map_t *stockcp_map(void);
#endif
