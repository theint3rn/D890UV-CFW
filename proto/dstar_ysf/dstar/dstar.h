/*
 * dstar.h - D-STAR DV protocol layer (JARL "Digitalization Technology
 * Standard for Amateur Radio", English ed. STD6.0, and the original "D-STAR
 * System" shogen).
 *
 * Covers: header (41 bytes, CRC-CCITT/X.25, rate-1/2 K=3 convolutional code,
 * 24-row interleave, x^7+x^4+1 scrambler), frame sync, voice frames (72-bit
 * AMBE, opaque), slow data (text message, GPS/D-PRS simple data, header
 * resend, code squelch), sync every 21 frames, end pattern, DV fast data,
 * callsign routing helpers. The reference GMSK modem is in dstar_gmsk.h.
 *
 * Memory: all state lives in caller-owned structs (no malloc). dstar_rx_t is
 * about 1 KB, dstar_tx_t about 0.5 KB + the fast-data buffer.
 *
 * Bits are one-per-byte (0/1) in air order (each byte LSB first, as the spec
 * says in Ap1.4). AMBE frames are 9 bytes in the same order D-STAR network
 * protocols use (byte 0 bit 0 is the first bit on air).
 */
#ifndef DSTAR_H
#define DSTAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants ----------------------------------------------------------- */

#define DSTAR_HEADER_BYTES   41u
#define DSTAR_HEADER_BITS    660u  /* after FEC */
#define DSTAR_FSYNC_BITS     15u
#define DSTAR_VOICE_BYTES    9u    /* 72 bits AMBE 2020 with FEC, opaque */
#define DSTAR_DATA_BYTES     3u    /* 24 bits slow data */
#define DSTAR_FRAME_BITS     96u
#define DSTAR_SUPERFRAME     21u   /* sync in data part of every 21st frame */
#define DSTAR_END_BITS       48u
#define DSTAR_TEXT_LEN       20u
#define DSTAR_PREAMBLE_BITS  64u   /* minimum bit sync before frame sync */

/* 15-bit frame sync 111011001010000, air order left to right */
#define DSTAR_FSYNC          0x7650u
/* 24-bit data sync 101010101011010001101000 (bytes 55 2D 16 LSB first) */
#define DSTAR_DSYNC          0xAAB468u
/* 48-bit end: 32 x "10" + 000100110101111 + 0 (bytes 55 55 55 55 C8 7A) */
#define DSTAR_END_PATTERN    0xAAAAAAAA135EULL

/* Slow data XOR (x^7+x^4+1 scrambler from all ones, restarted per frame). */
extern const uint8_t DSTAR_SLOW_SCRAMBLE[3]; /* 70 4F 93 */
/* AMBE frame used for "no voice" (the spec's packet-loss pattern). */
extern const uint8_t DSTAR_AMBE_SILENCE[9];  /* 9E 8D 32 88 26 1A 3F 61 E8 */

/* ---- header -------------------------------------------------------------- */

/* Flag 1 bits */
#define DSTAR_F1_DATA        0x80u /* 1 = data, 0 = voice */
#define DSTAR_F1_REPEATER    0x40u /* 1 = via repeater, 0 = direct */
#define DSTAR_F1_INTERRUPT   0x20u
#define DSTAR_F1_CONTROL     0x10u
#define DSTAR_F1_URGENT      0x08u /* "emergency": receivers open squelch */
#define DSTAR_F1_CTRL_MASK   0x07u
enum {
    DSTAR_CTRL_NULL = 0, DSTAR_CTRL_RELAY_UNAVAIL = 1, DSTAR_CTRL_NO_RESPONSE = 2,
    DSTAR_CTRL_ACK = 3, DSTAR_CTRL_RESEND = 4, DSTAR_CTRL_UNUSED = 5,
    DSTAR_CTRL_AUTO_REPLY = 6, DSTAR_CTRL_REPEATER_CTRL = 7,
};

/* Field order on air: flags, RPT2 (destination repeater), RPT1 (departure
 * repeater), UR (companion), MY (own 1), MY suffix (own 2), P_FCS. Callsign
 * fields are space padded, not NUL terminated. */
typedef struct {
    uint8_t flag[3];
    char rpt2[8];
    char rpt1[8];
    char ur[8];
    char my[8];
    char my_sfx[4];
} dstar_header_t;

/* Pack to the 41 byte form with P_FCS (CRC low byte first). */
void dstar_header_pack(const dstar_header_t *h, uint8_t out[DSTAR_HEADER_BYTES]);
/* Unpack; returns true when the CRC matches. */
bool dstar_header_unpack(const uint8_t in[DSTAR_HEADER_BYTES], dstar_header_t *h);
uint16_t dstar_header_crc(const uint8_t raw[DSTAR_HEADER_BYTES - 2]);

/* FEC: 41 bytes -> 660 air bits (conv code, interleave, scramble). */
void dstar_header_encode(const uint8_t raw[DSTAR_HEADER_BYTES], uint8_t bits[DSTAR_HEADER_BITS]);
/* 660 air bits -> 41 bytes. Returns the number of corrected code bit errors
 * (Viterbi metric); check the CRC with dstar_header_unpack(). */
