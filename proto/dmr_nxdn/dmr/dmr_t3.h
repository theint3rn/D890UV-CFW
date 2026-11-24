/* DMR Tier III trunking, MS side (ETSI TS 102 361-4).
 *
 * Feed it the events of a dmr_rx_t (CSBK, MBC, SHORT_LC, VOICE_END, SYNC_LOST) and a millisecond
 * tick. It tells the platform when to retune (tune callback, logical physical channel number +
 * timeslot) and hands it inbound CSBKs to transmit (tx_csbk). Channel number -> frequency is the
 * platform's channel plan (or the absolute parameters reported in an MBC grant).
 *
 * Covered: TSCC hunt over a list of LPCNs, C_ALOHA / C_SYS_Parms / C_BCAST (Ann_WD_TSCC,
 * adjacent site), registration via C_RAND (REGI), random access with Mask / Service_Function /
 * NRand_Wait / Backoff, C_ACKD/NACKD/QACKD/WACKD, C_AHOY presence check -> C_ACKU, talkgroup and
 * private voice grants (TV/BTV/PV_GRANT, late entry), following onto the payload channel and back
 * (P_CLEAR, call end + hang timer, sync loss).
 * Proprietary trunking (Motorola Capacity/Connect Plus, Hytera XPT, ...) is out of scope; every CSBK
 * with a manufacturer FID is passed to the mfid_csbk hook so a later module can add it.
 */
#ifndef DMR_T3_H
#define DMR_T3_H
#include "dmr.h"
#include "dmr_priv.h"

/* CSBK opcodes (TS 102 361-4 7.1.1) */
enum {
    T3_C_ALOHA = 0x19, T3_C_UDTHD = 0x1A, T3_C_UDTHU = 0x1B, T3_C_AHOY = 0x1C, T3_C_ACKVIT = 0x1E,
    T3_C_RAND = 0x1F, T3_C_ACKD = 0x20, T3_C_ACKU = 0x21, T3_C_BCAST = 0x28, T3_P_MAINT = 0x2A,
    T3_P_CLEAR = 0x2E, T3_P_PROTECT = 0x2F, T3_PV_GRANT = 0x30, T3_TV_GRANT = 0x31, T3_BTV_GRANT = 0x32,
    T3_PD_GRANT = 0x33, T3_TD_GRANT = 0x34, T3_PV_GRANT_DX = 0x35, T3_PD_GRANT_DX = 0x36, T3_C_MOVE = 0x39,
};

/* Service_Kind (table 7.49) */
enum { T3_SK_IND_VOICE = 0, T3_SK_TG_VOICE = 1, T3_SK_IND_DATA = 2, T3_SK_TG_DATA = 3,
       T3_SK_REGISTRATION = 14, T3_SK_CANCEL = 15 };

/* gateway addresses (annex A.4) */
#define T3_ADDR_REGI  0xFFFEC6u
#define T3_ADDR_TSI   0xFFFECAu
#define T3_ADDR_ALLMSID 0xFFFFFFu
#define T3_LPCN_ABSOLUTE 0xFFFu

/* reason codes used here (tables 7.42 - 7.45) */
#define T3_REASON_MSG_ACCEPTED 0x60u
#define T3_REASON_REG_ACCEPTED 0x62u
#define T3_REASON_MS_ACCEPTED  0x44u
#define T3_REASON_REG_REFUSED  0x2Au
#define T3_REASON_REG_DENIED   0x2Bu
#define T3_REASON_AUTH_RESP    0x48u
#define T3_AUTH_CHALLENGE_MAX  0xFFFCDFu

typedef enum { T3_ACK_ACK, T3_ACK_NACK, T3_ACK_QUEUED, T3_ACK_WAIT, T3_ACK_MS } t3_ack_class_t;
t3_ack_class_t dmr_t3_ack_class(uint8_t reason);

/* System Identity Code (7.2.6, 16 bit: MODEL 2, NET+SITE 12, PAR 2) */
typedef struct { uint8_t model, par; uint16_t net, site; } t3_syscode_t;
void dmr_t3_syscode_parse(uint16_t syscode, t3_syscode_t *s);

/* parsed PDUs */
typedef struct {
    uint8_t tsccas, site_sync, version, offset, active_conn, mask, svc_fn, nrand_wait, reg, backoff;
    uint16_t syscode;
    uint32_t ms_addr;
} t3_aloha_t;
typedef struct { uint16_t lpcn; uint8_t lcn, late_entry, emergency, offset; uint32_t target, source; } t3_grant_t;
typedef struct { uint8_t type, reg, backoff; uint16_t parms1, syscode; uint32_t parms2; } t3_bcast_t;
typedef struct { uint8_t som, skf, als, gi, ab, svc_kind; uint32_t target, source; } t3_ahoy_t;
typedef struct { uint8_t resp_info, reason; uint32_t target, source; } t3_ack_t;

void dmr_t3_parse_aloha(const dmr_csbk_t *c, t3_aloha_t *a);
void dmr_t3_parse_grant(const dmr_csbk_t *c, t3_grant_t *g);
void dmr_t3_parse_bcast(const dmr_csbk_t *c, t3_bcast_t *b);
void dmr_t3_parse_ahoy(const dmr_csbk_t *c, t3_ahoy_t *a);
void dmr_t3_parse_ack(const dmr_csbk_t *c, t3_ack_t *a);

