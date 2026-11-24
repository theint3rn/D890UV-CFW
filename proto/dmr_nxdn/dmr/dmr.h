/* DMR air interface layers 2/3 (ETSI TS 102 361-1/-2/-3), hardware independent.
 *
 * Input: hard dibits (0..3, 01=+3 00=+1 10=-1 11=-3) at 4800 sym/s, one call per symbol.
 * Output: opaque 72-bit vocoder frames (AMBE+2 with its own FEC, untouched) and events.
 * TX: builders that return 132-dibit bursts (+12 CACH dibits for BS-sourced slots).
 * No malloc; the receiver context is a plain struct the caller owns (~2.5 KB).
 */
#ifndef DMR_H
#define DMR_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmr_priv.h"

/* ---------------- constants (TS 102 361-1 9.1.1, 9.3.6) ---------------- */
#define DMR_BURST_DIBITS   132
#define DMR_CACH_DIBITS    12
#define DMR_SLOT_DIBITS    144          /* 30 ms */
#define DMR_VOICE_FRAME_BITS 72
#define DMR_VOICE_FRAME_BYTES 9

typedef enum {
    DMR_SYNC_NONE = 0,
    DMR_SYNC_BS_VOICE, DMR_SYNC_BS_DATA,
    DMR_SYNC_MS_VOICE, DMR_SYNC_MS_DATA, DMR_SYNC_MS_RC,
    DMR_SYNC_TS1_VOICE, DMR_SYNC_TS1_DATA,
    DMR_SYNC_TS2_VOICE, DMR_SYNC_TS2_DATA,
    DMR_SYNC_RESERVED,
    DMR_SYNC_COUNT
} dmr_sync_t;

extern const uint64_t DMR_SYNC_PATTERN[DMR_SYNC_COUNT];
bool dmr_sync_is_voice(dmr_sync_t s);
bool dmr_sync_is_data(dmr_sync_t s);
/* best matching sync for a 48-bit word, if within max_err bit errors */
dmr_sync_t dmr_sync_match(uint64_t w48, int max_err, int *err);

enum {
    DMR_DT_PI_HEADER = 0, DMR_DT_VOICE_LC_HEADER = 1, DMR_DT_TERMINATOR_LC = 2, DMR_DT_CSBK = 3,
    DMR_DT_MBC_HEADER = 4, DMR_DT_MBC_CONT = 5, DMR_DT_DATA_HEADER = 6, DMR_DT_RATE_12 = 7,
    DMR_DT_RATE_34 = 8, DMR_DT_IDLE = 9, DMR_DT_RATE_1 = 10, DMR_DT_USBD = 11,
};

enum { DMR_LCSS_SINGLE = 0, DMR_LCSS_FIRST = 1, DMR_LCSS_LAST = 2, DMR_LCSS_CONT = 3 };

/* CRC masks (table B.21) */
#define DMR_MASK_PI_HEADER   0x6969u
#define DMR_MASK_VOICE_HDR   0x969696u
#define DMR_MASK_TERMINATOR  0x999999u
#define DMR_MASK_CSBK        0xA5A5u
#define DMR_MASK_MBC_HEADER  0xAAAAu
#define DMR_MASK_DATA_HEADER 0xCCCCu
#define DMR_MASK_USBD        0x3333u
#define DMR_MASK_R12_CRC9    0x0F0u
#define DMR_MASK_R34_CRC9    0x1FFu
#define DMR_MASK_R1_CRC9     0x10Fu

/* ---------------- burst access (TS 102 361-1 clause 6, annex E) ---------------- */
uint64_t dmr_burst_sync(const uint8_t burst[DMR_BURST_DIBITS]);
void     dmr_burst_info_bits(const uint8_t burst[DMR_BURST_DIBITS], uint8_t bits196[196]);
/* slot type: returns 0 and fills cc/dt, or -1 if Golay fails */
int      dmr_burst_slot_type(const uint8_t burst[DMR_BURST_DIBITS], uint8_t *cc, uint8_t *dt);
/* voice payload: three 72-bit frames, 9 bytes each, MSB first (VS(215) = frame 0 bit 0) */
void     dmr_burst_voice(const uint8_t burst[DMR_BURST_DIBITS], uint8_t frames[3][9]);
/* EMB + 32 embedded bits of a voice burst B..F; returns 0 or -1 on QR failure */
int      dmr_burst_emb(const uint8_t burst[DMR_BURST_DIBITS], uint8_t *cc, uint8_t *pi, uint8_t *lcss,
                       uint8_t emb32[32]);

