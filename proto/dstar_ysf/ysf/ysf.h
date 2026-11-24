/*
 * ysf.h - Yaesu System Fusion (C4FM) protocol layer, written from Yaesu
 * "Amateur Radio Digital Standards" rev 1.02 (2015-07-29).
 *
 * Frame: 960 bits / 480 dibits / 100 ms = FS (40) + FICH (200) + payload (720).
 * Covers: frame sync, FICH (CRC16, Golay(24,12,8), K=5 trellis, 5x20 dibit
 * interleave), DCH units (PN9 whitening, CRC16, trellis, 9x20 / 5x20 dibit
 * interleave), V/D mode 1, V/D mode 2 (49-bit voice with x3 repetition,
 * whitening, 26x4 interleave), Voice FR (144-bit full-rate frames, CC0 sub
 * header), Data FR (DCH-1/DCH-2, blocks), CSD1/2/3 callsign data, DG-ID.
 *
 * Vocoder frames are opaque: V/D 1 = 72 bits (AMBE+2 3600 incl. its FEC),
 * V/D 2 = 49 bits (AMBE+2 2450 payload), Voice FR = 144 bits (full rate
 * 7200 incl. FEC; any VW-specific bit order is the vocoder adapter's job).
 *
 * Dibits: one per byte, value 0..3, first air bit in bit 1.
 */
#ifndef YSF_H
#define YSF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YSF_FRAME_BITS      960u
#define YSF_FRAME_DIBITS    480u
#define YSF_SYNC_DIBITS     20u
#define YSF_FICH_DIBITS     100u
#define YSF_SYNC            0xD471C9634DULL /* 40 bits */

/* ---- FICH ------------------------------------------------------------------ */

enum { YSF_FI_HEADER = 0, YSF_FI_COMM = 1, YSF_FI_TERM = 2, YSF_FI_TEST = 3 };
enum { YSF_DT_VD1 = 0, YSF_DT_DATA_FR = 1, YSF_DT_VD2 = 2, YSF_DT_VOICE_FR = 3 };
enum { YSF_CM_GROUP = 0, YSF_CM_RADIO_ID = 1, YSF_CM_INDIVIDUAL = 3 };
enum { YSF_MR_DIRECT = 0, YSF_MR_DOWN_UP_FREE = 1, YSF_MR_DOWN_UP_BUSY = 2 };

/* 32 information bits, MSB first: FI(2) CS(2) CM(2) BN(2) BT(2) FN(3) FT(3)
 * rsv(1) Dev(1) MR(3) VoIP(1) DT(2) SQL(1) SC(7). */
typedef struct {
    uint8_t fi, cs, cm, bn, bt, fn, ft, rsv, dev, mr, voip, dt, sql, sc;
} ysf_fich_t;

uint32_t ysf_fich_pack(const ysf_fich_t *f);
void ysf_fich_unpack(uint32_t v, ysf_fich_t *f);
/* DG-ID: current radios carry the 0..99 Digital Group ID in the 7-bit SC
 * field (spec 1.02 calls it squelch code; same bits). */
static inline uint8_t ysf_fich_dgid(const ysf_fich_t *f) { return f->sc; }
static inline void ysf_fich_set_dgid(ysf_fich_t *f, uint8_t dgid) { f->sc = (uint8_t)(dgid & 0x7Fu); }

/* 200 air bits <-> FICH. decode returns true when the CRC matches; *errs
 * (may be NULL) gets the corrected trellis + Golay bit errors. */
void ysf_fich_encode(const ysf_fich_t *f, uint8_t bits[200]);
bool ysf_fich_decode(const uint8_t bits[200], ysf_fich_t *f, int *errs);

/* ---- FEC building blocks ----------------------------------------------------- */

uint32_t ysf_golay24_encode(uint16_t data12);
/* Returns corrected 12 data bits, or -1 if more than 3 errors; *nerr gets the
 * number of corrected bits. */
int ysf_golay24_decode(uint32_t word24, int *nerr);
/* Dibit block interleave on bit arrays: code dibits written in rows of m,
 * nrows rows, read by column (air dibit k <- code dibit (k%nrows)*m + k/nrows).
 * FICH / 200-bit DCH: m=5, nrows=20; 360-bit DCH: m=9, nrows=20.
 * inverse != 0 de-interleaves. */
