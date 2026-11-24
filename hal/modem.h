/* cfw modem interface: how a digital-voice protocol library (cfw/proto/NAME) plugs into the platform.
 *
 * Self-contained (stdint/stddef/stdbool only): a protocol library includes this header and nothing else
 * from the platform, so it builds and unit-tests on a PC as well as in the firmware.
 *
 * Data flow
 *
 *   RX:  radio -> backend (demodulator) -> modem core -> proto->rx_symbols(sym[], soft[])
 *        proto decodes frames; voice frames go out through pf->voice_rx(vocoder, bits) -> vocoder -> audio
 *   TX:  PTT -> proto->tx_start(); then the backend pulls symbols at the symbol clock through
 *        proto->tx_symbols(buf, max); voice for TX comes in as encoded frames via proto->tx_voice_frame()
 *        (the platform runs the vocoder encoder) ; proto returns 0 symbols when done -> TX ends.
 *   Control: pf->tune() lets a trunking proto (P25 trunking, DMR Tier III) move the radio between
 *        control and voice channels; pf->request_tx() lets it key up on its own (registration, ACKs).
 *
 * Symbols are hard decisions, one per byte: dibits 0..3 for 4-level modes (bits_per_symbol = 2,
 * order: the protocol's own dibit mapping, i.e. +3 -> 01, +1 -> 00, -1 -> 10, -3 -> 11 for C4FM / 4FSK as
 * in TIA-102.BAAA and ETSI TS 102 361-1), single bits 0/1 for GMSK (D-STAR). soft[] (may be NULL) carries a
 * signed confidence per symbol (-127..127, 0 = erasure) for decoders that use it.
 */
#ifndef CFW_MODEM_H
#define CFW_MODEM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CFW_MODEM_API_VERSION 3   /* 2: configure() + cfw_chan_cfg_t, multi-line status; 3: pf->voice_tx */

typedef enum {
    CFW_MODE_FM = 0,      /* analog, no proto */
    CFW_MODE_DMR,         /* ETSI TS 102 361, Tier II/III */
    CFW_MODE_P25P1,       /* TIA-102 Phase 1 FDMA (C4FM) */
    CFW_MODE_P25P2,       /* TIA-102 Phase 2 TDMA (H-DQPSK / H-CPM) */
    CFW_MODE_DSTAR,       /* JARL D-STAR DV (GMSK) */
    CFW_MODE_NXDN,        /* NXDN 4800/9600 */
    CFW_MODE_YSF,         /* Yaesu System Fusion C4FM */
    CFW_MODE_COUNT
} cfw_mode_t;

typedef enum {
    CFW_MOD_FM = 0,       /* analog FM voice */
    CFW_MOD_4FSK,         /* C4FM / 4FSK: DMR, P25 P1, NXDN, YSF */
    CFW_MOD_HCPM,         /* P25 Phase 2 inbound */
    CFW_MOD_HDQPSK,       /* P25 Phase 2 outbound */
    CFW_MOD_GMSK,         /* D-STAR */
} cfw_mod_t;

/* Air interface a proto needs; the backend configures the demodulator / modulator from this. */
typedef struct {
    cfw_mod_t mod;
    uint32_t  symbol_rate;      /* symbols/s: DMR 4800, P25P1 4800, P25P2 6000, NXDN 2400/4800, YSF 4800, D-STAR 4800 */
    uint8_t   bits_per_symbol;  /* 2 for 4-level, 1 for GMSK */
    uint16_t  deviation_hz;     /* outer symbol deviation (e.g. DMR 1944, P25 1800, YSF 2700, NXDN48 1050) */
    uint16_t  channel_bw_hz;    /* 12500 or 6250 */
    bool      tdma;             /* DMR / P25P2: slot timing matters, see cfw_slot_t */
} cfw_air_t;

/* Vocoders (implemented on the platform side: SCT3288 hardware, or software later). */
typedef enum {
    CFW_VOC_NONE = 0,
    CFW_VOC_AMBE2_49,     /* AMBE+2 3600x2450 (DMR, NXDN, YSF DN, P25 P2 half rate): 49 bits / 20 ms */
    CFW_VOC_IMBE_88,      /* IMBE 7200x4400 (P25 P1, YSF VW full rate): 88 bits / 20 ms */
    CFW_VOC_AMBE_DSTAR,   /* AMBE 3600x2400 (D-STAR): 49 voice bits in 72-bit FEC frame / 20 ms */
    CFW_VOC_COUNT
} cfw_vocoder_t;