int dstar_header_decode(const uint8_t bits[DSTAR_HEADER_BITS], uint8_t raw[DSTAR_HEADER_BYTES]);

/* Building blocks, exposed for tests and other users. */
void dstar_interleave(const uint8_t in[DSTAR_HEADER_BITS], uint8_t out[DSTAR_HEADER_BITS]);
void dstar_deinterleave(const uint8_t in[DSTAR_HEADER_BITS], uint8_t out[DSTAR_HEADER_BITS]);
/* XOR n bits with the x^7+x^4+1 sequence, register initialised to all ones. */
void dstar_scramble(uint8_t *bits, size_t n);

/* ---- callsign routing helpers ------------------------------------------- */

/* Copy s into an n-char field, upper-cased, space padded. */
void dstar_callsign_set(char *field, size_t n, const char *s);
/* Copy an n-char field to a NUL terminated string with trailing spaces cut.
 * out must hold n+1 bytes. */
void dstar_callsign_get(const char *field, size_t n, char *out);

/* Direct (simplex): RPT1 = RPT2 = "DIRECT  ", flag1 = 0. */
void dstar_header_simplex(dstar_header_t *h, const char *my, const char *sfx, const char *ur);
/* Through a repeater: rpt = 8 char repeater call with module in char 8
 * ("W1ABC  B"). RPT1 = rpt; RPT2 = same call with 'G' as 8th char when
 * via_gateway (zone-to-zone / reflector), else rpt. flag1 = 0x40. */
void dstar_header_repeater(dstar_header_t *h, const char *my, const char *sfx, const char *ur,
                           const char *rpt, bool via_gateway);

/* UR field commands (8 chars, written into ur). */
void dstar_ur_cq(char ur[8]);                                       /* "CQCQCQ  " */
void dstar_ur_call(char ur[8], const char *call);                   /* callsign routing */
void dstar_ur_zone(char ur[8], const char *rpt);                    /* "/W1ABC B": CQ in another zone */
void dstar_ur_link(char ur[8], const char *reflector, char module); /* "REF001CL" */
void dstar_ur_unlink(char ur[8]);                                   /* "       U" */
void dstar_ur_info(char ur[8]);                                     /* "       I" */
void dstar_ur_echo(char ur[8]);                                     /* "       E" */

typedef enum {
    DSTAR_UR_CQ, DSTAR_UR_CALLSIGN, DSTAR_UR_ZONE, DSTAR_UR_LINK, DSTAR_UR_UNLINK,
    DSTAR_UR_INFO, DSTAR_UR_ECHO, DSTAR_UR_OTHER,
} dstar_ur_kind_t;
dstar_ur_kind_t dstar_ur_classify(const char ur[8]);
bool dstar_header_is_direct(const dstar_header_t *h);
bool dstar_header_via_gateway(const dstar_header_t *h); /* RPT2 module char == 'G' */
/* Repeater turn-around: what a repeater sends on its output for an incoming
 * header (RPT1/RPT2 swapped, flag1 repeater bit set). */
void dstar_header_turnaround(const dstar_header_t *in, dstar_header_t *out);

/* ---- D-PRS ---------------------------------------------------------------- */

/* Build "$$CRCxxxx,<aprs>\r" into out (cap bytes incl. NUL). aprs is a TNC2
 * line, e.g. "W1ABC-7>API51,DSTAR*:!4224.30N/07112.31W>". Returns length or 0. */
size_t dstar_dprs_build(char *out, size_t cap, const char *aprs);
/* Check a received "$$CRC" line; returns true when the CRC matches. */
bool dstar_dprs_check(const char *line, size_t len);

/* ---- receiver ------------------------------------------------------------ */

typedef enum {
    DSTAR_EV_HEADER = 1,   /* header (radio header, or rebuilt from slow data) */
    DSTAR_EV_LATE_ENTRY,   /* voice stream found via data sync, no header yet */
    DSTAR_EV_TEXT,         /* 20 char slow data message complete */
    DSTAR_EV_SIMPLE_DATA,  /* raw 0x3X slow data bytes (GPS / D-PRS / user data) */
    DSTAR_EV_GPS_LINE,     /* a CR/LF terminated line of simple data */
    DSTAR_EV_CSQ,          /* code squelch block (0xC2) */
    DSTAR_EV_FAST_DATA,    /* one DV fast data block */
    DSTAR_EV_END,          /* end pattern received */
    DSTAR_EV_LOST,         /* sync lost without end pattern */
} dstar_event_type_t;

typedef struct {
    dstar_event_type_t type;
    union {
        struct {
            dstar_header_t hdr;
            bool crc_ok;
            bool from_slow_data;
            int fec_errors;
        } header;
        char text[DSTAR_TEXT_LEN + 1];
        struct {
            const uint8_t *data;
            size_t len;
        } data; /* SIMPLE_DATA, GPS_LINE, FAST_DATA (valid during the callback) */
        uint8_t csq[2];
    } u;
} dstar_event_t;

