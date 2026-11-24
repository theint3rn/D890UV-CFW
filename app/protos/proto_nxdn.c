/* NXDN glue: proto/dmr_nxdn. NXDN48 (2400 sym/s, EHR voice = AMBE+2 49 bits) and NXDN96 (4800 sym/s, EFR voice:
 * AMBE+2 full rate, the IMBE 7200x4400 bitstream -> cfw/voc/imbe; 88 <-> 144 bits FEC in ambe_fec.c).
 * The spec EFR tone pattern (NXDN TS 1-A 7.2.2) decodes to a 1011 Hz tone with voc/imbe. */
#include "glue.h"
#include "nxdn.h"
#include "ambe_fec.h"

typedef struct {
    const cfw_platform_t *pf;
    cfw_chan_cfg_t cfg;
    nxdn_rx_t rx;
    cfw_call_info_t ci;
    uint32_t voice, frames, crc_err;
    uint8_t ran;
    bool seen_call, inited, efr;     /* efr: NXDN96 channel */
    /* TX (NXDN48 EHR conference call on RDCH) */
    txq_t q;
    uint8_t tx_phase, tx_ran;
    bool tx_stop;
    uint32_t tx_k;
    uint8_t vcall[10], rel[10], sacch[9];
    uint8_t lich_facch, lich_voice;
    nxdn_keyring_t keys;             /* RX keys (Settings > Encryption on) */
    nxdn_vtx_t txc;                  /* TX call sequencing + cipher (enc = encrypted TX) */
} g_nxdn_t;

enum { NTX_START, NTX_VOICE, NTX_DONE };

static g_nxdn_t s;

static void on_voice(void *user, const uint8_t *frame, unsigned nbits, uint8_t pos, uint8_t crypt)
{
    (void)user; (void)pos;
    s.voice++;
    if(crypt == NXDN_CRYPT_ENCRYPTED || !s.pf->voice_rx) { return; }
    if(nbits == 72) {                       /* EHR */
        uint8_t d49[7];
        int e = ambe72_decode(frame, d49);
        s.pf->voice_rx(CFW_VOC_AMBE2_49, d49, 49, e);
    } else if(nbits == 144) {               /* EFR: IMBE-compatible full rate frame */
        uint8_t d88[11];
        int e = ambe144_decode(frame, d88);
        s.pf->voice_rx(CFW_VOC_IMBE_88, d88, 88, e);
    }
}

static void on_event(void *user, const nxdn_event_t *ev)
{
    (void)user;
    switch(ev->type) {
    case NXDN_EV_LICH: s.frames++; return;
    case NXDN_EV_CRC_ERROR: s.crc_err++; return;
    case NXDN_EV_SACCH: s.ran = ev->sr & 0x3F; return;
    case NXDN_EV_SACCH_MSG:
        s.ran = ev->sr & 0x3F;
        /* late entry: the library starts the call with zero IDs and parses the VCALL from SACCH without an
         * event, so take the IDs from the SACCH superframe message here */
        if(ev->len == 9 && ev->data && (ev->data[0] & 0x3Fu) == NXDN_MSG_VCALL && s.ci.voice) {
            nxdn_call_t c;
            nxdn_msg_parse_call(ev->data, &c);
            if(c.src != s.ci.src_id || c.dst != s.ci.dst_id) {
                s.ci.src_id = c.src;
                s.ci.dst_id = c.dst;
                s.ci.encrypted = c.cipher != 0;
                if(s.pf->call_info) { s.pf->call_info(&s.ci); }
            }
        }
        return;
    case NXDN_EV_VOICE_START:
        s.seen_call = true;
        s.ci.src_id = ev->call.src;
        s.ci.dst_id = ev->call.dst;
        s.ci.color_code = ev->sr & 0x3F;
        s.ci.encrypted = ev->call.cipher != 0;
        s.ci.voice = true;
        break;
    case NXDN_EV_VOICE_END:
        s.ci.voice = false;
        break;
    default:
        return;
    }
    if(s.pf->call_info) { s.pf->call_info(&s.ci); }
}

static int nx_init(void *ctx, const cfw_platform_t *pf) { (void)ctx; s.pf = pf; return 0; }

static void nx_reset(void *ctx)
{
    (void)ctx;
    memset(&s.ci, 0, sizeof s.ci);
    s.voice = s.frames = s.crc_err = 0;
    s.seen_call = false;
    s.inited = false;
}

/* NXDN48: 2400 sym/s, +/-1050 Hz, 6.25 kHz; NXDN96: 4800 sym/s, +/-2400 Hz, 12.5 kHz (dsp DSP_MODE_NXDN96) */
static const cfw_air_t k_air48 = { CFW_MOD_4FSK, 2400, 2, 1050, 6250, false };
static const cfw_air_t k_air96 = { CFW_MOD_4FSK, 4800, 2, 2400, 12500, false };
static cfw_proto_t k_nxdn;