void dmr_burst_build_data(uint8_t burst[DMR_BURST_DIBITS], dmr_sync_t sync, uint8_t cc, uint8_t dt,
                          const uint8_t bits196[196]);
void dmr_burst_build_voice_sync(uint8_t burst[DMR_BURST_DIBITS], dmr_sync_t sync, const uint8_t frames[27]);
void dmr_burst_build_voice_emb(uint8_t burst[DMR_BURST_DIBITS], uint8_t cc, uint8_t pi, uint8_t lcss,
                               const uint8_t emb32[32], const uint8_t frames[27]);

/* ---------------- CACH (9.1.4, B.4.1) ---------------- */
typedef struct { uint8_t at, tc, lcss; uint8_t payload[17]; } dmr_cach_t;
int  dmr_cach_decode(const uint8_t dibits[DMR_CACH_DIBITS], dmr_cach_t *c);   /* -1 on Hamming(7,4) failure */
void dmr_cach_encode(const dmr_cach_t *c, uint8_t dibits[DMR_CACH_DIBITS]);

/* ---------------- link control / CSBK / headers (TS 102 361-1 9.1.6..9.2, -2, -4) ---------------- */
enum { DMR_FLCO_GROUP_VOICE = 0x00, DMR_FLCO_UNIT_VOICE = 0x03, DMR_FLCO_TALKER_ALIAS_HDR = 0x04,
       DMR_FLCO_TALKER_ALIAS_B1 = 0x05, DMR_FLCO_TALKER_ALIAS_B2 = 0x06, DMR_FLCO_TALKER_ALIAS_B3 = 0x07,
       DMR_FLCO_GPS_INFO = 0x08, DMR_FLCO_TERM_DATA = 0x30 };

typedef struct {
    uint8_t  raw[9];
    uint8_t  pf, flco, fid, svc_opts;
    uint32_t dst, src;                  /* valid for FLCO 0 / 3 (and most MFID voice LCs) */
} dmr_lc_t;

void dmr_lc_parse(const uint8_t raw[9], dmr_lc_t *lc);
void dmr_lc_make_voice(dmr_lc_t *lc, bool group, uint32_t dst, uint32_t src, uint8_t svc_opts);
/* full LC in a voice header / terminator burst (BPTC + RS(12,9) + mask) */
int  dmr_fulllc_decode(const uint8_t bits196[196], uint32_t mask, dmr_lc_t *lc);   /* -1 on error, else RS fixes */
void dmr_fulllc_encode(const dmr_lc_t *lc, uint32_t mask, uint8_t bits196[196]);

typedef struct {
    uint8_t raw[12];                    /* incl. CRC (unmasked) */
    uint8_t lb, pf, opcode, fid;
    uint8_t data[8];                    /* octets 2..9 */
} dmr_csbk_t;
int  dmr_csbk_decode(const uint8_t bits196[196], uint16_t mask, dmr_csbk_t *c);   /* -1 bad FEC/CRC */
void dmr_csbk_encode(dmr_csbk_t *c, uint16_t mask, uint8_t bits196[196]);
void dmr_csbk_set(dmr_csbk_t *c, uint8_t opcode, uint8_t fid, const uint8_t data[8]);

/* short LC (TS 102 361-2 7.1.x, -4 7.1.2) */
enum { DMR_SLCO_NULL = 0, DMR_SLCO_ACT_UPDT = 1, DMR_SLCO_C_SYS_PARMS = 2, DMR_SLCO_P_SYS_PARMS = 3 };
typedef struct { uint8_t slco; uint32_t data24; } dmr_slc_t;

/* data header (9.2.1 .. 9.2.10) */
enum { DMR_DPF_UDT = 0, DMR_DPF_RESPONSE = 1, DMR_DPF_UNCONFIRMED = 2, DMR_DPF_CONFIRMED = 3,
       DMR_DPF_DEFINED_SHORT = 13, DMR_DPF_RAW_SHORT = 14, DMR_DPF_PROPRIETARY = 15 };
typedef struct {
    uint8_t raw[12];
    uint8_t gi, a, dpf, sap, poc, fmf, bf, s, ns, fsn;
    uint32_t dst, src;
} dmr_data_hdr_t;
int  dmr_data_header_decode(const uint8_t bits196[196], dmr_data_hdr_t *h);
void dmr_data_header_build_unconfirmed(dmr_data_hdr_t *h, bool group, uint32_t dst, uint32_t src,
                                       uint8_t sap, uint8_t blocks, uint8_t pad_octets);
void dmr_data_header_encode(const dmr_data_hdr_t *h, uint8_t bits196[196]);

