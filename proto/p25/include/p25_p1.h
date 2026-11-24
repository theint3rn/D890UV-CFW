/*
 * p25_p1.h - P25 Phase 1 (FDMA, C4FM) framing: TIA-102.BAAA-B (CAI),
 * TIA-102.BABA (IMBE FEC/interleave), TIA-102.AABF (link control),
 * TIA-102.AABB/AABC (trunking TSBK formats).
 *
 * Data model
 *   "air dibits": one symbol 0..3 per byte, as transmitted, including the
 *   status symbol that follows every 35 data dibits (TIA-102.BAAA 6.3).
 *   "data dibits": the same stream with status symbols removed. All frame
 *   offsets below are data-bit offsets from the first frame-sync bit.
 *
 * Vocoder frames are opaque: IMBE = 88 bits packed MSB first in 11 bytes
 * (u0..u3 12 bits, u4..u6 11 bits, u7 7 bits). No codec internals here.
 */
#ifndef P25_P1_H
#define P25_P1_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "p25_crypt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P25_SYNC_P1        0x5575F5FF77FFULL  /* 48 bits, 24 dibits: 01=+3 11=-3 */
#define P25_NAC_ANY        0xFFFFu            /* rx filter: accept every NAC */
#define P25_NAC_DEFAULT    0x293u
#define P25_NAC_RX_ANY_TX  0xF7Eu             /* repeater: receive any */
#define P25_NAC_RPT_ALL    0xF7Fu             /* repeater: repeat all */

enum p25_duid {
    P25_DUID_HDU = 0x0, P25_DUID_TDU = 0x3, P25_DUID_LDU1 = 0x5, P25_DUID_TSBK = 0x7,
    P25_DUID_LDU2 = 0xA, P25_DUID_PDU = 0xC, P25_DUID_TDULC = 0xF
};

/* status symbols (TIA-102.BAAA 6.3) */
enum p25_status {
    P25_SS_UNKNOWN_TA = 0,   /* 00: unknown, talk-around / SU inbound */
    P25_SS_BUSY = 1,         /* 01: inbound channel busy */
    P25_SS_UNKNOWN_RPT = 2,  /* 10: unknown, repeater */
    P25_SS_IDLE = 3          /* 11: inbound channel idle */
};

/* air lengths (dibits incl. status + null pad) */
#define P25_HDU_AIR    396
#define P25_LDU_AIR    864
#define P25_TDU_AIR    72
#define P25_TDULC_AIR  216
#define P25_TSBK_AIR(n) ((n) == 1 ? 180 : (n) == 2 ? 288 : 360)
#define P25_P1_MAX_AIR 864

#define P25_IMBE_BYTES 11

/* ---------- payload structures ---------- */
typedef struct {
    uint8_t  mi[9];
    uint8_t  mfid;
    uint8_t  algid;          /* 0x80 = clear */
    uint16_t kid;
    uint16_t tgid;
} p25_hdu_t;

typedef struct {
    uint8_t  mi[9];
    uint8_t  algid;
    uint16_t kid;
} p25_es_t;                  /* LDU2 encryption sync */

/* Link control word, 72 bits raw: [0] P|SF|LCO(6), [1] MFID, [2..8] data */
typedef struct { uint8_t b[9]; } p25_lc_t;

enum p25_lco {
    P25_LCO_GRP_V_USER = 0x00, P25_LCO_GRP_V_UPDT = 0x02, P25_LCO_UU_V_USER = 0x03,
    P25_LCO_GRP_V_UPDT_EXP = 0x04, P25_LCO_CALL_TERM = 0x0F, P25_LCO_GRP_AFF_QUERY = 0x10,
    P25_LCO_SYS_SVC_BCST = 0x20, P25_LCO_ADJ_STS_BCST = 0x22, P25_LCO_RFSS_STS_BCST = 0x23,
    P25_LCO_NET_STS_BCST = 0x24
};