/* privacy keys (glue.h hooks): kinds 7 scrambler (15-bit key, 2 bytes), 8 DES (8), 9 AES-256 (32) */
static const nxdn_keyring_t *load_keys(void)
{
    uint8_t kind, kid, len; const uint8_t *key;
    s.keys.n = 0;
    for(int i = 0; s.keys.n < NXDN_MAX_KEYS && cfw_crypt_key(i, 7, 9, &kind, &kid, &key, &len); i++) {
        nxdn_key_t *k = &s.keys.k[s.keys.n];
        if(len > sizeof k->key) { continue; }
        k->cipher = (uint8_t)(kind - 6u); k->key_id = kid & 0x3Fu; k->len = len; memcpy(k->key, key, len);
        s.keys.n++;
    }
    return s.keys.n ? &s.keys : NULL;
}

static void nx_configure(void *ctx, const cfw_chan_cfg_t *cfg)
{
    (void)ctx;
    bool efr = cfg->nxdn96;
    bool restart = !s.inited || cfg->ran != s.cfg.ran || efr != s.efr;
    /* rate changed on a running channel: retune to the same frequency = restart the demodulator with the new .air */
    if(s.inited && efr != s.efr && s.pf && s.pf->tune) { s.pf->tune(cfg->rx_hz, 0); }
    s.cfg = *cfg;
    s.efr = efr;
    k_nxdn.air = efr ? k_air96 : k_air48;
    if(!restart) { s.rx.keys = load_keys(); return; }
    nxdn_rx_cfg_t rc = { 2, efr, false, s.cfg.ran ? (int)s.cfg.ran : -1 };
    nxdn_cb_t cb = { on_voice, on_event, NULL };
    nxdn_rx_init(&s.rx, &rc, &cb);
    s.rx.keys = load_keys();
    s.inited = true;
}

static void nx_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    if(s.inited) { nxdn_rx_dibits(&s.rx, sym, n); }
}

static int nx_tx_start(void *ctx, const cfw_tx_req_t *req)
{
    (void)ctx;
    if(!s.pf->voice_tx) { return -1; }
    uint16_t dst = (uint16_t)(req && req->dst_id ? req->dst_id : (s.cfg.talkgroup ? s.cfg.talkgroup : 1u));
    uint16_t src = (uint16_t)(s.cfg.my_id & 0xFFFFu);
    s.tx_ran = s.cfg.ran ? s.cfg.ran : 1;
    nxdn_msg_build_vcall(s.vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, src, dst);
    nxdn_msg_build_vcall(s.rel, NXDN_MSG_TX_REL, NXDN_CALL_CONFERENCE, src, dst);
    /* encrypted TX (Settings > Encryption on, channel key, not an amateur band -> cfw_crypt_tx): scramble (kind 7),
     * DES-OFB (8), AES-256-OFB (9) on NXDN48 (EHR) and NXDN96 (EFR); DES/AES send VCALL_IV in the header and
     * every 2nd superframe (nxdn_vtx_*). Key length must match the cipher, else the call goes out clear. */
    uint8_t kind, kid, klen; const uint8_t *kb;
    nxdn_key_t k;
    memset(&k, 0, sizeof k);
    bool enc = cfw_crypt_tx(7, 9, &kind, &kid, &kb, &klen) &&
               klen == (kind == 7 ? 2u : kind == 8 ? 8u : 32u);
    if(enc) { k.cipher = (uint8_t)(kind - 6u); k.key_id = kid & 0x3Fu; k.len = klen; memcpy(k.key, kb, klen); }
    uint64_t iv = ((uint64_t)cfw_crypt_iv() << 32) | cfw_crypt_iv();
    nxdn_vtx_start(&s.txc, enc ? &k : NULL, s.efr, iv, s.vcall);
    cipher_wipe(&k, sizeof k);
    memset(s.sacch, 0, sizeof s.sacch);
    memcpy(s.sacch, s.vcall, 8);
    /* conventional simplex and repeater are the same from the SU side (proto/dmr_nxdn README): RF channel RDCH,
     * direction 0; FACCH1 in both halves (VCALL first, TX_REL last) vs voice with superframe SACCH */
    s.lich_facch = nxdn_lich_rdch_voice(false, NXDN_STEAL_BOTH);
    s.lich_voice = nxdn_lich_rdch_voice(true, NXDN_STEAL_NONE);
    txq_reset(&s.q);
    s.tx_phase = NTX_START;
    s.tx_stop = false;
    s.tx_k = 0;
    return 0;
}