/* ---------------- receiver ---------------- */
enum { DMR_CRYPT_CLEAR = 0, DMR_CRYPT_DECRYPTED = 1, DMR_CRYPT_ENCRYPTED = 2 };

typedef enum {
    DMR_EV_SYNC_LOST = 1,     /* slot timing lost */
    DMR_EV_VOICE_START,       /* lc valid (header or late entry from embedded LC) */
    DMR_EV_VOICE_LC_EMB,      /* embedded LC decoded during a call (late entry / talker alias, GPS...) */
    DMR_EV_VOICE_END,         /* terminator or timeout */
    DMR_EV_CSBK,
    DMR_EV_MBC,               /* multi-block CSBK: csbk = header block, blocks in data/len */
    DMR_EV_DATA_HEADER,
    DMR_EV_DATA_PACKET,       /* reassembled user data (header + payload) */
    DMR_EV_SHORT_LC,          /* from CACH */
    DMR_EV_PI_HEADER,         /* privacy header: raw 10 octets (cfw privacy ones also give DMR_EV_PRIVACY) */
    DMR_EV_IDLE,
    DMR_EV_RC,
    DMR_EV_PRIVACY,
    /* DMR_EV_RC: data[0] = RC command (DMR_RC_*), crc_ok; slot = backward slot it arrived in */           /* cfw privacy parameters seen (PI header or privacy LC): priv, crypt */
} dmr_ev_type_t;

typedef struct {
    dmr_ev_type_t type;
    uint8_t slot;             /* 0 = TS1, 1 = TS2 */
    uint8_t cc;
    dmr_sync_t sync;
    dmr_lc_t lc;
    dmr_csbk_t csbk;
    dmr_data_hdr_t dh;
    dmr_slc_t slc;
    const uint8_t *data;      /* event specific payload */
    uint16_t len;
    int8_t  crc_ok;           /* data packet: 1 ok, 0 bad, -1 not checked */
    uint8_t crypt;            /* DMR_CRYPT_*: voice/data privacy state */
    dmr_priv_info_t priv;
} dmr_event_t;

typedef struct {
    /* crypt: DMR_CRYPT_CLEAR, _DECRYPTED (frame is plain now), _ENCRYPTED (no key / not synced: mute it) */
    void (*voice)(void *user, uint8_t slot, const uint8_t frame[9], uint8_t index_in_superframe, uint8_t crypt);
    void (*event)(void *user, const dmr_event_t *ev);
    void *user;
} dmr_cb_t;

#ifndef DMR_MAX_DATA_BLOCKS
#define DMR_MAX_DATA_BLOCKS 32          /* per slot reassembly limit (x 24 bytes) */
#endif

typedef struct {
    bool     active;                    /* voice call running */
    bool     lc_valid;
    uint8_t  sf_index;                  /* 0 = burst A ... 5 = F, 0xFF = none */
    uint8_t  emb_bits[128];
    uint8_t  emb_frags;
    uint16_t missed;                    /* bursts without useful content */
    dmr_lc_t lc;
    /* colour code gate (cfg.cc_filter >= 0): voice is delivered only once the call's CC is known good */
    int8_t   cc_state;                  /* 0 unknown, 1 = cc_filter (header / EMB), -1 other CC (EMB) */
    bool     a_held;                    /* burst A kept until the EMB of burst B checks the CC */
    uint8_t  a_burst[DMR_BURST_DIBITS];
    /* data / MBC reassembly */
    dmr_data_hdr_t dh;
    uint8_t  data_dt;                   /* expected block data type */
    uint8_t  blocks_left;
    uint16_t data_len;
    uint8_t  data[DMR_MAX_DATA_BLOCKS * 24];
    dmr_csbk_t mbc;
    uint8_t  mbc_left;
    /* privacy */
    bool     enc;                       /* this call / packet is encrypted */
    bool     data_enc;                  /* next data packet follows a privacy PI header */
    dmr_priv_info_t pinfo;
    dmr_priv_t priv;
} dmr_slot_state_t;

typedef struct {
    int      sync_max_err;              /* bit errors allowed in the 48-bit sync (default 4) */
    int      cc_filter;                 /* -1 = any colour code, else 0..15 */
    bool     bs_cach;                   /* decode the CACH of BS-sourced bursts (ignored for MS / direct syncs) */
} dmr_rx_cfg_t;