void ysf_interleave_dibits(const uint8_t *in, uint8_t *out, unsigned m, unsigned nrows, int inverse);
/* XOR n bits with the PN(9,5) whitening sequence (register 1 1100 1001). */
void ysf_whiten(uint8_t *bits, size_t n);

/* DCH unit: nbytes = 20 (360 air bits) or 10 (200 air bits, V/D 2). */
void ysf_dch_encode(const uint8_t *data, unsigned nbytes, uint8_t *bits);
bool ysf_dch_decode(const uint8_t *bits, unsigned nbytes, uint8_t *data, int *errs);

/* V/D mode 2 voice: 49 bits (7 bytes, MSB first, last 7 bits unused) <->
 * 104 air bits (72 VCH + 32 VeCH). decode returns repetition votes that
 * were not unanimous (a quality hint). */
void ysf_vd2_voice_encode(const uint8_t voice[7], uint8_t bits[104]);
int ysf_vd2_voice_decode(const uint8_t bits[104], uint8_t voice[7]);

/* ---- callsign data ------------------------------------------------------------ */

typedef struct {
    char dest[10], src[10], down[10], up[10]; /* space padded */
    uint8_t rem[4][5];                       /* Rem1..4 (IDs) */
    uint8_t valid;                           /* bit0 dest 1 src 2 down 3 up 4 rem12 5 rem34 */
} ysf_csd_t;

#define YSF_CSD_ALL 0x3Fu

/* Fill a field with s, space padded. */
void ysf_field_set(char *field, size_t n, const char *s);
/* Rem3/Rem4 helpers for Wires-X / radio IDs: 5 ASCII digits or raw bytes. */
void ysf_csd_init(ysf_csd_t *c, const char *src, const char *dest);
/* Pack CSD1 (dest+src), CSD2 (down+up), CSD3 (rem1..4), 20 bytes each. */
void ysf_csd1_pack(const ysf_csd_t *c, uint8_t out[20]);
void ysf_csd2_pack(const ysf_csd_t *c, uint8_t out[20]);
void ysf_csd3_pack(const ysf_csd_t *c, uint8_t out[20]);

/* ---- frame layout ----------------------------------------------------------- */

/* Convert 960 bits <-> 480 dibits. */
void ysf_bits_to_dibits(const uint8_t *bits, size_t nbits, uint8_t *dibits);
void ysf_dibits_to_bits(const uint8_t *dibits, size_t ndibits, uint8_t *bits);
/* C4FM mapping: dibit -> symbol level (+1,+3,-1,-3) and back (nearest). */
int ysf_dibit_to_level(uint8_t dibit);
uint8_t ysf_level_to_dibit(float level);

/* ---- receiver ---------------------------------------------------------------- */

typedef enum {
    YSF_VOICE_VD1 = 1, /* 72 bits, 9 bytes */
    YSF_VOICE_VD2 = 2, /* 49 bits, 7 bytes */
    YSF_VOICE_FR = 3,  /* 144 bits, 18 bytes */
} ysf_voice_kind_t;

typedef enum {
    YSF_EV_HEADER = 1, /* HC: CSD1 + CSD2 in .csd */
    YSF_EV_TERMINATOR,
    YSF_EV_FICH,       /* every frame (also CC) */
    YSF_EV_CSD,        /* callsign data updated (.csd, .csd.valid) */
    YSF_EV_DCH,        /* raw DCH unit (.dch) */
    YSF_EV_DATA,       /* DT payload complete (V/D DT1.. at FN==FT, or Data FR block) */
    YSF_EV_LOST,
} ysf_event_type_t;

typedef struct {
    ysf_event_type_t type;
    ysf_fich_t fich;
    bool fich_ok;
    int fich_errs;
    ysf_csd_t csd;
    struct {
        const uint8_t *data;
        unsigned len;
        unsigned unit; /* DCH: 0/1 = DCH-1/DCH-2 (HC/TC, Data FR), else 0 */
        bool crc_ok;
    } dch;
} ysf_event_t;

