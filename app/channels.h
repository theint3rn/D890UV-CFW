#ifndef CFW_CHANNELS_H
#define CFW_CHANNELS_H
#include <stdint.h>
#include <stdbool.h>
#include "modem.h"

typedef struct {
    char       name[17];
    uint32_t   rx_hz;
    uint32_t   tx_hz;        /* 0 = RX only */
    cfw_mode_t mode;
    uint16_t   ctcss_dhz;    /* FM tone, 0.1 Hz units, 0 = none */
    bool       wide;         /* FM 25 kHz */
    /* digital */
    uint8_t    color_code;   /* DMR CC */
    uint8_t    slot;         /* DMR / P25P2: 1..2 */
    uint32_t   talkgroup;    /* DMR / P25 / NXDN */
    uint16_t   nac;          /* P25 */
    uint32_t   wacn;         /* P25 Phase 2 scrambling: WACN + system id */
    uint16_t   sysid;
    uint8_t    ran;          /* NXDN */
    uint8_t    dgid;         /* YSF */
    char       ur[9], rpt1[9], rpt2[9];   /* D-STAR */
    bool       trunk;        /* P25 control channel / DMR Tier III TSCC */
    bool       talkaround;   /* repeater channel used simplex: TX on the RX frequency, direct-mode framing */
    bool       dmr_tdma;     /* DMR simplex: dual-capacity TDMA direct (TS1/TS2) instead of normal direct (MS syncs) */
    /* CPS channel extensions (cfw codeplug section 11) */
    uint8_t    dgid_tx;      /* YSF TX DG-ID + 1, 0 = same as dgid */
    uint8_t    vocoder;      /* CPS vocoder choice (0 auto; the others name the mode's own codec) - informational */
    uint8_t    key;          /* encryption key index, 0xFF none (P25 RX: all P25 keys are tried by ALGID/KID) */
    uint8_t    ysf_mode;     /* 0 DN (V/D mode 2), 1 VW, 2 DATA - stored, TX always DN */
    bool       nxdn96;       /* NXDN 9600 (EFR voice via voc/imbe); false = NXDN48 (EHR) */
    bool       wiresx;       /* YSF Wires-X - stored, not used */
    uint16_t   dstar_rpt;    /* D-STAR repeater list index, 0xFFFF none - stored (RPT1/RPT2 come from the channel) */
    uint8_t    power;        /* TX power 0 low, 1 mid, 2 high, 3 turbo */
    bool       private_call; /* talkgroup is a private (unit) target: contact / manual dial */
    uint8_t    scanlist;     /* codeplug scan list index, 0xFF none */
    bool       scan_skip;
    /* where it came from */
    uint8_t    src;          /* CH_SRC_* */
    uint16_t   cp_slot;      /* codeplug slot number (bank * 128 + index) */
    uint16_t   cp_contact;   /* codeplug DMR contact index (talkgroup table not decoded yet) */
} channel_t;

enum { CH_SRC_BUILTIN = 0, CH_SRC_CODEPLUG = 1 };

/* Memory channels = CPS codeplug channels from NAND (if any) followed by the built-in demo channels.
 * User edits (mode, digital settings) are kept as overrides and saved with the settings. */
void channels_init(void);                /* scans the NAND codeplug (~1 s in the emulator) */
int  channels_count(void);
int  channels_codeplug_count(void);
bool channel_load(int i, channel_t *out);   /* applies the user's override, if any */
void channel_store(int i, const channel_t *c); /* remember an edit (RAM; persisted by radio_save) */
void channel_mode_defaults(channel_t *c);
void channels_reload(void);                 /* after a CPS write (app/prog.c): re-scan, drop overrides */   /* sensible per-mode defaults when the mode changes */

/* Zones (codeplug section 3). Zone -1 = all channels (also when the codeplug has no zones). */
int  zone_count(void);
const char *zone_name(int z);               /* "All channels" for -1 */
int  zone_size(int z);
int  zone_channel(int z, int k);            /* channel index of member k, -1 if invalid */
int  zone_find(int z, int ch);              /* member position of channel ch in zone z, -1 = not a member */
/* contacts (codeplug section 5) are read through cfwcp_contact() */

/* override table persisted by radio_save / restored by radio_init */
typedef struct {
    uint16_t   index;        /* memory channel number this overrides (0xFFFF = VFO) */
    channel_t  ch;
} ch_override_t;
#define CH_MAX_OVERRIDES 16
int  channels_overrides(const ch_override_t **list);
void channels_set_overrides(const ch_override_t *list, int n);

/* VFO bands the radio covers (D890UV: VHF + UHF) */
bool freq_in_band(uint32_t hz);
uint16_t ctcss_from_index(uint8_t idx);     /* CPS tone index -> 0.1 Hz, 0 = invalid */
int  ctcss_count(void);
uint16_t ctcss_at(int i);

#endif
