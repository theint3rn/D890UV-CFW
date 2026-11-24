/*
 * p25_trunk.h - P25 trunking follower (receive side) for an SU:
 * control-channel hunt, site/iden tracking, group grant following with
 * talkgroup priority / scan list, Phase 1 and Phase 2 voice channels.
 * TIA-102.AABD (trunking procedures), AABC (messages), BBAD (Phase 2 MAC).
 *
 * The state machine is passive: it consumes decoded events and a millisecond
 * tick and asks the platform to retune through a callback. Inbound requests
 * (registration, affiliation, voice request) are built with p25_tsbk.h and
 * sent by the application on the frequency from p25_trunk_cc_tx_hz().
 */
#ifndef P25_TRUNK_H
#define P25_TRUNK_H

#include <stdint.h>
#include <stdbool.h>
#include "p25_tsbk.h"
#include "p25_p1.h"
#include "p25_p2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P25_TRUNK_MAX_CC  8
#define P25_TRUNK_MAX_TG  32
#define P25_TRUNK_MAX_ADJ 8

typedef enum { P25_TS_IDLE = 0, P25_TS_HUNT, P25_TS_CC, P25_TS_VOICE } p25_trunk_state_t;
typedef enum { P25_MODE_P1 = 1, P25_MODE_P2 = 2 } p25_mode_t;

typedef struct {
    uint16_t tg;
    uint8_t  prio;              /* 1 = highest .. 255; 0 = never follow */
    bool     lockout;
} p25_tg_entry_t;

typedef struct {
    uint16_t sysid;
    uint8_t  rfss, site;
    uint16_t ch;
    uint8_t  cfva;
} p25_adj_site_t;

typedef enum {
    P25_TE_CC_FOUND,            /* rx_hz = control channel */
    P25_TE_CC_LOST,
    P25_TE_SITE,                /* wacn/sysid/rfss/site/nac updated */
    P25_TE_ADJ,                 /* adjacent site table updated (adj index in prio) */
    P25_TE_CALL_START,          /* following a grant: tg, src, rx_hz, slot, mode */
    P25_TE_CALL_PREEMPT,        /* left a call for a higher-priority one */
    P25_TE_CALL_END,            /* back to control channel */
    P25_TE_AFF_RSP,             /* group affiliation response (tg, src = result) */
    P25_TE_REG_RSP,             /* unit registration response (src = result) */
    P25_TE_DENY,                /* deny response (src = reason) */
} p25_trunk_ev_type_t;

typedef struct {
    p25_trunk_ev_type_t type;
    uint16_t tg;
    uint32_t src;
    uint32_t rx_hz, tx_hz;
    uint8_t  slot;
    p25_mode_t mode;
    uint8_t  prio;
} p25_trunk_event_t;

typedef struct {
    /* retune request: mode P1 = C4FM FDMA, P2 = TDMA (slot = logical channel) */
    void (*tune)(void *ctx, uint32_t rx_hz, uint32_t tx_hz, p25_mode_t mode, uint8_t slot);
    void (*event)(void *ctx, const p25_trunk_event_t *ev);
} p25_trunk_cb_t;

typedef struct {
    /* ---- configuration (set after init) ---- */
    uint32_t cc_list[P25_TRUNK_MAX_CC];
    uint8_t  n_cc;
    p25_tg_entry_t tgs[P25_TRUNK_MAX_TG];
    uint8_t  n_tg;
    bool     scan_all;          /* follow unlisted talkgroups at default_prio */
    uint8_t  default_prio;
    uint16_t cc_timeout_ms;     /* hunt dwell / CC loss (default 1500) */
    uint16_t grant_wait_ms;     /* no voice after tune -> back (default 1000) */
    uint16_t hang_ms;           /* no voice activity -> back (default 1500) */
    uint32_t wacn_filter;       /* 0 = any */
    uint16_t sysid_filter;      /* 0 = any */
    /* ---- learned site state ---- */
    p25_iden_t iden[16];
    uint32_t wacn;
    uint16_t sysid, nac;
    uint8_t  rfss, site;
    uint16_t cc_ch;             /* own control channel number (RFSS status) */
    p25_adj_site_t adj[P25_TRUNK_MAX_ADJ];
    uint8_t  n_adj;
    /* ---- runtime ---- */
    const p25_trunk_cb_t *cb;
    void    *ctx;
    p25_trunk_state_t state;
    uint8_t  cc_idx;
    uint32_t cc_hz;
    uint32_t t_state;           /* ms in this state */
    uint32_t t_quiet;           /* ms since last good CC message / voice activity */
    bool     voice_seen;
    uint16_t call_tg;
    uint32_t call_src;
    uint8_t  call_prio;
    uint32_t call_rx_hz;
    uint8_t  call_slot;
    p25_mode_t call_mode;
} p25_trunk_t;

void p25_trunk_init(p25_trunk_t *t, const p25_trunk_cb_t *cb, void *ctx);
bool p25_trunk_add_cc(p25_trunk_t *t, uint32_t hz);
bool p25_trunk_add_tg(p25_trunk_t *t, uint16_t tg, uint8_t prio, bool lockout);
/* 0 = do not follow, else priority (1 highest) */
uint8_t p25_trunk_tg_prio(const p25_trunk_t *t, uint16_t tg);

void p25_trunk_start(p25_trunk_t *t);          /* begin hunting cc_list */
void p25_trunk_stop(p25_trunk_t *t);
void p25_trunk_tick(p25_trunk_t *t, uint32_t ms);

/* inputs */
void p25_trunk_osp(p25_trunk_t *t, const p25_tsbk_t *m);       /* parsed OSP (TSBK or MAC) */
void p25_trunk_p1_event(p25_trunk_t *t, const p25_p1_event_t *ev);
void p25_trunk_p2_event(p25_trunk_t *t, const p25_p2_event_t *ev);
void p25_trunk_voice_activity(p25_trunk_t *t);                  /* from the voice callbacks */

/* SU inbound frequency of the current control channel (for ISP messages) */
bool p25_trunk_cc_tx_hz(const p25_trunk_t *t, uint32_t *tx_hz);

#ifdef __cplusplus
}
#endif
#endif
