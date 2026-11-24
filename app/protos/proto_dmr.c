/* DMR glue: proto/dmr_nxdn receiver (Tier II conventional) + Tier III control-channel follow (dmr_t3).
 * Receive only: the D890UV has no 4FSK modulator path for the MCU yet (see docs/FEASIBILITY.md). */
#include "glue.h"
#include "emerg.h"
#include "stockdlg.h"
#include "dmr.h"
#include "dmr_t3.h"
#include "ambe_fec.h"
#include "dsp_mod.h"
#include "fec.h"
#include "tms.h"

/* SMS (Menu > SMS), two formats, both received always, TX by Settings > SMS format:
 *  cfw    DMR unconfirmed rate-1/2 data, SAP 9 (proprietary), payload "CFS" 0x01, u8 length, text (cfw radios only).
 *  M-SMS  the stock radios' default "SMS format" (CPS Digital Func): Motorola-style text message service = IPv4 / UDP
 *         port 4007 in DMR IP packet data (SAP 4). Source 12.x.x.x (CAI network 12 + 24-bit radio id), destination
 *         12.x.x.x (private) or 225.x.x.x (group); TMS payload = u16 length, header 0xA0 (ack wanted, ext), address
 *         length 0, extension 0x80|seq, 0x04 (UTF-16LE), text in UTF-16LE. Built from the public descriptions of the
 *         format - the stock D890UV / D168UV make the air frames inside the encrypted SCT3288 baseband firmware (the
 *         MCU only hands it the text), so this is NOT checked against a stock radio's transmission. Sent unconfirmed
 *         (rate 1/2); stock radios may send it confirmed. No TMS acknowledgement is sent back. */
#define SMS_SAP 9
#define SMS_SAP_IP 4
#define SMS_MAX_BURSTS 20

typedef struct {
    const cfw_platform_t *pf;
    cfw_chan_cfg_t cfg;
    dmr_rx_t rx;
    dmr_t3_t t3;
    cfw_call_info_t ci;
    uint32_t voice, csbk, calls;
    bool seen_call, trunk;
    char t3_line[28];
    /* TX (proto/dmr_nxdn talkspurt transmitter): DIRECT = simplex / talkaround with MS syncs, TDMA_DIRECT = dual
     * capacity direct (TS1/TS2), REPEATER = MS -> repeater input. One 132-dibit burst keyed every 60 ms in our slot. */
    txq_t q;
    dmr_call_tx_t ct;
    dmr_lc_t lc;
    uint8_t tx_phase, tx_slot;
    bool tx_stop;
    uint32_t tx_k;
    /* data (SMS) TX */
    uint8_t data_bursts[SMS_MAX_BURSTS][DMR_BURST_DIBITS];
    uint8_t data_n, data_k;
    /* talker alias: RX assembly (217 bits: header 49 + 3 x 56) and TX blocks */
    uint8_t ta_bits[28], ta_have, ta_fmt, ta_len;
    uint8_t ta_tx[4][9], ta_tx_n;
    dmr_keyring_t keys;                  /* RX keys (Settings > Encryption on) */
    dmr_priv_t priv;                     /* TX privacy session */
} g_dmr_t;

enum { DTX_VOICE = 1, DTX_TERM, DTX_DONE, DTX_DATA };

static g_dmr_t s;

/* ---- talker alias (TS 102 361-2 7.2.18): FLCO 4 header (format 2 bit, length 5 bit, 49 data bits), FLCO 5..7
 * blocks of 56 bits. Formats 0 = 7 bit, 1 = ISO 8 bit, 2 = UTF-8 (ASCII part shown), 3 = UTF-16BE. In the 8 and 16 bit
 * formats the first of the header's 49 bits is reserved (characters start at octet 3); 7 bit uses all 49. ---- */
#define TA_START(fmt) ((fmt) ? 1 : 0)
static void ta_put_bits(uint8_t *dst, int at, const uint8_t *src, int from, int n)
{
    for(int i = 0; i < n; i++) {
        int b = (src[(from + i) / 8] >> (7 - (from + i) % 8)) & 1;
        int k = at + i;
        if(b) { dst[k / 8] |= (uint8_t)(0x80u >> (k % 8)); } else { dst[k / 8] &= (uint8_t)~(0x80u >> (k % 8)); }
    }
}