typedef struct {
    dmr_rx_cfg_t cfg;
    dmr_cb_t cb;
    uint8_t  ring[256];                 /* last dibits */
    uint32_t pos;                       /* dibits received */
    uint64_t shift;                     /* last 24 dibits as 48 bits */
    bool     locked;
    uint32_t next_end;                  /* absolute position of the next burst end */
    dmr_sync_t pending_sync;
    uint8_t  cur_slot;
    uint16_t unsynced;                  /* consecutive bursts without sync */
    dmr_slot_state_t slot[2];
    uint8_t  slc_bits[68];
    uint8_t  slc_frags;
    uint8_t  last_cc;
    bool     bs_stream;                 /* last sync seen was BS-sourced (CACH present) */
    const dmr_keyring_t *keys;          /* optional: privacy keys (set after dmr_rx_init) */
} dmr_rx_t;

void dmr_rx_init(dmr_rx_t *rx, const dmr_rx_cfg_t *cfg, const dmr_cb_t *cb);
void dmr_rx_dibit(dmr_rx_t *rx, uint8_t dibit);
void dmr_rx_dibits(dmr_rx_t *rx, const uint8_t *d, size_t n);
/* Repeater access timing (TS 102 361-1 5.1.1, table 5.1). With the default aligned channel timing the
 * MS keys logical channel N exactly when the BS sends outbound slot N, so this gives the receive-dibit
 * index (same counter as dmr_rx_dibit calls, rx->pos) at which the next outbound burst of `slot`
 * starts (first dibit after its CACH); +288 per TDMA frame. Returns false if not locked to a BS
 * (CACH-numbered) stream. The platform adds its own demodulator / TX path delays. */
bool dmr_rx_next_slot_start(const dmr_rx_t *rx, uint8_t slot, uint32_t *dibit_index);
/* process one already aligned burst (e.g. from a baseband chip that delivers bursts) */
void dmr_rx_burst(dmr_rx_t *rx, const uint8_t cach[DMR_CACH_DIBITS], const uint8_t burst[DMR_BURST_DIBITS],
                  dmr_sync_t sync, int slot_hint);

/* ---------------- transmitter helpers ---------------- */
typedef struct {
    dmr_lc_t lc;
    uint8_t  cc;
    dmr_sync_t voice_sync;              /* BS_VOICE (repeater out), MS_VOICE (to repeater), TS1/TS2_VOICE (direct) */
    uint8_t  emb_bits[128];
    uint8_t  n;                         /* burst counter inside the superframe 0..5 */
    dmr_priv_t *priv;                   /* optional cfw privacy (dmr_voice_tx_set_priv) */
    uint16_t sf;                        /* superframe number for the privacy keystream */
    uint8_t  priv_bits[128];            /* privacy LC for odd superframes */
} dmr_voice_tx_t;

void dmr_voice_tx_init(dmr_voice_tx_t *t, const dmr_lc_t *lc, uint8_t cc, dmr_sync_t voice_sync);
/* builds the next voice burst A..F from three vocoder frames */
void dmr_voice_tx_burst(dmr_voice_tx_t *t, const uint8_t frames[27], uint8_t burst[DMR_BURST_DIBITS]);
/* data sync variant matching a voice sync (BS_VOICE -> BS_DATA ...) */
dmr_sync_t dmr_sync_data_for(dmr_sync_t voice_sync);

void dmr_tx_voice_header(const dmr_lc_t *lc, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS]);
void dmr_tx_terminator(const dmr_lc_t *lc, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS]);
void dmr_tx_csbk(dmr_csbk_t *c, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS]);
void dmr_tx_idle(uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS]);
/* unconfirmed rate 1/2 data: header + blocks. Returns number of bursts written (max_bursts limit). */
/* encrypt the voice of this transmission: priv must be set up with a key and synced (iv, sf = 0);
 * send dmr_tx_pi_header() before the voice LC header */
void dmr_voice_tx_set_priv(dmr_voice_tx_t *t, dmr_priv_t *priv);
void dmr_tx_pi_header(const dmr_priv_t *priv, uint8_t cc, dmr_sync_t data_sync, uint8_t burst[DMR_BURST_DIBITS]);
/* priv: NULL = clear, else a PI header is written first and the user data is encrypted */
int  dmr_tx_data_unconfirmed(bool group, uint32_t dst, uint32_t src, uint8_t sap, const uint8_t *payload,
                             size_t len, dmr_priv_t *priv, uint8_t cc, dmr_sync_t data_sync,
                             uint8_t bursts[][DMR_BURST_DIBITS], int max_bursts);
/* CACH stream for a BS: emits CACH n of a 4-burst short LC (n = 0..3) */
void dmr_tx_cach_slc(const dmr_slc_t *slc, unsigned n, uint8_t at, uint8_t tc, uint8_t dibits[DMR_CACH_DIBITS]);

