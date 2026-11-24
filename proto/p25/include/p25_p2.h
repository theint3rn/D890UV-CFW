/*
 * p25_p2.h - P25 Phase 2 two-slot TDMA: TIA-102.BBAB (physical layer),
 * TIA-102.BBAC (MAC layer: ISCH, DUID, bursts, scrambling, FEC),
 * TIA-102.BBAD (MAC messages), TIA-102.BABA-1 (AMBE+2 half-rate FEC).
 *
 * Slot model (outbound, 12 kbit/s = 6000 sym/s): a superframe is 360 ms =
 * 12 timeslots of 180 dibits. In this library a slot buffer is
 *   [0..19]    ISCH (I-ISCH or S-ISCH, 40 bits)
 *   [20..179]  burst (160 dibits); DUID dibits at 20, 57, 142, 179
 * (the alignment OP25 uses). Logical channel of slot i: i&1, except slots 10
 * and 11 (the SACCH slots) which are swapped. Per logical channel and
 * superframe: 4V 4V 4V 4V 2V + SACCH. Each voice frame is 20 ms.
 *
 * Voice frames are opaque AMBE+2 half-rate (2450 bit/s) information: 49 bits,
 * packed MSB first in 7 bytes as u0(12) u1(12) u2(11) u3(14). Only the FEC
 * (Golay(24,12) on u0, PN-masked Golay(23,12) on u1) and interleave are here.
 *
 * TX builds outbound-format slots (for testing, simplex/repeater use). The
 * inbound H-CPM burst framing (ramp/guard) belongs to the modem layer.
 */
#ifndef P25_P2_H
#define P25_P2_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "p25_tsbk.h"
#include "p25_crypt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P25_P2_SLOT        180
#define P25_P2_SLOTS       12
#define P25_P2_SYNC        0x575D57F7FFULL      /* S-ISCH, 40 bits */
#define P25_AMBE_BYTES     7

/* DUID burst types (4-bit DUID value) */
enum p25_p2_burst {
    P25_P2_4V = 0, P25_P2_SACCH_S = 3, P25_P2_LCCH_S = 4, P25_P2_2V = 6, P25_P2_FACCH_S = 9,
    P25_P2_SACCH = 12, P25_P2_LCCH = 13, P25_P2_FACCH = 15
};

/* MAC PDU opcodes (3 bits) */
enum p25_mac_op {
    P25_MAC_SIGNAL = 0, P25_MAC_PTT = 1, P25_MAC_END_PTT = 2, P25_MAC_IDLE = 3, P25_MAC_ACTIVE = 4,
    P25_MAC_HANGTIME = 6
};

#define P25_FACCH_BYTES 18      /* 144-bit MAC PDU (+CRC-12 = 156 bits = 26 hexbits) */
#define P25_SACCH_BYTES 21      /* 168-bit MAC PDU (+CRC-12 = 180 bits = 30 hexbits) */

/* ---------- ISCH / DUID ---------- */
/* I-ISCH info: chn(2) loc(2) fr(1) cnt(2) -> 7-bit value -> 40-bit code word */
uint64_t p25_p2_isch_encode(unsigned value7);
static inline unsigned p25_p2_isch_value(unsigned chn, unsigned loc, unsigned fr, unsigned cnt)
{
    return ((chn & 3u) << 5) | ((loc & 3u) << 3) | ((fr & 1u) << 2) | (cnt & 3u);
}
/* returns 0..127 (I-ISCH value), -2 for S-ISCH (sync), -1 if no code word
 * within distance 7. *dist = Hamming distance to the chosen word. */
int      p25_p2_isch_decode(uint64_t cw40, int *dist);
uint8_t  p25_p2_duid_encode(unsigned duid4);           /* (8,4) code */
int      p25_p2_duid_decode(uint8_t cw8);              /* -1 if > 1 bit error */