typedef struct {
    /* One 20 ms voice frame (opaque AMBE). seq = 0..20 position in the
     * superframe. Not called for frames that carry DV fast data. */
    void (*voice)(void *user, const uint8_t ambe[DSTAR_VOICE_BYTES], uint8_t seq);
    void (*event)(void *user, const dstar_event_t *ev);
} dstar_rx_cb_t;

typedef enum { DSTAR_RX_SEARCH = 0, DSTAR_RX_HEADER, DSTAR_RX_VOICE } dstar_rx_state_t;

#define DSTAR_LINE_MAX 96u

typedef struct {
    const dstar_rx_cb_t *cb;
    void *user;
    dstar_rx_state_t state;
    uint64_t sr;            /* last 64 received bits, newest in bit 0 */
    uint16_t nbits;         /* bits collected in buf */
    uint8_t buf[DSTAR_HEADER_BITS];
    uint8_t seq;            /* superframe position of the frame being received */
    uint8_t sync_miss;      /* consecutive superframes without data sync */
    uint8_t have_header;
    uint8_t max_sync_err;   /* tolerances (defaults in dstar_rx_init) */
    uint8_t max_dsync_err;
    uint8_t max_end_err;
    dstar_header_t hdr;
    /* slow data */
    uint8_t half[3];        /* first frame of the current 2-frame block */
    uint8_t text[DSTAR_TEXT_LEN];
    uint8_t text_mask;
    char last_text[DSTAR_TEXT_LEN + 1];
    uint8_t hdr_buf[DSTAR_HEADER_BYTES];
    uint8_t hdr_len;
    char line[DSTAR_LINE_MAX];
    uint8_t line_len;
    /* fast data */
    uint8_t fast_active;      /* current block carries fast data */
    uint8_t fast_len;         /* valid bytes announced by the mini header */
    uint8_t fast_buf[28];
    uint8_t held_voice[DSTAR_VOICE_BYTES]; /* seq 0 voice held until seq 1 tells fast/slow */
    uint8_t held_valid;
} dstar_rx_t;

void dstar_rx_init(dstar_rx_t *rx, const dstar_rx_cb_t *cb, void *user);
void dstar_rx_reset(dstar_rx_t *rx);          /* back to sync search */
void dstar_rx_push_bit(dstar_rx_t *rx, uint8_t bit);
void dstar_rx_push_bits(dstar_rx_t *rx, const uint8_t *bits, size_t n);
static inline dstar_rx_state_t dstar_rx_state(const dstar_rx_t *rx) { return rx->state; }

/* ---- transmitter --------------------------------------------------------- */

#define DSTAR_GPS_RING 256u

typedef struct {
    uint8_t raw_hdr[DSTAR_HEADER_BYTES];
    uint8_t seq;              /* next frame position 0..20 */
    uint32_t superframes;     /* completed superframes */
    /* slow data plan for the current superframe: 10 blocks x 6 bytes */
    uint8_t plan[10][6];
    uint8_t plan_kind[10];    /* 0 filler/gps, 1 text, 2 header, 3 csq */
    /* sources */
    char text[DSTAR_TEXT_LEN];
    uint8_t text_set;
    uint8_t text_period;      /* send text every N superframes (default 4, first one always) */
    uint8_t header_resend;    /* send header in slow data (default on) */
    uint8_t csq_set;
    uint8_t csq[2];
    uint8_t gps[DSTAR_GPS_RING];
    uint16_t gps_head, gps_tail;
    /* fast data */
    const uint8_t *fast;
    size_t fast_len, fast_pos;
    uint8_t fast_block;       /* building a fast block now */
    uint8_t fast_blk[28];
    uint8_t fast_blk_len;
} dstar_tx_t;

void dstar_tx_init(dstar_tx_t *tx, const dstar_header_t *hdr);
void dstar_tx_set_text(dstar_tx_t *tx, const char *msg);          /* NULL = none */
void dstar_tx_set_csq(dstar_tx_t *tx, bool on, uint8_t b0, uint8_t b1);
/* Queue simple data (GPS NMEA / D-PRS) bytes; returns bytes accepted. */
size_t dstar_tx_queue_data(dstar_tx_t *tx, const uint8_t *data, size_t len);
/* Use `data` (kept by reference until sent) as DV fast data; it is sent in
 * frames where dstar_tx_frame() gets ambe == NULL. */
void dstar_tx_set_fast_data(dstar_tx_t *tx, const uint8_t *data, size_t len);
bool dstar_tx_fast_busy(const dstar_tx_t *tx);

/* preamble_bits of "10" bit sync (>= 64) + frame sync + 660 header bits.
 * Returns bits written (preamble_bits + 675); out must be large enough. */
size_t dstar_tx_header(dstar_tx_t *tx, size_t preamble_bits, uint8_t *out);
/* One 96 bit frame. ambe == NULL -> fast data if queued, else silence. */
size_t dstar_tx_frame(dstar_tx_t *tx, const uint8_t *ambe, uint8_t out[DSTAR_FRAME_BITS]);
/* 48 bit end pattern. */
size_t dstar_tx_end(dstar_tx_t *tx, uint8_t out[DSTAR_END_BITS]);

#ifdef __cplusplus
}
#endif
#endif