static void ta_rx(const dmr_lc_t *lc)
{
    int blk = lc->flco - DMR_FLCO_TALKER_ALIAS_HDR;
    if(blk == 0) {
        uint8_t fmt = lc->raw[2] >> 6, len = (lc->raw[2] >> 1) & 31u;
        if(fmt != s.ta_fmt || len != s.ta_len) { s.ta_have = 0; memset(s.ta_bits, 0, sizeof s.ta_bits); }
        s.ta_fmt = fmt; s.ta_len = len;
        ta_put_bits(s.ta_bits, 0, lc->raw, 23, 49);       /* octet 2 bit 0 .. octet 8 */
    } else {
        ta_put_bits(s.ta_bits, 49 + 56 * (blk - 1), lc->raw, 16, 56);
    }
    s.ta_have |= (uint8_t)(1u << blk);
    /* decode the contiguous part */
    int bits = 0;
    for(int b = 0; b < 4 && (s.ta_have >> b & 1); b++) { bits += b ? 56 : 49; }
    int cw = s.ta_fmt == 0 ? 7 : s.ta_fmt == 3 ? 16 : 8, n = 0, st = TA_START(s.ta_fmt);
    char *o = s.ci.alias;
    for(int c = 0; c < s.ta_len && st + (c + 1) * cw <= bits && n < (int)sizeof s.ci.alias - 1; c++) {
        uint32_t v = 0;
        for(int k = 0; k < cw; k++) { int i = st + c * cw + k; v = v << 1 | (uint32_t)((s.ta_bits[i / 8] >> (7 - i % 8)) & 1); }
        o[n++] = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
    }
    o[n] = 0;
}

/* alias -> the header + block LCs, in the form the stock CPS Talk Alias setting asks for: ISO 8 bit (up to 27
 * characters), ISO 7 bit (31) or "Unicode" = UTF-16BE (13; the stock menu does not say UTF-8 or UTF-16) */
static void ta_tx_build(const char *a)
{
    uint8_t sel = stockdlg_alias_format();
    uint8_t fmt = sel == 1 ? 0 : sel == 2 ? 3 : 1;
    int cw = fmt == 0 ? 7 : fmt == 3 ? 16 : 8, nbits = TA_START(fmt), len = 0;
    uint8_t bits[28];
    memset(bits, 0, sizeof bits);
    for(; a[len] && len < 31 && nbits + cw <= 49 + 3 * 56; len++) {
        uint16_t v = (uint8_t)a[len];
        for(int k = cw - 1; k >= 0; k--, nbits++) { if(v >> k & 1) { bits[nbits / 8] |= (uint8_t)(0x80u >> (nbits % 8)); } }
    }
    s.ta_tx_n = (uint8_t)(nbits <= 49 ? 1 : 1 + (nbits - 49 + 55) / 56);
    for(int b = 0; b < s.ta_tx_n; b++) {
        uint8_t *lc = s.ta_tx[b];
        memset(lc, 0, 9);
        lc[0] = (uint8_t)(DMR_FLCO_TALKER_ALIAS_HDR + b);
        if(b == 0) {
            lc[2] = (uint8_t)(fmt << 6 | (uint8_t)len << 1);
            ta_put_bits(lc, 23, bits, 0, 49);
        } else {
            ta_put_bits(lc, 16, bits, 49 + 56 * (b - 1), 56);
        }
    }
}


static uint32_t lpcn_hz(uint16_t lpcn)
{
    uint32_t step = s.cfg.chan_step_hz ? s.cfg.chan_step_hz : 12500u;
    return s.cfg.rx_hz + (uint32_t)(lpcn - 1u) * step;
}

static void on_voice(void *user, uint8_t slot, const uint8_t frame[9], uint8_t idx, uint8_t crypt)
{
    (void)user; (void)idx;
    s.voice++;
    if(s.trunk) { dmr_t3_tick(&s.t3, s.pf->now_ms()); }
    if(crypt != DMR_CRYPT_ENCRYPTED && s.pf->voice_rx) {
        uint8_t d49[7];
        int e = ambe72_decode(frame, d49);                /* 72-bit vocoder socket frame -> 49 voice bits */
        s.pf->voice_rx(CFW_VOC_AMBE2_49, d49, 49, e);
    }
    (void)slot;
}