typedef enum { CFW_LOG_ERR, CFW_LOG_INFO, CFW_LOG_DEBUG } cfw_log_level_t;

/* Call / talker information a proto reports for the UI. All fields optional (0 / ""). */
typedef struct {
    uint32_t src_id;            /* DMR/P25/NXDN radio id */
    uint32_t dst_id;            /* talkgroup / target */
    char     src_call[12];      /* D-STAR / YSF callsign */
    char     dst_call[12];
    uint8_t  slot;              /* TDMA slot 1/2, 0 = n/a */
    uint8_t  color_code;        /* DMR CC / P25 NAC low bits / NXDN RAN */
    uint16_t nac;               /* P25 */
    bool     encrypted;
    bool     voice;             /* call in progress */
    bool     private_call;      /* unit-to-unit (DMR FLCO 3, ...) */
    char     alias[32];         /* DMR talker alias (as far as received), "" = none */
} cfw_call_info_t;

/* Services the platform gives a proto. Valid from init() on. All callable from proto callbacks. */
typedef struct cfw_platform {
    uint32_t api_version;
    uint32_t (*now_ms)(void);
    void     (*log)(cfw_log_level_t lvl, const char *msg);
    /* decoded voice frame for the vocoder (nbits as in cfw_vocoder_t; errs = corrected bit errors or -1) */
    void     (*voice_rx)(cfw_vocoder_t voc, const uint8_t *bits, size_t nbits, int errs);
    /* data / SMS / GPS payloads, short-data, etc. (type is proto-defined) */
    void     (*data_rx)(uint8_t type, const uint8_t *data, size_t len);
    void     (*call_info)(const cfw_call_info_t *ci);              /* update UI */
    /* trunking: retune receive (and the TX frequency used for the next transmission). 0 = unchanged */
    bool     (*tune)(uint32_t rx_hz, uint32_t tx_hz);
    /* ask the platform to key up and start pulling tx_symbols (e.g. registration / ACK); false = refused */
    bool     (*request_tx)(void);
    /* persistent storage for the proto (ids, talkgroup lists): read-only NAND codeplug access for now */
    bool     (*codeplug_read)(uint32_t addr, void *buf, uint32_t len);
    /* API v3, TX: the next 20 ms of transmit audio, vocoder-encoded (mic / test tone / silence), bits packed MSB
     * first; nbits = 49 (AMBE+2 / AMBE payload) or 88 (IMBE). The proto calls it whenever its TX framing needs
     * a voice frame (it pulls at the air rate). Returns nbits, 0 = no vocoder. */
    size_t   (*voice_tx)(cfw_vocoder_t voc, uint8_t *bits, size_t nbits);
} cfw_platform_t;

/* Why TX starts. */
typedef struct {
    bool     voice;             /* PTT voice call (else data/control the proto asked for) */
    uint32_t dst_id;            /* from the channel / contact */
    char     dst_call[12];
    uint8_t  slot;
    uint8_t  color_code;
    bool     private_call;      /* unit-to-unit call to dst_id (DMR) */
    /* data (voice == false): short message for the proto (DMR: unconfirmed data, cfw SMS format, see proto_dmr.c) */
    const uint8_t *data;
    uint16_t data_len;
    const char *alias;          /* talker alias to send (DMR embedded LC), NULL = none */
    uint8_t  data_format;       /* DMR SMS: 0 = cfw format, 1 = M-SMS (Motorola TMS over IPv4/UDP 4007, SAP 4) */
} cfw_tx_req_t;

/* Channel settings handed to the proto when its mode is entered and whenever the user edits them
 * (API v2). Fields that do not apply to a mode are 0 / "". */