/* ---------- scrambling (TIA-102.BBAC 7.2.5) ---------- */
typedef struct {
    uint8_t bits[540];          /* 4320-bit sequence, MSB first */
    bool    valid;
} p25_p2_scrambler_t;
void p25_p2_scrambler_init(p25_p2_scrambler_t *s, uint32_t wacn, uint16_t sysid, uint16_t nac);
/* XOR mask dibit for slot (0..11) and slot dibit position (20..179) */
static inline uint8_t p25_p2_scramble_dibit(const p25_p2_scrambler_t *s, unsigned slot, unsigned pos)
{
    unsigned k = 2 * (slot * P25_P2_SLOT + pos - 10);
    return (uint8_t)((((s->bits[k >> 3] >> (7 - (k & 7))) & 1u) << 1) |
                     ((s->bits[(k + 1) >> 3] >> (7 - ((k + 1) & 7))) & 1u));
}

/* ---------- AMBE+2 half-rate voice frame FEC ---------- */
void p25_ambe_encode(const uint8_t frame[P25_AMBE_BYTES], uint8_t dibits[36]);
int  p25_ambe_decode(const uint8_t dibits[36], uint8_t frame[P25_AMBE_BYTES]); /* bits corrected, -1 bad u0 */

/* ---------- ESS (encryption sync: ALGID 8, KID 16, MI 72 = 96 bits) ---------- */
typedef struct { uint8_t algid; uint16_t kid; uint8_t mi[9]; } p25_p2_ess_t;
/* RS(44,16,29): 16 ESS-B hexbits (4 per 4V burst) + 28 ESS-A hexbits (2V) */
void p25_p2_ess_encode(const p25_p2_ess_t *e, uint8_t essb[16], uint8_t essa[28]);
int  p25_p2_ess_decode(const uint8_t essb[16], const uint8_t essa[28], p25_p2_ess_t *e);

/* ---------- MAC PDU ---------- */
typedef struct {
    uint8_t  opcode;            /* enum p25_mac_op */
    uint8_t  offset;            /* 3 bits */
    bool     sacch;             /* 21-byte PDU (else 18) */
    uint8_t  pdu[P25_SACCH_BYTES];
    /* MAC_PTT / MAC_END_PTT fields */
    uint8_t  mi[9];
    uint8_t  algid;
    uint16_t kid;
    uint32_t src;
    uint16_t grp;
    uint16_t colour;            /* END_PTT colour code (12 bits) */
} p25_mac_pdu_t;

/* iterate the MAC messages of an IDLE/ACTIVE/HANGTIME/SIGNAL PDU:
 * returns the number found; each message is translated into a p25_tsbk_t
 * when it maps to an outbound TSBK layout (0x40|op abbreviated forms). */
typedef void (*p25_mac_msg_cb)(void *ctx, uint8_t mco, const uint8_t *msg, unsigned len, const p25_tsbk_t *osp);
int  p25_mac_messages(const p25_mac_pdu_t *m, p25_mac_msg_cb cb, void *ctx);
/* length of a MAC message from its opcode (0 = unknown) */
unsigned p25_mac_msg_len(const uint8_t *msg);

void p25_mac_ptt(p25_mac_pdu_t *m, bool sacch, uint32_t src, uint16_t grp);    /* clear call */
void p25_mac_end_ptt(p25_mac_pdu_t *m, bool sacch, uint16_t colour, uint32_t src, uint16_t grp);
void p25_mac_idle(p25_mac_pdu_t *m, bool sacch);

/* ---------- slot builders (TX) ---------- */
/* All write P25_P2_SLOT dibits. `slot` = superframe slot index (0..11) for
 * ISCH and scrambling; scr NULL = no scrambling. isch_value < 0 = S-ISCH. */
void p25_p2_build_4v(uint8_t out[P25_P2_SLOT], unsigned slot, int isch_value, const p25_p2_scrambler_t *scr,
                     const uint8_t frames[4][P25_AMBE_BYTES], const uint8_t essb4[4]);