/* Annex D idle message info bits (96 bits) */
extern const uint8_t DMR_IDLE_INFO[12];

/* ---------------- link modes: which syncs / CACH a transmitter uses (TS 102 361-1 4.4, 5.1.4) ----------------
 * DIRECT        simplex / talkaround, MS-sourced syncs, one burst every 60 ms, other slot = backward (RC)
 * TDMA_DIRECT   dual capacity direct mode: TS1 / TS2 direct syncs, timing from the channel timing leader
 * REPEATER      MS -> BS inbound: MS-sourced syncs in the repeater's slot
 * BS            BS / hotspot outbound: BS syncs, continuous, CACH in front of every burst
 * Receivers take every sync type on any channel; nothing to configure for RX besides bs_cach. */
typedef enum { DMR_LINK_DIRECT = 0, DMR_LINK_TDMA_DIRECT, DMR_LINK_REPEATER, DMR_LINK_BS } dmr_link_t;
dmr_sync_t dmr_link_voice_sync(dmr_link_t link, uint8_t slot);
dmr_sync_t dmr_link_data_sync(dmr_link_t link, uint8_t slot);
bool       dmr_sync_is_bs(dmr_sync_t s);
bool       dmr_sync_is_ms(dmr_sync_t s);       /* MS-sourced incl. RC */
bool       dmr_sync_is_direct(dmr_sync_t s);   /* TDMA direct TS1/TS2 */

/* ---------------- reverse channel (6.4.1 standalone, 6.4.2 embedded, B.2.2.2, B.3.13; -2 table 7.27) ---------------- */
enum { DMR_RC_CEASE_TX_CMD = 0x4, DMR_RC_CEASE_TX_REQ = 0x5 };
#define DMR_RC_BURST_DIBITS 48                 /* standalone RC burst, centred in the slot (L24..R24) */
#define DMR_RC_BURST_OFFSET 42                 /* its first dibit inside a 132-dibit burst window */
uint16_t dmr_rc_info(uint8_t cmd);             /* 4-bit payload + masked CRC-7 -> 11 bits */
int      dmr_rc_check(uint16_t info11, uint8_t *cmd);   /* 0 ok, -1 CRC error */
/* standalone RC burst (table E.12): EMB(CC, PI=1, LCSS single) + RC BPTC + RC sync */
void dmr_tx_rc_standalone(uint8_t cc, uint8_t cmd, uint8_t rc[DMR_RC_BURST_DIBITS]);
/* 32 embedded bits for an outbound voice burst carrying RC (use with PI = 1, LCSS single) */
void dmr_rc_emb32(uint8_t cmd, uint8_t emb32[32]);

/* ---------------- talkspurt transmitter for any link ----------------
 * Yields one slot (144 dibits = 12 CACH/guard + 132 burst) per call. For DIRECT / TDMA_DIRECT /
 * REPEATER the 12 leading dibits are PA ramp/guard (not keyed, zero here) and the radio sends this slot
 * every 60 ms in its own timeslot; the other slot stays receive (RC). For BS the 12 dibits are a CACH
 * (AT = 1, TC = slot, short LC from slc if set) and the platform fills the other slot (dmr_tx_idle). */
typedef enum { DMR_TXK_PI = 1, DMR_TXK_HEADER, DMR_TXK_VOICE, DMR_TXK_TERMINATOR } dmr_txk_t;
typedef struct {
    dmr_link_t link;
    uint8_t slot, cc, n_headers, sent_headers, cach_n;
    bool    pi_pending, ended;
    dmr_lc_t lc;
    dmr_voice_tx_t v;
    dmr_priv_t *priv;
    const dmr_slc_t *slc;               /* BS: short LC to cycle in the CACH (NULL = null message) */
} dmr_call_tx_t;
/* n_headers: voice LC headers before the first superframe (>= 1; 2 is a common choice) */
void dmr_call_tx_start(dmr_call_tx_t *t, dmr_link_t link, uint8_t slot, uint8_t cc, const dmr_lc_t *lc,
                       uint8_t n_headers, dmr_priv_t *priv);
/* next slot: PI header / headers first, then voice bursts A..F from frames27 (3 x 9 bytes) */
dmr_txk_t dmr_call_tx_next(dmr_call_tx_t *t, const uint8_t frames27[27], uint8_t out[DMR_SLOT_DIBITS]);
void      dmr_call_tx_end(dmr_call_tx_t *t, uint8_t out[DMR_SLOT_DIBITS]);   /* terminator with LC */

#endif