static void on_event(void *user, const dmr_event_t *ev)
{
    (void)user;
    if(s.trunk) { dmr_t3_on_rx(&s.t3, ev); }
    switch(ev->type) {
    case DMR_EV_VOICE_LC_EMB:
        if(ev->lc.flco >= DMR_FLCO_TALKER_ALIAS_HDR && ev->lc.flco <= DMR_FLCO_TALKER_ALIAS_B3) {
            ta_rx(&ev->lc);
            break;
        }
        if(ev->lc.flco != DMR_FLCO_GROUP_VOICE && ev->lc.flco != DMR_FLCO_UNIT_VOICE) { return; }   /* GPS, ... */
        /* fall through */
    case DMR_EV_VOICE_START:
        if(ev->type == DMR_EV_VOICE_START) {
            s.calls++;
            s.ci.alias[0] = 0; s.ta_have = 0; s.ta_len = 0;
        }
        s.seen_call = true;
        s.ci.src_id = ev->lc.src;
        s.ci.dst_id = ev->lc.dst;
        s.ci.private_call = ev->lc.flco == DMR_FLCO_UNIT_VOICE;
        s.ci.slot = (uint8_t)(ev->slot + 1);
        s.ci.color_code = ev->cc;
        s.ci.encrypted = ev->crypt == DMR_CRYPT_ENCRYPTED;
        s.ci.voice = true;
        if((ev->lc.svc_opts & 0x80u) && (ev->lc.flco == DMR_FLCO_GROUP_VOICE || ev->lc.flco == DMR_FLCO_UNIT_VOICE)) {
            dmr_rx_emergency(ev->lc.src, ev->lc.dst);
        }
        break;
    case DMR_EV_VOICE_END:
        s.ci.voice = false;
        break;
    case DMR_EV_DATA_PACKET: {
        uint8_t m[7 + 64];
        int n = -1;
        if(ev->crc_ok != 1) { return; }
        if(ev->dh.sap == SMS_SAP && ev->len >= 5 && memcmp(ev->data, "CFS\x01", 4) == 0) {
            n = ev->data[4];
            if(n > 64) { n = 64; }
            if(n > (int)ev->len - 5) { n = (int)ev->len - 5; }
            memcpy(m + 7, ev->data + 5, (size_t)n);
        } else if(ev->dh.sap == SMS_SAP_IP) {
            n = tms_parse(ev->data, ev->len, (char *)m + 7, 64);
        }
        if(n < 0) { return; }
        m[0] = (uint8_t)(ev->dh.src >> 16); m[1] = (uint8_t)(ev->dh.src >> 8); m[2] = (uint8_t)ev->dh.src;
        m[3] = (uint8_t)(ev->dh.dst >> 16); m[4] = (uint8_t)(ev->dh.dst >> 8); m[5] = (uint8_t)ev->dh.dst;
        m[6] = ev->dh.gi;
        if(s.pf->data_rx) { s.pf->data_rx(1, m, (size_t)(7 + n)); }
        return;
    }
    case DMR_EV_CSBK:
        s.csbk++;
        if(s.trunk && (ev->csbk.opcode == T3_TV_GRANT || ev->csbk.opcode == T3_PV_GRANT) &&
           !dmr_t3_tg_wanted(&s.t3, 0) && s.t3.state != T3_ST_TRAFFIC) {
            t3_grant_t g;
            dmr_t3_parse_grant(&ev->csbk, &g);
            if(!dmr_t3_tg_wanted(&s.t3, g.target)) {          /* seen, but not on our talkgroup list */
                sbuf_t b = sb_init(s.t3_line, sizeof s.t3_line);
                sb_s(&b, "grant TG"); sb_u(&b, g.target); sb_s(&b, " ch"); sb_u(&b, g.lpcn); sb_s(&b, " (skip)");
            }
        }
        return;
    default:
        return;
    }
    if(s.pf->call_info) { s.pf->call_info(&s.ci); }
}

/* ---- Tier III ---- */
static void t3_tune(void *user, uint16_t lpcn, uint8_t slot, bool control)
{
    (void)user; (void)slot; (void)control;
    s.pf->tune(lpcn_hz(lpcn), 0);
}

static void t3_tx(void *user, const dmr_csbk_t *c, uint8_t slot) { (void)user; (void)c; (void)slot; }  /* no TX */