typedef struct {
    uint8_t  lco;            /* 6 bits */
    bool     protect;
    bool     sf;             /* implicit/explicit MFID format bit */
    uint8_t  mfid;
    uint8_t  svc_opts;       /* emergency 0x80, encrypted 0x40, ... */
    uint32_t tgid;           /* group (16 bit) or unit target (24 bit) */
    uint32_t src;            /* source unit id (24 bit) */
    bool     is_group;
    bool     known;          /* lco understood (fields valid) */
} p25_lc_info_t;

/* ---------- FEC pieces (exposed for tests and other layers) ---------- */
/* IMBE 88 info bits <-> 144 interleaved air bits (packed, 18 bytes, data
 * order without status symbols). Returns total corrected bits, or -1 if c0
 * (the most important Golay word) was uncorrectable. */
void p25_imbe_encode(const uint8_t frame[P25_IMBE_BYTES], uint8_t air[18]);
int  p25_imbe_decode(const uint8_t air[18], uint8_t frame[P25_IMBE_BYTES]);
/* data-bit offset (0..143) of code word bit j (c0..c7 concatenated) */
unsigned p25_imbe_il(unsigned j);

/* link control helpers */
void p25_lc_group_voice(p25_lc_t *lc, uint8_t svc, uint16_t tgid, uint32_t src);
void p25_lc_unit_voice(p25_lc_t *lc, uint8_t svc, uint32_t dst, uint32_t src);
void p25_lc_parse(const p25_lc_t *lc, p25_lc_info_t *out);

/* ---------- TX frame builders ---------- */
/* All return the number of air dibits written (status included). `status` is
 * the status symbol to insert (SU: P25_SS_UNKNOWN_TA, repeater busy: BUSY). */
size_t p25_p1_build_hdu(uint8_t *air, uint16_t nac, const p25_hdu_t *h, uint8_t status);
size_t p25_p1_build_ldu1(uint8_t *air, uint16_t nac, const uint8_t imbe[9][P25_IMBE_BYTES],
                         const p25_lc_t *lc, const uint8_t lsd[2], uint8_t status);
size_t p25_p1_build_ldu2(uint8_t *air, uint16_t nac, const uint8_t imbe[9][P25_IMBE_BYTES],
                         const p25_es_t *es, const uint8_t lsd[2], uint8_t status);
size_t p25_p1_build_tdu(uint8_t *air, uint16_t nac, uint8_t status);
size_t p25_p1_build_tdulc(uint8_t *air, uint16_t nac, const p25_lc_t *lc, uint8_t status);
/* TSBK frame with n (1..3) blocks of 10 bytes (opcode byte .. last arg byte);
 * last-block flag and CRC are filled in. Needs P25_TSBK_AIR(n) dibits. */
size_t p25_p1_build_tsbk(uint8_t *air, uint16_t nac, const uint8_t blocks[][10], unsigned n, uint8_t status);

/* ---------- PDU (TIA-102.BAAA 5.4, basics) ---------- */
enum { P25_PDU_FMT_RESPONSE = 0x03, P25_PDU_FMT_UNCONF = 0x15, P25_PDU_FMT_CONF = 0x16,
       P25_PDU_FMT_AMBT = 0x17 };
typedef struct {
    bool     confirmed;      /* A/N: confirmation required */
    bool     outbound;       /* I/O */
    uint8_t  format;         /* 5 bits */
    uint8_t  sap;            /* 6 bits */
    uint8_t  mfid;
    uint32_t llid;           /* 24 bits */
    bool     fmf;            /* full message flag */
    uint8_t  blocks;         /* blocks to follow (7 bits) */
    uint8_t  pad;            /* pad octets (5 bits) */
    uint8_t  ns;             /* confirmed: Syn|N(S)|FSNF byte */
    uint8_t  offset;         /* data header offset */
} p25_pdu_hdr_t;
void p25_pdu_hdr_pack(const p25_pdu_hdr_t *h, uint8_t out[12]);       /* adds CRC-16 */
bool p25_pdu_hdr_unpack(const uint8_t in[12], p25_pdu_hdr_t *h);      /* false on CRC error */
/* Build a PDU frame carrying `len` user octets. Returns air dibits written
 * (size it with p25_p1_pdu_air(len, confirmed)), 0 if too long. */
