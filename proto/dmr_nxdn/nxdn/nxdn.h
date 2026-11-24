/* NXDN air interface (NXDN TS 1-A v1.3 common air interface; TS 1-E v1.1 Type-D RTCH2/SCCH).
 *
 * Input: hard dibits (01=+3 00=+1 10=-1 11=-3), 2400 sym/s (4800 bps) or 4800 sym/s (9600 bps);
 * the framing is the same 384-bit / 192-symbol frame at both rates.
 * Output: opaque VCH vocoder frames (EHR: 72 bits = AMBE+2 49 bits + the vocoder's own FEC;
 * EFR: 144 bits) and layer 3 messages. TX: frame builders. No malloc.
 */
#ifndef NXDN_H
#define NXDN_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nxdn_crypt.h"

#define NXDN_FRAME_DIBITS 192
#define NXDN_FSW          0xCDF59u        /* 4.4.4, 20 bits */
#define NXDN_PREAMBLE     0x5775FDu       /* 4.4.3, 24 bits */
#define NXDN_POST_TAIL    0x375FDu        /* last 9 post/preamble symbols -3 +3 -3 +3 +3 -3 -3 -3 +3 (18 bits) */

/* ---------------- LICH (4.5.3, 5.2) ---------------- */
enum { NXDN_RF_RCCH = 0, NXDN_RF_RTCH = 1, NXDN_RF_RDCH = 2, NXDN_RF_RTCH_C = 3 /* = Type-D RTCH2 */ };
enum { NXDN_USC_SACCH_NS = 0, NXDN_USC_UDCH = 1, NXDN_USC_SACCH_SS = 2, NXDN_USC_SACCH_SS_IDLE = 3 };
enum { NXDN_CAC_OUT = 0, NXDN_CAC_LONG = 1, NXDN_CAC_SHORT = 3 };
enum { NXDN_STEAL_BOTH = 0, NXDN_STEAL_FIRST = 1, NXDN_STEAL_SECOND = 2, NXDN_STEAL_NONE = 3 };

typedef struct { uint8_t rf, fct, option, dir; } nxdn_lich_t;
uint8_t nxdn_lich_pack(const nxdn_lich_t *l);                    /* 7-bit value */
void    nxdn_lich_unpack(uint8_t v7, nxdn_lich_t *l);
void    nxdn_lich_encode(uint8_t v7, uint8_t dibits[8]);
int     nxdn_lich_decode(const uint8_t dibits[8], uint8_t *v7);  /* -1 parity error */

/* ---------------- scrambler (4.6) ---------------- */
/* XOR (sign inversion) for symbols [first, last) of a frame, PN restarted per frame at symbol 10 */
void nxdn_scramble(uint8_t frame[NXDN_FRAME_DIBITS], unsigned first, unsigned last);

/* ---------------- functional channel coding (4.5.1, 4.5.2, TS 1-E 4.5.1.1) ----------------
 * Each takes the information bits as bytes (SR octet first where the channel has one).
 * Encoders produce channel bits (after interleave); decoders return 0 or -1 (CRC fail)
 * and the Viterbi path metric in *metric when not NULL.
 */
typedef enum { NXDN_CH_SACCH, NXDN_CH_FACCH1, NXDN_CH_UDCH, NXDN_CH_CAC, NXDN_CH_LCAC, NXDN_CH_SCAC,
               NXDN_CH_SCCH, NXDN_CH_COUNT } nxdn_ch_t;
unsigned nxdn_ch_info_bits(nxdn_ch_t ch);     /* 26, 80, 184, 152, 136, 104, 25 */
unsigned nxdn_ch_coded_bits(nxdn_ch_t ch);    /* 60, 144, 348, 300, 252, 252, 60 */
void nxdn_ch_encode(nxdn_ch_t ch, const uint8_t *info, uint8_t *bits);
int  nxdn_ch_decode(nxdn_ch_t ch, const uint8_t *bits, uint8_t *info, int *metric);