static void t3_event(void *user, const t3_event_t *ev)
{
    (void)user;
    sbuf_t b = sb_init(s.t3_line, sizeof s.t3_line);
    switch(ev->type) {
    case T3_EV_HUNT_TUNE: sb_s(&b, "T3 hunt "); sb_mhz(&b, lpcn_hz(ev->lpcn)); break;
    case T3_EV_CAMPED:    sb_s(&b, "T3 TSCC sys "); sb_x(&b, ev->syscode, 4); break;
    case T3_EV_LOST_CC:   sb_s(&b, "T3 TSCC lost"); break;
    case T3_EV_GRANT:     sb_s(&b, "GRANT TG"); sb_u(&b, ev->grant.target); sb_s(&b, " ch"); sb_u(&b, ev->grant.lpcn); break;
    case T3_EV_RETURN:    sb_s(&b, "T3 back on TSCC"); break;
    default: break;
    }
}

static int dmr_init(void *ctx, const cfw_platform_t *pf) { (void)ctx; s.pf = pf; return 0; }

static void dmr_reset(void *ctx)
{
    (void)ctx;
    memset(&s.ci, 0, sizeof s.ci);
    s.voice = s.csbk = s.calls = 0;
    s.seen_call = false;
    s.t3_line[0] = 0;
    s.cfg.rx_hz = 0;                               /* configure() re-initialises the receiver */
}

/* privacy keys (glue.h hooks): cfw_cps key kinds 0 basic privacy (16 bit), 1 ARC4 (40 bit), 2 AES-128, 3 AES-256 ->
 * the library's cfw privacy schemes (dmr_priv.h: cfw radios only, the stock schemes run in the SCT3288 and are unknown) */
static const uint8_t k_alg[4] = { DMR_PRIV_CFW_BP, DMR_PRIV_CFW_RC4, DMR_PRIV_CFW_AES128, DMR_PRIV_CFW_AES256 };
static bool make_key(dmr_key_t *k, uint8_t kind, uint8_t kid, const uint8_t *key, uint8_t len)
{
    if(kind > 3 || len > sizeof k->key || (int)len != dmr_priv_key_len(k_alg[kind])) { return false; }
    memset(k, 0, sizeof *k);
    k->alg = k_alg[kind]; k->key_id = kid; k->len = len; memcpy(k->key, key, len);
    return true;
}

static const dmr_keyring_t *load_keys(void)
{
    uint8_t kind, kid, len; const uint8_t *key;
    s.keys.n = 0;
    for(int i = 0; s.keys.n < DMR_MAX_KEYS && cfw_crypt_key(i, 0, 3, &kind, &kid, &key, &len); i++) {
        if(make_key(&s.keys.k[s.keys.n], kind, kid, key, len)) { s.keys.n++; }
    }
    return s.keys.n ? &s.keys : NULL;
}

static void dmr_configure(void *ctx, const cfw_chan_cfg_t *cfg)
{
    (void)ctx;
    bool restart = !s.cfg.rx_hz || cfg->color_code != s.cfg.color_code || cfg->trunk != s.cfg.trunk ||
                   cfg->direct != s.cfg.direct;
    s.cfg = *cfg;
    if(!restart) { s.rx.keys = load_keys(); return; }
    /* one RX config hears BS (repeater output, CACH), MS (simplex / inbound) and TDMA-direct syncs */
    dmr_rx_cfg_t rc = { 4, s.cfg.trunk ? -1 : (int)(s.cfg.color_code & 15), true };
    dmr_cb_t cb = { on_voice, on_event, NULL };
    dmr_rx_init(&s.rx, &rc, &cb);
    s.rx.keys = load_keys();
    s.trunk = s.cfg.trunk;
    if(s.trunk) {
        dmr_t3_cfg_t tc;
        memset(&tc, 0, sizeof tc);
        tc.my_id = s.cfg.my_id;
        if(s.cfg.talkgroup) { tc.tg[0] = s.cfg.talkgroup; tc.n_tg = 1; }
        tc.hunt[0] = 1; tc.n_hunt = 1;                 /* the channel frequency is lpcn 1 of the plan */
        tc.tscc_slot = 0;
        tc.syscode_filter = -1;
        dmr_t3_cb_t tcb = { t3_tune, t3_tx, t3_event, NULL };
        dmr_t3_init(&s.t3, &tc, &tcb);
        dmr_t3_start(&s.t3);
    }
}