size_t p25_p1_pdu_air(size_t len, bool confirmed);
size_t p25_p1_build_pdu(uint8_t *air, uint16_t nac, const p25_pdu_hdr_t *tmpl, const uint8_t *data,
                        size_t len, uint8_t status);

/* ---------- RX ---------- */
typedef enum {
    P25_P1_EV_SYNC,        /* frame sync found (nac/duid valid after NID) */
    P25_P1_EV_NID_FAIL,
    P25_P1_EV_HDU,
    P25_P1_EV_LDU1,        /* LC decoded (lc_ok) */
    P25_P1_EV_LDU2,        /* ES decoded (es_ok) */
    P25_P1_EV_TDU,
    P25_P1_EV_TDULC,
    P25_P1_EV_TSBK,        /* one block: tsbk[12] incl. CRC, crc_ok */
    P25_P1_EV_PDU,         /* complete PDU: pdu_hdr + pdu_data/pdu_len */
} p25_p1_ev_type_t;

#define P25_PDU_MAX 256    /* RX reassembly limit (user octets) */

/* voice decryption state reported with LDU events (p25_p1_rx_set_crypt) */
enum p25_voice_crypt {
    P25_VC_CLEAR = 0,      /* clear voice (or not known to be encrypted yet) */
    P25_VC_DECRYPTED,      /* encrypted, key found: frames delivered decrypted */
    P25_VC_NOKEY,          /* encrypted, no key / unsupported ALGID: frames muted */
    P25_VC_WAIT_MI,        /* late entry into an encrypted call: muted until LDU2 ES */
};

typedef struct {
    p25_p1_ev_type_t type;
    uint16_t nac;
    uint8_t  duid;
    int      errs;          /* corrected bits/symbols in the frame (approx.) */
    int16_t  rssi;          /* mean RSSI over the frame (caller's units) */
    uint8_t  status;        /* last status symbol seen in the frame */
    bool     inverted;      /* sync found with inverted polarity */
    uint8_t  crypt;         /* enum p25_voice_crypt for this frame's voice */
    union {
        p25_hdu_t hdu;
        struct { p25_lc_t lc; bool lc_ok; uint8_t lsd[2]; } ldu1;
        struct { p25_es_t es; bool es_ok; uint8_t lsd[2]; } ldu2;
        struct { p25_lc_t lc; bool lc_ok; } tdulc;
        struct { uint8_t tsbk[12]; bool crc_ok; uint8_t index; } tsbk;
        struct { p25_pdu_hdr_t hdr; const uint8_t *data; size_t len; bool crc_ok; } pdu;
    } u;
} p25_p1_event_t;

typedef struct {
    void (*on_voice)(void *ctx, const uint8_t imbe[P25_IMBE_BYTES], int errs);
    void (*on_event)(void *ctx, const p25_p1_event_t *ev);
} p25_p1_cb_t;