void dmr_t3_build_aloha(dmr_csbk_t *c, const t3_aloha_t *a);
void dmr_t3_build_grant(dmr_csbk_t *c, uint8_t opcode, const t3_grant_t *g);
void dmr_t3_build_bcast(dmr_csbk_t *c, const t3_bcast_t *b);
void dmr_t3_build_ack(dmr_csbk_t *c, uint8_t opcode, const t3_ack_t *a);
void dmr_t3_build_rand(dmr_csbk_t *c, uint8_t svc_opts, uint8_t proxy, uint8_t svc_kind, uint32_t target,
                       uint32_t source);
void dmr_t3_build_p_clear(dmr_csbk_t *c, uint16_t lpcn, uint8_t gi, uint32_t target, uint32_t source);

/* ---------------- MS state machine ---------------- */
typedef enum { T3_ST_OFF, T3_ST_HUNT, T3_ST_CAMPED, T3_ST_TRAFFIC } t3_state_t;

typedef enum {
    T3_EV_HUNT_TUNE,        /* lpcn/slot: now listening for a TSCC there */
    T3_EV_CAMPED,           /* found TSCC: syscode */
    T3_EV_LOST_CC,          /* TSCC lost, hunting again */
    T3_EV_REGISTERED, T3_EV_REG_REFUSED, T3_EV_REG_FAILED,
    T3_EV_REQUEST_ACK,      /* our service request answered: reason, ack class */
    T3_EV_REQUEST_FAILED,   /* no answer after retries */
    T3_EV_GRANT,            /* following a call: grant valid */
    T3_EV_RETURN,           /* back on the TSCC after a call */
    T3_EV_TSCC_LIST,        /* hunt list changed by Ann_WD_TSCC */
    T3_EV_ADJ_SITE,         /* bcast parsed */
    T3_EV_MFID_CSBK,        /* manufacturer CSBK (hook for proprietary trunking) */
    T3_EV_AUTH,             /* answered an authentication challenge (reason = 0 / 1 no key) */
} t3_ev_type_t;

typedef struct {
    t3_ev_type_t type;
    uint16_t lpcn;
    uint8_t  slot;
    uint8_t  opcode;
    uint8_t  reason;
    t3_ack_class_t ack;
    t3_grant_t grant;
    t3_syscode_t sys;
    uint16_t syscode;
    const dmr_csbk_t *csbk;
} t3_event_t;

#define T3_MAX_TG   16
#define T3_MAX_HUNT 16

typedef struct {
    uint32_t my_id;
    uint32_t tg[T3_MAX_TG];
    uint8_t  n_tg;
    uint16_t hunt[T3_MAX_HUNT];         /* TSCC logical physical channel numbers */
    uint8_t  n_hunt;
    uint8_t  tscc_slot;                 /* 0 or 1 (TSCC timeslot) */
    int      syscode_filter;            /* -1 any, else expected C_SYScode */
    bool     auto_register;
    uint16_t dwell_ms;                  /* hunt dwell per channel (default 600) */
    uint16_t hang_ms;                   /* traffic hang before returning (default 3000) */
    uint8_t  nrand;                     /* random access attempts (default 4) */
    /* authentication (6.4.8): answer C_AHOY challenges with RC4(challenge || K) [^ PSN] */
    bool     auth;
    uint8_t  auth_key[16];
    bool     use_psn;
    uint8_t  psn[3];
} dmr_t3_cfg_t;

typedef struct {
    void (*tune)(void *user, uint16_t lpcn, uint8_t slot, bool control);
    void (*tx_csbk)(void *user, const dmr_csbk_t *c, uint8_t slot);
    void (*event)(void *user, const t3_event_t *ev);
    void *user;
} dmr_t3_cb_t;

typedef struct {
    dmr_t3_cfg_t cfg;
    dmr_t3_cb_t cb;
    t3_state_t state;
    uint8_t  hunt_idx;
    uint16_t cc_lpcn;                   /* current TSCC */
    uint32_t now, state_t0, last_cc_rx, last_traffic_rx;
    t3_aloha_t aloha;
    bool     aloha_seen, registered, reg_pending;
    /* pending random access request */
    bool     ra_active, ra_sent;
    uint8_t  ra_tries;
    uint32_t ra_due, ra_timeout;
    dmr_csbk_t ra_csbk;
    uint8_t  ra_kind;
    uint32_t ra_target;
    /* traffic */
    t3_grant_t grant;
    uint8_t  grant_opcode;
    bool     voice_seen;
    uint32_t rng;
} dmr_t3_t;

void dmr_t3_init(dmr_t3_t *t, const dmr_t3_cfg_t *cfg, const dmr_t3_cb_t *cb);
void dmr_t3_start(dmr_t3_t *t);                      /* begin hunting */
void dmr_t3_on_rx(dmr_t3_t *t, const dmr_event_t *ev);
void dmr_t3_tick(dmr_t3_t *t, uint32_t now_ms);
/* PTT on a talkgroup (group = true) or individual: issues C_RAND; the grant moves us to traffic */
bool dmr_t3_request_voice(dmr_t3_t *t, uint32_t target, bool group);
bool dmr_t3_register(dmr_t3_t *t);
bool dmr_t3_tg_wanted(const dmr_t3_t *t, uint32_t tg);

#endif