static void dmr_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    dmr_rx_dibits(&s.rx, sym, n);
}

static void dmr_tick(void *ctx, uint32_t now) { (void)ctx; if(s.trunk) { dmr_t3_tick(&s.t3, now); } }

/* One 60 ms TDMA frame: our slot = 12 dibits guard / PA ramp + 132 burst, the other 30 ms receive. The guard and
 * the other slot are DSP_SYM_GAP (unmodulated) - on a real radio the transmitter would be off there. */
static void tx_slot(const uint8_t out[DMR_SLOT_DIBITS])
{
    uint8_t gap[DMR_SLOT_DIBITS];
    memset(gap, DSP_SYM_GAP, sizeof gap);
    if(s.tx_slot == 2) { txq_put(&s.q, gap, sizeof gap); }
    txq_put(&s.q, gap, DMR_CACH_DIBITS);
    txq_put(&s.q, out + DMR_CACH_DIBITS, DMR_BURST_DIBITS);
    if(s.tx_slot != 2) { txq_put(&s.q, gap, sizeof gap); }
    s.tx_k++;
}

static void tx_build(void)
{
    uint8_t out[DMR_SLOT_DIBITS];
    if(s.tx_phase == DTX_DONE) { return; }
    if(s.tx_phase == DTX_DATA) {
        if(s.data_k >= s.data_n) { s.tx_phase = DTX_DONE; return; }
        memset(out, 0, DMR_CACH_DIBITS);
        memcpy(out + DMR_CACH_DIBITS, s.data_bursts[s.data_k++], DMR_BURST_DIBITS);
        tx_slot(out);
        return;
    }
    if(s.ta_tx_n && s.ct.v.n == 0 && !s.ct.priv) {       /* superframe start: odd ones carry the talker alias */
        uint16_t sf = s.ct.v.sf;
        if(sf & 1u) { emblc_encode(s.ta_tx[(sf / 2u) % s.ta_tx_n], s.ct.v.emb_bits); }
        else { emblc_encode(s.ct.v.lc.raw, s.ct.v.emb_bits); }
    }
    if(s.tx_phase == DTX_TERM) {
        dmr_call_tx_end(&s.ct, out);
        tx_slot(out);
        s.tx_phase = DTX_DONE;
        return;
    }
    uint8_t fr[27], d49[7];
    for(int f = 0; f < 3; f++) {
        memset(d49, 0, sizeof d49);
        s.pf->voice_tx(CFW_VOC_AMBE2_49, d49, 49);
        ambe72_encode(d49, &fr[9 * f]);
    }
    dmr_txk_t k = dmr_call_tx_next(&s.ct, fr, out);
    tx_slot(out);
    /* end the talkspurt after burst F (clean superframes) */
    if(s.tx_stop && k == DMR_TXK_VOICE && s.ct.v.n == 0) { s.tx_phase = DTX_TERM; }
}