typedef struct {
    /* idx 0..4 = 20 ms slot in the frame. */
    void (*voice)(void *user, ysf_voice_kind_t kind, const uint8_t *frame, unsigned idx, const ysf_fich_t *f);
    void (*event)(void *user, const ysf_event_t *ev);
} ysf_rx_cb_t;

typedef enum { YSF_RX_SEARCH = 0, YSF_RX_FRAME, YSF_RX_SYNC } ysf_rx_state_t;

#define YSF_DATA_MAX 260u

typedef struct {
    const ysf_rx_cb_t *cb;
    void *user;
    ysf_rx_state_t state;
    uint64_t sr;
    uint16_t n;            /* dibits collected */
    uint8_t buf[YSF_FRAME_DIBITS - YSF_SYNC_DIBITS];
    uint8_t sync_buf[YSF_SYNC_DIBITS];
    uint8_t miss;          /* consecutive bad syncs */
    uint8_t max_sync_err;  /* search tolerance (bits of 40), default 3 */
    uint8_t max_flywheel_err; /* default 8 */
    uint8_t max_miss;      /* default 4 (spec N2) */
    ysf_fich_t last;
    bool have_last;
    ysf_csd_t csd;
    uint8_t data[YSF_DATA_MAX];
    unsigned data_len;
} ysf_rx_t;

void ysf_rx_init(ysf_rx_t *rx, const ysf_rx_cb_t *cb, void *user);
void ysf_rx_reset(ysf_rx_t *rx);
void ysf_rx_push_dibit(ysf_rx_t *rx, uint8_t dibit);
void ysf_rx_push_dibits(ysf_rx_t *rx, const uint8_t *dibits, size_t n);
/* Decode one complete 480-dibit frame that starts with the sync. */
void ysf_rx_frame(ysf_rx_t *rx, const uint8_t dibits[YSF_FRAME_DIBITS]);

/* ---- transmitter --------------------------------------------------------------- */

typedef struct {
    uint8_t dt;            /* YSF_DT_* */
    ysf_fich_t base;       /* cm, dev, mr, voip, sql, sc(DG-ID) taken from here */
    ysf_csd_t csd;
    uint8_t text[100];     /* V/D DT payload (VD1 <= 100 B, VD2 <= 20 B) */
    unsigned text_len;
    uint8_t fn, ft;
    uint32_t cc_count;     /* CC frames sent */
} ysf_tx_t;

void ysf_tx_init(ysf_tx_t *tx, uint8_t dt, const ysf_csd_t *csd, uint8_t dgid);
/* Set V/D mode DT payload (rolling with the CSDs, FT follows Table 4-3/4-4). */
void ysf_tx_set_data(ysf_tx_t *tx, const uint8_t *data, unsigned len);
/* Header / terminator frame (FI 0 / 2), DCH-1 = CSD1, DCH-2 = CSD2. */
void ysf_tx_header(ysf_tx_t *tx, uint8_t dibits[YSF_FRAME_DIBITS]);
void ysf_tx_terminator(ysf_tx_t *tx, uint8_t dibits[YSF_FRAME_DIBITS]);
/* One CC voice frame with 5 vocoder frames (VD1: 5x9 B, VD2: 5x7 B, VW: 5x18 B,
 * consecutive). In Voice FR the first CC frame is the CC0 sub header which
 * only has room for frames 3 and 4. */
void ysf_tx_voice(ysf_tx_t *tx, const uint8_t *frames, uint8_t dibits[YSF_FRAME_DIBITS]);
/* Data FR: number of CC frames needed for len bytes (<= 4 blocks x 260 B). */
unsigned ysf_tx_data_fr_frames(unsigned len);
/* Build CC frame k (0-based) of a Data FR transfer. */
void ysf_tx_data_fr(ysf_tx_t *tx, const uint8_t *data, unsigned len, unsigned k,
                    uint8_t dibits[YSF_FRAME_DIBITS]);

/* Low level: assemble a frame from FICH + 720 payload bits. */
void ysf_build_frame(const ysf_fich_t *f, const uint8_t payload[720], uint8_t dibits[YSF_FRAME_DIBITS]);

#ifdef __cplusplus
}
#endif
#endif