/* ---------------- layer 3 messages (6.4) ---------------- */
enum {
    NXDN_MSG_VCALL = 0x01, NXDN_MSG_VCALL_IV = 0x03, NXDN_MSG_TX_REL_EX = 0x07, NXDN_MSG_TX_REL = 0x08,
    NXDN_MSG_DCALL_HDR = 0x09, NXDN_MSG_DCALL_DATA = 0x0B, NXDN_MSG_DCALL_ACK = 0x0C, NXDN_MSG_HEAD_DLY = 0x0F,
    NXDN_MSG_IDLE = 0x10, NXDN_MSG_DISC = 0x11,
    NXDN_MSG_SITE_INFO = 0x18, NXDN_MSG_SRV_INFO = 0x19, NXDN_MSG_CCH_INFO = 0x1A, NXDN_MSG_ADJ_SITE_INFO = 0x1B,
    NXDN_MSG_FAIL_STAT_INFO = 0x1C,
    NXDN_MSG_REG = 0x20, NXDN_MSG_REG_C = 0x22, NXDN_MSG_REG_COMM = 0x23, NXDN_MSG_GRP_REG = 0x24,
    NXDN_MSG_PROP_FORM = 0x3F,
    /* trunked control channel (same codes, direction tells request/response) */
    NXDN_MSG_VCALL_REQ = 0x01, NXDN_MSG_VCALL_RESP = 0x01, NXDN_MSG_VCALL_REC_REQ = 0x02,
    NXDN_MSG_VCALL_CONN = 0x03, NXDN_MSG_VCALL_ASSGN = 0x04, NXDN_MSG_VCALL_ASSGN_DUP = 0x05,
    NXDN_MSG_DCALL_ASSGN = 0x0E, NXDN_MSG_DCALL_ASSGN_DUP = 0x0D,
};
enum { NXDN_CALL_BROADCAST = 0, NXDN_CALL_CONFERENCE = 1, NXDN_CALL_UNSPEC = 2, NXDN_CALL_INDIVIDUAL = 4,
       NXDN_CALL_INTERCONNECT = 6, NXDN_CALL_SPEED_DIAL = 7 };
#define NXDN_CAUSE_REG_ACCEPTED 0x01u

typedef struct {
    uint8_t type, cc_option, call_type, call_option, cipher, key_id;
    uint16_t src, dst;
    uint8_t call_timer;           /* VCALL_ASSGN */
    uint16_t channel;             /* VCALL_ASSGN / DCALL_ASSGN */
} nxdn_call_t;
void nxdn_msg_parse_call(const uint8_t *m, nxdn_call_t *c);
unsigned nxdn_msg_build_vcall(uint8_t *m, uint8_t type, uint8_t call_type, uint16_t src, uint16_t dst);
unsigned nxdn_msg_build_assgn(uint8_t *m, uint8_t call_type, uint16_t src, uint16_t dst, uint16_t channel);

typedef struct { uint8_t category; uint32_t system; uint16_t site; } nxdn_locid_t;
void nxdn_locid_parse(uint32_t loc24, nxdn_locid_t *l);

typedef struct {
    uint32_t loc;
    nxdn_locid_t lid;
    uint16_t chan_struct, service;
    uint32_t restriction, access;
    uint8_t version, adj_alloc;
    uint16_t cc1, cc2;
} nxdn_site_info_t;
void nxdn_msg_parse_site_info(const uint8_t *m, nxdn_site_info_t *s);
unsigned nxdn_msg_build_site_info(uint8_t *m, const nxdn_site_info_t *s);

typedef struct { uint32_t loc; uint8_t curr, new_, add, del; uint16_t cc1, cc2; } nxdn_cch_info_t;
void nxdn_msg_parse_cch_info(const uint8_t *m, nxdn_cch_info_t *c);

typedef struct { uint8_t option; uint32_t loc; uint16_t unit, group; uint8_t cause; } nxdn_reg_t;
void nxdn_msg_parse_reg(const uint8_t *m, nxdn_reg_t *r);            /* REG_REQ / REG_RESP */
unsigned nxdn_msg_build_reg_req(uint8_t *m, uint8_t loc_category, uint32_t system, uint16_t unit, uint16_t group);
unsigned nxdn_msg_build_grp_reg_req(uint8_t *m, uint16_t unit, uint16_t group);
unsigned nxdn_msg_build_vcall_req(uint8_t *m, uint8_t call_type, uint16_t src, uint16_t dst);

/* ---------------- receiver ---------------- */
typedef enum {
    NXDN_EV_SYNC = 1, NXDN_EV_SYNC_LOST,
    NXDN_EV_LICH,             /* every decoded frame: lich */
    NXDN_EV_SACCH,            /* one SACCH quarter (sr + 18 bits) */
    NXDN_EV_SACCH_MSG,        /* complete superframe SACCH message (9 octets) or single */
    NXDN_EV_FACCH1,           /* 10 octet message */
    NXDN_EV_UDCH, NXDN_EV_FACCH2,      /* SR + 22 octets */
    NXDN_EV_CAC,              /* outbound: SR + 18 octets; inbound long: SR + 16; short: SR + 12 */
    NXDN_EV_SCCH,             /* Type-D: 25 bits in data[0..3] (structure = top 2 bits) */
    NXDN_EV_VOICE_START, NXDN_EV_VOICE_END,
    NXDN_EV_CRC_ERROR,
    NXDN_EV_CRYPT,            /* encrypted call/packet seen: crypt = DECRYPTED (key found) or ENCRYPTED */
} nxdn_ev_type_t;