typedef struct {
    /* config */
    uint16_t nac_filter;          /* P25_NAC_ANY or a NAC */
    uint8_t  sync_max_err;        /* default 4 */
    const p25_p1_cb_t *cb;
    void    *ctx;
    /* state */
    uint64_t sreg;                /* last 24 dibits as 48 bits */
    uint8_t  state;
    bool     inverted;
    uint16_t air_idx;             /* air dibit index in frame */
    uint16_t d_idx;               /* data dibit index in frame */
    uint16_t nac;
    uint8_t  duid;
    uint8_t  status;
    uint8_t  next_voice;          /* LDU voice frame to decode next */
    uint8_t  blk;                 /* TSBK/PDU block counter */
    uint8_t  pdu_blocks;
    uint16_t pdu_len;
    p25_pdu_hdr_t pdu_hdr;
    int      errs;
    int32_t  rssi_sum;
    uint16_t rssi_n;
    uint8_t  d[P25_LDU_AIR];      /* data dibits of the current frame */
    uint8_t  pdu[P25_PDU_MAX + 16];
    /* optional decryption (user keys) */
    const p25_keyring_t *keys;
    p25_crypt_t *crypt;
    uint8_t  vc;                  /* enum p25_voice_crypt */
    uint8_t  enc_algid;
    uint16_t enc_kid;
} p25_p1_rx_t;

void p25_p1_rx_init(p25_p1_rx_t *rx, uint16_t nac_filter, const p25_p1_cb_t *cb, void *ctx);
void p25_p1_rx_reset(p25_p1_rx_t *rx);   /* back to sync hunt */
void p25_p1_rx_push(p25_p1_rx_t *rx, const uint8_t *dibits, size_t n, int16_t rssi);
/* Enable decryption of encrypted calls with the user's key table. The MI comes
 * from the HDU (first superframe) and each LDU2's ES (next superframe).
 * Frames that cannot be decrypted are not passed to on_voice. */
void p25_p1_rx_set_crypt(p25_p1_rx_t *rx, const p25_keyring_t *keys, p25_crypt_t *crypt);

/* ---------- TX voice session helper ---------- */
typedef struct {
    uint16_t nac;
    uint8_t  status;
    uint8_t  n;                    /* IMBE frames buffered */
    bool     ldu2_next;
    p25_lc_t lc;
    p25_es_t es;
    uint8_t  lsd[4];
    uint8_t  imbe[9][P25_IMBE_BYTES];
    p25_crypt_t *crypt;            /* NULL = clear */
    const p25_keyring_t *keys;
    uint8_t  mi[9];                /* MI of the current superframe */
} p25_p1_voice_tx_t;

/* Starts a call: writes the HDU (P25_HDU_AIR dibits). Clear voice only:
 * algid 0x80, MI 0. */
size_t p25_p1_voice_tx_start(p25_p1_voice_tx_t *tx, uint8_t *air, uint16_t nac, const p25_lc_t *lc,
                             uint8_t status);
/* Encrypted call (only where the user is authorised; not on amateur bands):
 * like _start, with ALGID/KID from the key table and the first MI (should be
 * random/non-repeating). Sets the LC "encrypted" service option. Returns 0 if
 * the key is missing. `crypt` must stay valid during the call. */
size_t p25_p1_voice_tx_start_enc(p25_p1_voice_tx_t *tx, uint8_t *air, uint16_t nac, const p25_lc_t *lc,
                                 uint8_t status, p25_crypt_t *crypt, const p25_keyring_t *keys,
                                 uint8_t algid, uint16_t kid, const uint8_t mi[9]);
/* Queues one IMBE frame; every 9th call writes an LDU (P25_LDU_AIR dibits)
 * and returns its length, else 0. */
size_t p25_p1_voice_tx_push(p25_p1_voice_tx_t *tx, uint8_t *air, const uint8_t imbe[P25_IMBE_BYTES]);
/* Ends the call: pads the open LDU with `silence` frames (if non-NULL and
 * frames are pending) then writes a TDU (SU call end). Returns dibits;
 * air must hold P25_LDU_AIR + P25_TDU_AIR. */
size_t p25_p1_voice_tx_end(p25_p1_voice_tx_t *tx, uint8_t *air, const uint8_t silence[P25_IMBE_BYTES]);

/* helpers */
size_t p25_p1_insert_status(const uint8_t *data, size_t n_data, uint8_t *air, uint8_t status);

#ifdef __cplusplus
}
#endif
#endif