static int dmr_tx_start(void *ctx, const cfw_tx_req_t *req)
{
    (void)ctx;
    if(!s.pf->voice_tx) { return -1; }
    uint32_t dst = req && req->dst_id ? req->dst_id : (s.cfg.talkgroup ? s.cfg.talkgroup : 91u);
    uint8_t cc = req && req->color_code ? req->color_code : s.cfg.color_code;
    s.tx_slot = (req && req->slot) ? req->slot : (s.cfg.slot ? s.cfg.slot : 1);
    dmr_link_t link = !s.cfg.direct ? DMR_LINK_REPEATER : s.cfg.dmr_tdma_direct ? DMR_LINK_TDMA_DIRECT : DMR_LINK_DIRECT;
    bool priv = req && req->private_call;
    if(req && !req->voice && req->data) {                /* SMS: idle bursts for receiver sync, then the data */
        static uint8_t pl[28 + 6 + 2 * 64];
        static uint8_t seq;
        uint16_t n = req->data_len > 64 ? 64 : req->data_len;
        size_t plen;
        uint8_t sap;
        if(req->data_format == 1) {
            plen = tms_build(pl, s.cfg.my_id, dst, !priv, req->data, n, ++seq);
            sap = SMS_SAP_IP;
        } else {
            memcpy(pl, "CFS\x01", 4); pl[4] = (uint8_t)n; memcpy(pl + 5, req->data, n);
            plen = (size_t)n + 5u;
            sap = SMS_SAP;
        }
        dmr_sync_t ds = dmr_link_data_sync(link, (uint8_t)(s.tx_slot & 1u));   /* as dmr_call_tx_start */
        int k = 0;
        for(; k < 3; k++) { dmr_tx_idle(cc, ds, s.data_bursts[k]); }
        int m = dmr_tx_data_unconfirmed(!priv, dst, s.cfg.my_id, sap, pl, plen, NULL, cc, ds,
                                        &s.data_bursts[k], SMS_MAX_BURSTS - k);
        if(m < 0) { return -1; }
        s.data_n = (uint8_t)(k + m); s.data_k = 0;
        txq_reset(&s.q);
        s.tx_phase = DTX_DATA;
        s.tx_stop = false;
        s.tx_k = 0;
        return 0;
    }
    if(!req || req->voice) { s.ta_tx_n = 0; if(req && req->alias) { ta_tx_build(req->alias); } }
    /* encrypted voice: channel key (Settings > Encryption on, not an amateur band); the privacy LC takes the odd
     * superframes, so no talker alias then */
    dmr_priv_t *pv = NULL;
    uint8_t kind, kid, klen; const uint8_t *kb;
    dmr_key_t k;
    if(cfw_crypt_tx(0, 3, &kind, &kid, &kb, &klen) && make_key(&k, kind, kid, kb, klen)) {
        if(dmr_priv_setup(&s.priv, &k) == 0) { dmr_priv_sync(&s.priv, cfw_crypt_iv(), 0); pv = &s.priv; s.ta_tx_n = 0; }
        memset(&k, 0, sizeof k);
    }
    dmr_lc_make_voice(&s.lc, !priv, dst, s.cfg.my_id, (uint8_t)((pv ? 0x40u : 0u) | (dmr_tx_emergency() ? 0x80u : 0u)));
    dmr_call_tx_start(&s.ct, link, s.tx_slot, cc, &s.lc, 2, pv);
    txq_reset(&s.q);
    s.tx_phase = DTX_VOICE;
    s.tx_stop = false;
    s.tx_k = 0;
    return 0;
}

static size_t dmr_tx_symbols(void *ctx, uint8_t *sym, size_t max)
{
    (void)ctx;
    size_t n = 0;
    while(n < max) {
        if(txq_empty(&s.q)) {
            if(s.tx_phase == DTX_DONE) { break; }
            tx_build();
        }
        n += txq_take(&s.q, sym + n, max - n);
    }
    return n;
}

static void dmr_tx_stop(void *ctx) { (void)ctx; if(s.tx_phase != DTX_DATA) { s.tx_stop = true; } }

static size_t dmr_status(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    sbuf_t b = sb_init(buf, len);
    if(s.seen_call) {
        sb_s(&b, "TG"); sb_u(&b, s.ci.dst_id); sb_s(&b, " < "); sb_u(&b, s.ci.src_id);
        sb_s(&b, " TS"); sb_u(&b, s.ci.slot);
        if(s.ci.encrypted) { sb_s(&b, " ENC"); }
        if(s.ci.private_call) { sb_s(&b, " PRV"); }
    } else {
        sb_s(&b, "DMR no call  CC"); sb_u(&b, s.cfg.color_code);
    }
    sb_c(&b, '\n');
    if(s.trunk && s.t3_line[0]) { sb_s(&b, s.t3_line); }
    else {
        sb_s(&b, s.ci.voice ? "voice " : "frames "); sb_u(&b, s.voice);
        sb_s(&b, " csbk "); sb_u(&b, s.csbk);
    }
    return sb_len(&b, buf);
}

static const cfw_proto_t k_dmr = {
    .name = "DMR",
    .mode = CFW_MODE_DMR,
    .air = { CFW_MOD_4FSK, 4800, 2, 1944, 12500, true },
    .ctx = &s,
    .init = dmr_init,
    .rx_reset = dmr_reset,
    .rx_symbols = dmr_symbols,
    .tx_start = dmr_tx_start,
    .tx_symbols = dmr_tx_symbols,
    .tx_stop = dmr_tx_stop,
    .tick = dmr_tick,
    .status = dmr_status,
    .configure = dmr_configure,
};
CFW_PROTO_REGISTER(k_dmr);