enum { NXDN_CRYPT_CLEAR = 0, NXDN_CRYPT_DECRYPTED = 1, NXDN_CRYPT_ENCRYPTED = 2 };

typedef struct {
    nxdn_ev_type_t type;
    nxdn_lich_t lich;
    nxdn_ch_t ch;
    uint8_t sr;               /* structure (2) | RAN (6) */
    const uint8_t *data;      /* message octets after SR */
    uint16_t len;
    nxdn_call_t call;         /* VOICE_START / VOICE_END */
    uint8_t crypt;            /* NXDN_CRYPT_* (UDCH data: decrypted in place when a key is known) */
} nxdn_event_t;

typedef struct {
    /* frame: 72 (EHR) or 144 (EFR) bit VCH. pos = (SACCH quarter << 2) | VCH index, or NXDN_VPOS_UNKNOWN.
     * crypt: CLEAR; DECRYPTED (the frame is already plain, EHR / EFR FEC re-applied); ENCRYPTED (no key /
     * no IV yet: mute it). */
    void (*voice)(void *user, const uint8_t *frame, unsigned nbits, uint8_t pos, uint8_t crypt);
    void (*event)(void *user, const nxdn_event_t *ev);
    void *user;
} nxdn_cb_t;

typedef struct {
    int  fsw_max_err;         /* bit errors in the 20-bit FSW (default 2, +2 on the flywheel) */
    bool efr;                 /* 9600 bps EFR voice (2 x 144-bit VCH) */
    bool type_d;              /* RF type 3 = Type-D RTCH2 (SCCH in place of SACCH) */
    int  ran_filter;          /* -1 any */
} nxdn_rx_cfg_t;

typedef struct {
    nxdn_rx_cfg_t cfg;
    nxdn_cb_t cb;
    uint8_t ring[256];
    uint32_t pos;
    uint32_t shift;
    bool locked;
    uint32_t frame_end;
    uint32_t alt_end;             /* end of the frame of an unconfirmed FSW that a later FSW replaced (0 = none) */
    uint8_t missed;
    bool fsw_ok;
    bool confirmed;               /* a second FSW arrived where expected */
    bool in_call;
    nxdn_call_t call;
    uint8_t sacch[9];
    uint8_t sacch_mask;
    uint8_t last_facch[10];
    bool last_facch_valid;
    /* security (TS 1-D) */
    const nxdn_keyring_t *keys;   /* optional, set after nxdn_rx_init */
    nxdn_vcrypt_t vc;             /* voice session of the current call */
    nxdn_ks_t dks;                /* data packet session */
    uint8_t dcrypt;               /* NXDN_CRYPT_* of the current data packet */
    uint8_t last_rf;              /* RF channel type of the last frame with a good FSW */
    bool ctl_ok;                  /* SACCH / SCCH CRC good in the current frame */
    bool ran_bad;                 /* SACCH RAN differs from cfg.ran_filter: voice stays muted */
} nxdn_rx_t;

void nxdn_rx_init(nxdn_rx_t *rx, const nxdn_rx_cfg_t *cfg, const nxdn_cb_t *cb);
void nxdn_rx_dibit(nxdn_rx_t *rx, uint8_t d);
void nxdn_rx_dibits(nxdn_rx_t *rx, const uint8_t *d, size_t n);
void nxdn_rx_frame(nxdn_rx_t *rx, const uint8_t frame[NXDN_FRAME_DIBITS]);   /* aligned frame */

/* ---------------- transmitter ---------------- */
/* Conventional simplex / direct mode (TS 1-A 4.3.1.3, 4.4.3, 5.2.2 table 5.2-2 part 2, 6.3.2):
 * RF channel RDCH, an SU always sends direction 0 ("SU -> CR/SU"), a repeater output is direction 1.
 * A transmission starts with the preamble (>= 24 bits; a longer one repeats +3 +3 -3 -3 in front).
 * RAN: 0 = receiver unmutes on any RAN; 1..63 = talk group / access code; set rx cfg.ran_filter. */