void p25_p2_build_2v(uint8_t out[P25_P2_SLOT], unsigned slot, int isch_value, const p25_p2_scrambler_t *scr,
                     const uint8_t frames[2][P25_AMBE_BYTES], const uint8_t essa[28]);
void p25_p2_build_mac(uint8_t out[P25_P2_SLOT], unsigned slot, int isch_value, const p25_p2_scrambler_t *scr,
                      const p25_mac_pdu_t *m);
/* standard ISCH for a superframe slot: S-ISCH in slots 2,3,6,7,10,11, else
 * I-ISCH with loc = slot/4, chn = slot%4, cnt = ultraframe position */
int  p25_p2_isch_for_slot(unsigned slot, unsigned ultraframe_cnt);
static inline unsigned p25_p2_slot_channel(unsigned slot) { return slot >= 10 ? (slot ^ 1u) & 1u : slot & 1u; }

/* ---------- RX ---------- */
typedef enum {
    P25_P2_EV_SYNC,             /* superframe position known */
    P25_P2_EV_SYNC_LOST,
    P25_P2_EV_MAC,              /* mac valid (CRC ok) */
    P25_P2_EV_MAC_BAD,          /* FACCH/SACCH failed RS/CRC */
    P25_P2_EV_ESS,              /* ess valid */
} p25_p2_ev_type_t;

typedef struct {
    p25_p2_ev_type_t type;
    uint8_t  slot;              /* superframe slot index */
    uint8_t  channel;           /* logical channel 0/1 */
    uint8_t  burst;             /* DUID burst type */
    int      errs;
    int16_t  rssi;
    p25_mac_pdu_t mac;
    p25_p2_ess_t  ess;
} p25_p2_event_t;

typedef struct {
    void (*on_voice)(void *ctx, unsigned channel, const uint8_t frame[P25_AMBE_BYTES], int errs);
    void (*on_event)(void *ctx, const p25_p2_event_t *ev);
} p25_p2_cb_t;

typedef struct {
    const p25_p2_cb_t *cb;
    void   *ctx;
    int8_t  channel_filter;     /* -1 = both logical channels, else 0/1 */
    const p25_p2_scrambler_t *scr;  /* set when WACN/SysID/NAC known */
    /* state */
    uint64_t sreg;
    bool    in_sync;
    int8_t  slot_id;            /* -1 until an I-ISCH told us */
    uint8_t miss;               /* consecutive bad ISCH */
    uint16_t pos;               /* dibit position inside the slot */
    int32_t rssi_sum;
    uint16_t rssi_n;
    uint8_t buf[P25_P2_SLOT];
    uint8_t essb[2][16];
    uint8_t essb_n[2];
    /* optional decryption of the followed logical channel (channel_filter) */
    const p25_keyring_t *keys;
    p25_crypt_t *crypt;
    uint8_t vc;                 /* enum p25_voice_crypt (p25_p1.h values) */
} p25_p2_rx_t;

void p25_p2_rx_init(p25_p2_rx_t *rx, const p25_p2_cb_t *cb, void *ctx);
void p25_p2_rx_set_scrambler(p25_p2_rx_t *rx, const p25_p2_scrambler_t *scr);
/* Decrypt the channel selected by channel_filter (must be 0 or 1) with the
 * user's keys: MI from MAC_PTT (first superframe) and from each ESS (next
 * superframe). Undecryptable frames are muted. */
void p25_p2_rx_set_crypt(p25_p2_rx_t *rx, const p25_keyring_t *keys, p25_crypt_t *crypt);
void p25_p2_rx_push(p25_p2_rx_t *rx, const uint8_t *dibits, size_t n, int16_t rssi);
/* decode one complete slot (for callers that frame slots themselves) */
void p25_p2_rx_slot(p25_p2_rx_t *rx, const uint8_t slot_dibits[P25_P2_SLOT], unsigned slot);

#ifdef __cplusplus
}
#endif
#endif