static void nx_build(void)
{
    uint8_t info[4], frame[NXDN_FRAME_DIBITS];
    if(s.tx_phase == NTX_START) {                   /* preamble before the first frame */
        uint8_t pre[64];
        txq_put(&s.q, pre, nxdn_tx_preamble(4, pre));
    }
    if(s.tx_phase == NTX_START || s.tx_stop) {      /* FACCH1 VCALL first, TX_REL last (both halves) */
        const uint8_t *m = s.tx_phase == NTX_START ? s.vcall : s.rel;
        memset(s.sacch, 0, sizeof s.sacch);
        memcpy(s.sacch, s.vcall, 8);
        nxdn_sacch_quarter(s.sacch, 0, s.tx_ran, false, info);
        nxdn_tx_voice_frame(s.lich_facch, info, m, s.tx_phase == NTX_START ? nxdn_vtx_header2(&s.txc) : m, NULL,
                            s.efr, frame);
        txq_put(&s.q, frame, sizeof frame);
        s.tx_phase = s.tx_phase == NTX_START ? NTX_VOICE : NTX_DONE;
        if(s.tx_phase == NTX_DONE) { nxdn_vtx_wipe(&s.txc); }
        return;
    }
    uint8_t vch[36];
    if(s.efr) {
        for(int i = 0; i < 2; i++) {                /* NXDN96: 2 EFR VCH = 40 ms */
            uint8_t d88[11];
            memset(d88, 0, sizeof d88);
            s.pf->voice_tx(CFW_VOC_IMBE_88, d88, 88);
            ambe144_encode(d88, &vch[18 * i]);
        }
    } else {
        for(int i = 0; i < 4; i++) {                /* NXDN48: 4 EHR VCH = 80 ms */
            uint8_t d49[7];
            memset(d49, 0, sizeof d49);
            s.pf->voice_tx(CFW_VOC_AMBE2_49, d49, 49);
            ambe72_encode(d49, &vch[9 * i]);
        }
    }
    uint8_t q;
    memcpy(s.sacch, nxdn_vtx_frame(&s.txc, vch, &q), 9);    /* encrypts the VCH; VCALL or VCALL_IV superframe */
    nxdn_sacch_quarter(s.sacch, q, s.tx_ran, true, info);
    nxdn_tx_voice_frame(s.lich_voice, info, NULL, NULL, vch, s.efr, frame);
    txq_put(&s.q, frame, sizeof frame);
    s.tx_k++;
}

static size_t nx_tx_symbols(void *ctx, uint8_t *sym, size_t max)
{
    (void)ctx;
    size_t n = 0;
    while(n < max) {
        if(txq_empty(&s.q)) {
            if(s.tx_phase == NTX_DONE) { break; }
            nx_build();
        }
        n += txq_take(&s.q, sym + n, max - n);
    }
    return n;
}

static void nx_tx_stop(void *ctx) { (void)ctx; s.tx_stop = true; }

static size_t nx_status(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    sbuf_t b = sb_init(buf, len);
    if(s.seen_call && !s.ci.src_id && !s.ci.dst_id) {
        sb_s(&b, "NXDN call, IDs pending");                  /* late entry: IDs follow in SACCH */
    } else if(s.seen_call) {
        sb_s(&b, "TG"); sb_u(&b, s.ci.dst_id); sb_s(&b, " < "); sb_u(&b, s.ci.src_id);
        sb_s(&b, " RAN"); sb_u(&b, s.ran);
        if(s.ci.encrypted) { sb_s(&b, " ENC"); }
    } else {
        sb_s(&b, s.efr ? "NXDN96 no call  RAN" : "NXDN no call  RAN"); sb_u(&b, s.cfg.ran);
    }
    sb_c(&b, '\n');
    sb_s(&b, "frames "); sb_u(&b, s.frames); sb_s(&b, " voice "); sb_u(&b, s.voice);
    if(s.crc_err) { sb_s(&b, " crc! "); sb_u(&b, s.crc_err); }
    return sb_len(&b, buf);
}

static cfw_proto_t k_nxdn = {          /* not const: .air follows the channel's NXDN48 / NXDN96 setting */
    .name = "NXDN",
    .mode = CFW_MODE_NXDN,
    .air = { CFW_MOD_4FSK, 2400, 2, 1050, 6250, false },
    .ctx = &s,
    .init = nx_init,
    .rx_reset = nx_reset,
    .rx_symbols = nx_symbols,
    .tx_start = nx_tx_start,
    .tx_symbols = nx_tx_symbols,
    .tx_stop = nx_tx_stop,
    .status = nx_status,
    .configure = nx_configure,
};
CFW_PROTO_REGISTER(k_nxdn);