/* preamble symbols: n_extra x (+3 +3 -3 -3) then the 12-symbol pattern 5775FD; returns symbols written */
unsigned nxdn_tx_preamble(unsigned n_extra4, uint8_t *out);
/* LICH for an SU voice frame on RDCH: superframe SACCH (true) or single SACCH, steal NXDN_STEAL_* */
uint8_t  nxdn_lich_rdch_voice(bool superframe, uint8_t steal);
uint8_t  nxdn_lich_rdch_data(bool facch2);

/* voice frame: sacch_info = SR + 18 bits (26 bits in 4 bytes), facch = NULL for none / one message for
 * both halves (steal), vch = 4 x 9 bytes (EHR) or 2 x 18 (EFR). Output 192 dibits incl. FSW, scrambled. */
void nxdn_tx_voice_frame(uint8_t lich7, const uint8_t sacch_info[4], const uint8_t *facch1_first,
                         const uint8_t *facch1_second, const uint8_t *vch, bool efr,
                         uint8_t out[NXDN_FRAME_DIBITS]);
/* Type-D RTCH2 voice frame: SCCH (25 bits: structure 2 + 23, in 4 bytes) in place of SACCH */
void nxdn_tx_typed_voice_frame(uint8_t lich7, const uint8_t scch_info[4], const uint8_t *facch1_first,
                               const uint8_t *facch1_second, const uint8_t *vch, uint8_t out[NXDN_FRAME_DIBITS]);
void nxdn_tx_udch_frame(uint8_t lich7, const uint8_t info[23], uint8_t out[NXDN_FRAME_DIBITS]);
void nxdn_tx_cac_frame(uint8_t lich7, const uint8_t info[19], uint8_t out[NXDN_FRAME_DIBITS]);      /* outbound */
void nxdn_tx_lcac_frame(uint8_t lich7, const uint8_t *info, bool short_cac, uint8_t out[NXDN_FRAME_DIBITS]);
/* SACCH superframe helper: quarter q (0..3) of a 72-bit message with given RAN */
void nxdn_sacch_quarter(const uint8_t msg9[9], unsigned q, uint8_t ran, bool superframe, uint8_t info4[4]);

/* ---------------- Type-C trunking, SU side (TS 1-A 2.2, TS 1-C) ---------------- */
typedef enum { NXT_OFF, NXT_HUNT, NXT_CAMPED, NXT_TRAFFIC } nxt_state_t;
typedef enum { NXT_EV_HUNT_TUNE, NXT_EV_CAMPED, NXT_EV_LOST_CC, NXT_EV_REGISTERED, NXT_EV_REG_FAILED,
               NXT_EV_GRANT, NXT_EV_RETURN, NXT_EV_REQ_FAILED } nxt_ev_type_t;
typedef struct { nxt_ev_type_t type; uint16_t channel; nxdn_site_info_t site; nxdn_call_t call; uint8_t cause; } nxt_event_t;

#define NXT_MAX_GROUPS 16
#define NXT_MAX_HUNT   16
typedef struct {
    uint16_t unit_id;
    uint16_t groups[NXT_MAX_GROUPS];
    uint8_t  n_groups;
    uint16_t hunt[NXT_MAX_HUNT];      /* control channel numbers */
    uint8_t  n_hunt;
    int      ran;                     /* -1 any; else expected RAN = site code mod 64 */
    bool     auto_register;
    uint16_t dwell_ms, hang_ms;
} nxt_cfg_t;
typedef struct {
    void (*tune)(void *user, uint16_t channel, bool control);
    void (*tx)(void *user, const uint8_t *info, unsigned nbytes, bool short_cac);   /* inbound CAC message */
    void (*event)(void *user, const nxt_event_t *ev);
    void *user;
} nxt_cb_t;
typedef struct {
    nxt_cfg_t cfg;
    nxt_cb_t cb;
    nxt_state_t state;
    uint8_t hunt_idx;
    uint16_t cc;
    uint32_t now, t0, last_cc, last_tr;
    nxdn_site_info_t site;
    bool site_valid, registered, req_pending;
    uint8_t req_tries;
    uint32_t req_due;
    uint8_t req_msg[16];
    uint8_t req_len;
    uint8_t req_type;
    nxdn_call_t call;
    bool voice_seen;
} nxt_t;
void nxt_init(nxt_t *t, const nxt_cfg_t *cfg, const nxt_cb_t *cb);
void nxt_start(nxt_t *t);
void nxt_on_rx(nxt_t *t, const nxdn_event_t *ev);
void nxt_tick(nxt_t *t, uint32_t now_ms);
bool nxt_request_group_call(nxt_t *t, uint16_t group);
bool nxt_register(nxt_t *t);

#endif