typedef struct {
    uint32_t rx_hz, tx_hz;
    uint32_t my_id;             /* own DMR / P25 / NXDN unit id */
    char     my_call[9];        /* own callsign (D-STAR MY, YSF source), space padded or NUL ended */
    uint32_t talkgroup;         /* DMR / P25 / NXDN group (TX target, RX highlight) */
    uint8_t  color_code;        /* DMR CC 0..15 */
    uint8_t  slot;              /* DMR / P25P2 slot 1..2, 0 = both */
    uint16_t nac;               /* P25 NAC 0..0xFFF (0x293 default, 0xF7E = any) */
    uint32_t wacn;              /* P25 Phase 2 scrambling seed: WACN (20 bit) + system id (12 bit) + NAC */
    uint16_t sysid;
    uint8_t  ran;               /* NXDN RAN 0..63 (0 = any) */
    uint8_t  dgid;              /* YSF DG-ID 0..99 (0 = open / all) */
    char     ur[9], rpt1[9], rpt2[9];   /* D-STAR routing */
    bool     trunk;             /* rx_hz is a control channel: P25 trunking / DMR Tier III TSCC */
    bool     direct;            /* simplex / talkaround (tx_hz == rx_hz): direct-mode framing (DMR DMO bursts, NXDN
                                 * RDCH, D-STAR DIRECT header, YSF MR direct); false = via a repeater (inbound framing) */
    bool     dmr_tdma_direct;   /* DMR simplex as dual-capacity TDMA direct (TS1/TS2 syncs) instead of normal MS syncs */
    uint32_t chan_step_hz;      /* DMR Tier III channel plan: f(lpcn) = rx_hz + (lpcn - 1) * step (default 12500) */
    uint8_t  dgid_tx;           /* YSF TX DG-ID + 1 (0 = transmit with dgid) */
    bool     nxdn96;            /* NXDN 9600 (4800 sym/s, EFR = IMBE 88-bit voice); false = NXDN48 (EHR) */
} cfw_chan_cfg_t;

/* A protocol library. All callbacks run in the platform's main loop (never in an interrupt) and must
 * return within ~2 ms: rx_symbols is fed in blocks of up to 64 symbols. ctx belongs to the proto. */
typedef struct cfw_proto {
    const char *name;           /* short, shown on screen: "DMR", "P25", ... */
    cfw_mode_t  mode;
    cfw_air_t   air;
    void       *ctx;

    int    (*init)(void *ctx, const cfw_platform_t *pf);           /* 0 = ok */
    void   (*rx_reset)(void *ctx);                                  /* retune / carrier lost / mode entered */
    void   (*rx_symbols)(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n);
    int    (*tx_start)(void *ctx, const cfw_tx_req_t *req);         /* 0 = ok, then tx_symbols is pulled */
    size_t (*tx_symbols)(void *ctx, uint8_t *sym, size_t max);      /* 0 = transmission finished */
    void   (*tx_voice_frame)(void *ctx, cfw_vocoder_t voc, const uint8_t *bits, size_t nbits);
    void   (*tx_stop)(void *ctx);                                   /* PTT released: finish (terminator) */
    void   (*tick)(void *ctx, uint32_t now_ms);                     /* every 10 ms */
    /* up to 3 UI lines separated by '\n', e.g. "TG91 < 9990201 TS1\nvoice 36 frames\nCC 851.0125" */
    size_t (*status)(void *ctx, char *buf, size_t len);
    /* API v2 (may be NULL): channel settings, called after rx_reset on mode entry and after every edit */
    void   (*configure)(void *ctx, const cfw_chan_cfg_t *cfg);
} cfw_proto_t;

/* Registration: put this at file scope in the proto's glue file (one per proto). The firmware collects
 * them through the .cfw_proto linker section; host builds (CFW_HOST defined) just get the object. */
#ifdef CFW_HOST
#define CFW_PROTO_REGISTER(obj) const cfw_proto_t *const cfw_proto_reg_##obj = &(obj)
#else
#define CFW_PROTO_REGISTER(obj) \
    __attribute__((used, section(".cfw_proto"))) const cfw_proto_t *const cfw_proto_reg_##obj = &(obj)
#endif

/* ---- vocoder backends (platform side) ---- */
typedef struct cfw_vocoder_ops {
    cfw_vocoder_t id;
    const char   *name;
    bool        interoperable;     /* false = test-only codec, real radios cannot decode it */
    /* encoded frame (packed MSB first, 49 or 88 bits) -> 160 samples 8 kHz PCM (20 ms). Return 0 = ok. */
    int (*decode)(const uint8_t *bits, size_t nbits, int16_t pcm[160]);
    /* 160 samples -> encoded frame (packed MSB first). Return number of bits. */
    size_t (*encode)(const int16_t pcm[160], uint8_t *bits, size_t nbits);
} cfw_vocoder_ops_t;

/* ---- modem backends (platform side: where symbols come from / go to) ---- */
typedef struct cfw_modem_backend {
    const char *name;
    bool (*supports)(const cfw_air_t *air);
    void (*start_rx)(const cfw_air_t *air, uint32_t rx_hz);
    void (*start_tx)(const cfw_air_t *air, uint32_t tx_hz);
    void (*stop)(void);
    void (*poll)(void);         /* main loop: move symbols */
} cfw_modem_backend_t;

#endif
