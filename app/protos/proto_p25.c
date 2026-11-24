/* P25 glue: Phase 1 (C4FM, conventional + trunking control channel follow) and Phase 2 (TDMA voice)
 * around proto/p25. Receive only. */
#include "glue.h"
#include "p25_p1.h"
#include "p25_p2.h"
#include "p25_trunk.h"
#include "p25_tsbk.h"

typedef struct {
    const cfw_platform_t *pf;
    cfw_chan_cfg_t cfg;
    bool inited;
    /* phase 1 */
    p25_p1_rx_t p1;
    p25_trunk_t trunk;
    bool trunking;
    uint32_t last_ms;
    char trunk_line[28];
    /* phase 2 */
    p25_p2_rx_t p2;
    p25_p2_scrambler_t scr;
    p25_keyring_t keys;           /* user keys from the CPS (RX decryption only) */
    p25_crypt_t crypt;
    /* shared display */
    cfw_call_info_t ci;
    uint16_t nac;
    uint32_t voice, tsbk, frames;
    bool seen_call, is_p2;
    /* TX */
    txq_t q;
    p25_p1_voice_tx_t vt;
    uint8_t tx_phase;
    bool tx_stop;
    uint32_t tx_sf;
    p25_p2_scrambler_t tx_scr;
    uint16_t tx_tg;
} g_p25_t;

enum { PTX_START, PTX_VOICE, PTX_END, PTX_DONE };

static g_p25_t s;

static void call_update(void) { if(s.pf->call_info) { s.pf->call_info(&s.ci); } }

/* ---------------- phase 1 ---------------- */
static void p1_voice(void *ctx, const uint8_t imbe[P25_IMBE_BYTES], int errs)
{
    (void)ctx;
    s.voice++;
    if(s.trunking) { p25_trunk_voice_activity(&s.trunk); }
    if(s.pf->voice_rx) { s.pf->voice_rx(CFW_VOC_IMBE_88, imbe, 88, errs); }
}

static void p1_event(void *ctx, const p25_p1_event_t *ev)
{
    (void)ctx;
    if(s.trunking) { p25_trunk_p1_event(&s.trunk, ev); }
    s.frames++;
    s.nac = ev->nac;
    switch(ev->type) {
    case P25_P1_EV_TSBK:
        if(ev->u.tsbk.crc_ok) { s.tsbk++; }
        break;
    case P25_P1_EV_HDU:
        s.seen_call = true;
        s.ci.voice = true;
        s.ci.dst_id = ev->u.hdu.tgid;
        s.ci.encrypted = ev->u.hdu.algid != 0x80;
        call_update();
        break;
    case P25_P1_EV_LDU1:
        if(ev->u.ldu1.lc_ok) {
            p25_lc_info_t li;
            p25_lc_parse(&ev->u.ldu1.lc, &li);
            if(li.known) {
                s.seen_call = true;
                s.ci.voice = true;
                s.ci.dst_id = li.tgid;
                s.ci.src_id = li.src;
                s.ci.encrypted = (li.svc_opts & 0x40) != 0;
                call_update();
            }
        }
        break;
    case P25_P1_EV_TDU:
    case P25_P1_EV_TDULC:
        s.ci.voice = false;
        call_update();
        break;
    default:
        break;
    }
}

static const p25_p1_cb_t k_p1_cb = { p1_voice, p1_event };

/* ---------------- trunking ---------------- */
static void tr_tune(void *ctx, uint32_t rx_hz, uint32_t tx_hz, p25_mode_t mode, uint8_t slot)
{
    (void)ctx; (void)mode; (void)slot;
    s.pf->tune(rx_hz, tx_hz);
}

static void tr_event(void *ctx, const p25_trunk_event_t *ev)
{
    (void)ctx;
    sbuf_t b = sb_init(s.trunk_line, sizeof s.trunk_line);
    switch(ev->type) {
    case P25_TE_CC_FOUND: sb_s(&b, "CC "); sb_mhz(&b, ev->rx_hz); break;
    case P25_TE_CC_LOST:  sb_s(&b, "CC lost, hunting"); break;
    case P25_TE_SITE:     sb_s(&b, "SYS "); sb_x(&b, s.trunk.sysid, 3); sb_s(&b, " site "); sb_u(&b, s.trunk.rfss);
                          sb_c(&b, '-'); sb_u(&b, s.trunk.site); break;
    case P25_TE_CALL_START:
        sb_s(&b, "GRANT TG"); sb_u(&b, ev->tg); sb_s(&b, " "); sb_mhz(&b, ev->rx_hz);
        if(ev->mode == P25_MODE_P2) { sb_s(&b, " P2"); }
        s.seen_call = true;
        s.ci.dst_id = ev->tg;
        s.ci.src_id = ev->src;
        s.ci.voice = true;
        call_update();
        break;
    case P25_TE_CALL_END: sb_s(&b, "call end, back on CC"); s.ci.voice = false; call_update(); break;
    case P25_TE_DENY:     sb_s(&b, "DENY "); sb_x(&b, ev->src, 2); break;
    default: return;
    }
}

static const p25_trunk_cb_t k_tr_cb = { tr_tune, tr_event };

/* ---------------- phase 2 ---------------- */
static void p2_voice(void *ctx, unsigned ch, const uint8_t frame[P25_AMBE_BYTES], int errs)
{
    (void)ctx; (void)ch;
    s.voice++;
    if(s.pf->voice_rx) { s.pf->voice_rx(CFW_VOC_AMBE2_49, frame, 49, errs); }
}

static void p2_event(void *ctx, const p25_p2_event_t *ev)
{
    (void)ctx;
    s.frames++;
    if(ev->type != P25_P2_EV_MAC) { return; }
    if(ev->mac.opcode == P25_MAC_PTT) {
        s.seen_call = true;
        s.ci.voice = true;
        s.ci.src_id = ev->mac.src;
        s.ci.dst_id = ev->mac.grp;
        s.ci.slot = (uint8_t)(ev->channel + 1);
        s.ci.encrypted = ev->mac.algid != 0x80 && ev->mac.algid != 0;
        call_update();
    } else if(ev->mac.opcode == P25_MAC_END_PTT) {
        s.ci.voice = false;
        call_update();
    }
}

static const p25_p2_cb_t k_p2_cb = { p2_voice, p2_event };

/* ---------------- common ---------------- */
static int p25_init(void *ctx, const cfw_platform_t *pf) { (void)ctx; s.pf = pf; return 0; }

static void p25_reset(void *ctx)
{
    (void)ctx;
    memset(&s.ci, 0, sizeof s.ci);
    s.voice = s.tsbk = s.frames = 0;
    s.seen_call = false;
    s.inited = false;
    s.trunk_line[0] = 0;
    if(s.trunking) { p25_trunk_stop(&s.trunk); s.trunking = false; }
}

/* user keys (app/radio.c reads them from the CPS codeplug; weak default = none) */
__attribute__((weak)) bool cfw_p25_key(int i, uint8_t *algid, uint16_t *kid, const uint8_t **key, uint8_t *len)
{
    (void)i; (void)algid; (void)kid; (void)key; (void)len;
    return false;
}

static const p25_keyring_t *load_keys(void)
{
    p25_keyring_init(&s.keys);
    uint8_t algid, len; uint16_t kid; const uint8_t *key;
    for(int i = 0; i < P25_CRYPT_MAX_KEYS && cfw_p25_key(i, &algid, &kid, &key, &len); i++) {
        p25_keyring_add(&s.keys, algid, kid, key, len);
    }
    return s.keys.n ? &s.keys : NULL;
}

static void p1_configure(void *ctx, const cfw_chan_cfg_t *cfg)
{
    (void)ctx;
    bool restart = !s.inited || cfg->nac != s.cfg.nac || cfg->trunk != s.cfg.trunk;
    s.cfg = *cfg;
    /* keys follow Settings > Encryption (radio_apply calls configure after a change) */
    if(!restart) { const p25_keyring_t *k = load_keys(); if(k != s.p1.keys) { p25_p1_rx_set_crypt(&s.p1, k, &s.crypt); } return; }
    s.is_p2 = false;
    p25_p1_rx_init(&s.p1, (s.cfg.trunk || !s.cfg.nac || s.cfg.nac == 0xF7E) ? P25_NAC_ANY : s.cfg.nac, &k_p1_cb, NULL);
    p25_p1_rx_set_crypt(&s.p1, load_keys(), &s.crypt);
    if(s.trunking) { p25_trunk_stop(&s.trunk); }
    s.trunking = s.cfg.trunk;
    if(s.trunking) {
        p25_trunk_init(&s.trunk, &k_tr_cb, NULL);
        p25_trunk_add_cc(&s.trunk, s.cfg.rx_hz);
        if(s.cfg.talkgroup) { p25_trunk_add_tg(&s.trunk, (uint16_t)s.cfg.talkgroup, 1, false); }
        s.trunk.scan_all = true;
        s.trunk.default_prio = 10;
        s.last_ms = s.pf->now_ms();
        p25_trunk_start(&s.trunk);
    }
    s.inited = true;
}

static void p2_configure(void *ctx, const cfw_chan_cfg_t *cfg)
{
    (void)ctx;
    bool restart = !s.inited || cfg->slot != s.cfg.slot || cfg->nac != s.cfg.nac || cfg->wacn != s.cfg.wacn ||
                   cfg->sysid != s.cfg.sysid;
    s.cfg = *cfg;
    if(!restart) { const p25_keyring_t *k = load_keys(); if(k != s.p2.keys) { p25_p2_rx_set_crypt(&s.p2, k, &s.crypt); } return; }
    p25_p2_rx_init(&s.p2, &k_p2_cb, NULL);
    s.is_p2 = true;
    s.p2.channel_filter = s.cfg.slot ? (int8_t)(s.cfg.slot - 1) : -1;
    /* voice bursts are scrambled with a sequence seeded by WACN / system id / NAC (TIA-102.BBAC) */
    p25_p2_scrambler_init(&s.scr, s.cfg.wacn, s.cfg.sysid, s.cfg.nac);
    p25_p2_rx_set_scrambler(&s.p2, &s.scr);
    p25_p2_rx_set_crypt(&s.p2, load_keys(), &s.crypt);
    s.inited = true;
}

static void p1_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    if(s.inited) { p25_p1_rx_push(&s.p1, sym, n, 0); }
}

static void p2_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    if(s.inited) { p25_p2_rx_push(&s.p2, sym, n, 0); }
}

static void p1_tick(void *ctx, uint32_t now)
{
    (void)ctx;
    if(!s.trunking) { return; }
    p25_trunk_tick(&s.trunk, now - s.last_ms);
    s.last_ms = now;
}

/* ---------------- TX ---------------- */
static uint8_t s_air[P25_LDU_AIR + P25_TDU_AIR + P25_HDU_AIR];

static int p25_tx_start(void *ctx, const cfw_tx_req_t *req)
{
    (void)ctx;
    if(!s.pf->voice_tx) { return -1; }
    s.tx_tg = (uint16_t)(req && req->dst_id ? req->dst_id : (s.cfg.talkgroup ? s.cfg.talkgroup : 1u));
    txq_reset(&s.q);
    s.tx_phase = PTX_START;
    s.tx_stop = false;
    s.tx_sf = 0;
    if(s.is_p2) { p25_p2_scrambler_init(&s.tx_scr, s.cfg.wacn, s.cfg.sysid, s.cfg.nac); }
    return 0;
}

static void p1_build(void)
{
    uint16_t nac = s.cfg.nac ? s.cfg.nac : 0x293;
    switch(s.tx_phase) {
    case PTX_START: {
        p25_lc_t lc;
        p25_lc_group_voice(&lc, 0, s.tx_tg, s.cfg.my_id);
        txq_put(&s.q, s_air, p25_p1_voice_tx_start(&s.vt, s_air, nac, &lc, 0));
        s.tx_phase = PTX_VOICE;
        break;
    }
    case PTX_VOICE:
        if(s.tx_stop) {
            txq_put(&s.q, s_air, p25_p1_voice_tx_end(&s.vt, s_air, NULL));
            s.tx_phase = PTX_DONE;
            break;
        }
        for(int f = 0; f < 9; f++) {               /* one LDU = 9 IMBE frames = 180 ms */
            uint8_t imbe[P25_IMBE_BYTES];
            memset(imbe, 0, sizeof imbe);
            s.pf->voice_tx(CFW_VOC_IMBE_88, imbe, 88);
            size_t n = p25_p1_voice_tx_push(&s.vt, s_air, imbe);
            if(n) { txq_put(&s.q, s_air, n); }
        }
        break;
    default:
        break;
    }
}

/* Phase 2: one superframe (12 slots x 180 dibits = 360 ms), voice on logical channel 0 (5 voice bursts =
 * 18 AMBE frames), MAC_PTT / MAC_END_PTT on its SACCH, idle MAC on channel 1. OUTBOUND slot format with the
 * cfw CPM modulator: a test link between cfw radios, not a real inbound H-CPM burst. */
static void p2_build(void)
{
    static uint8_t o[P25_P2_SLOT];
    p25_p2_ess_t ess = { 0x80, 0, { 0 } };
    uint8_t essb[16], essa[28];
    p25_p2_ess_encode(&ess, essb, essa);
    bool last = s.tx_stop;
    int vb = 0;
    for(unsigned slot = 0; slot < P25_P2_SLOTS; slot++) {
        int isch = p25_p2_isch_for_slot(slot, s.tx_sf);
        unsigned ch = p25_p2_slot_channel(slot);
        p25_mac_pdu_t m;
        if(ch == 0 && slot < 10) {
            uint8_t fr[4][P25_AMBE_BYTES];
            int nf = vb < 4 ? 4 : 2;
            for(int i = 0; i < nf; i++) { memset(fr[i], 0, P25_AMBE_BYTES); s.pf->voice_tx(CFW_VOC_AMBE2_49, fr[i], 49); }
            if(vb < 4) { p25_p2_build_4v(o, slot, isch, &s.tx_scr, (const uint8_t (*)[P25_AMBE_BYTES])fr, essb + 4 * vb); }
            else { p25_p2_build_2v(o, slot, isch, &s.tx_scr, (const uint8_t (*)[P25_AMBE_BYTES])fr, essa); }
            vb++;
        } else if(ch == 0) {
            if(!last) { p25_mac_ptt(&m, true, s.cfg.my_id, s.tx_tg); }
            else { p25_mac_end_ptt(&m, true, s.cfg.nac, s.cfg.my_id, s.tx_tg); }
            p25_p2_build_mac(o, slot, isch, &s.tx_scr, &m);
        } else {
            p25_mac_idle(&m, slot == 10);
            p25_p2_build_mac(o, slot, isch, slot == 10 ? NULL : &s.tx_scr, &m);
        }
        txq_put(&s.q, o, P25_P2_SLOT);
    }
    s.tx_sf++;
    if(last) { s.tx_phase = PTX_DONE; }
}

static size_t p25_tx_symbols(void *ctx, uint8_t *sym, size_t max)
{
    (void)ctx;
    size_t n = 0;
    while(n < max) {
        if(txq_empty(&s.q)) {
            if(s.tx_phase == PTX_DONE) { break; }
            if(s.is_p2) { p2_build(); } else { p1_build(); }
        }
        n += txq_take(&s.q, sym + n, max - n);
    }
    return n;
}

static void p25_tx_stop(void *ctx) { (void)ctx; s.tx_stop = true; }

static size_t p25_status(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    sbuf_t b = sb_init(buf, len);
    if(s.seen_call) {
        sb_s(&b, "TG"); sb_u(&b, s.ci.dst_id);
        if(s.ci.src_id) { sb_s(&b, " < "); sb_u(&b, s.ci.src_id); }
        if(s.ci.slot) { sb_s(&b, " S"); sb_u(&b, s.ci.slot); }
        if(s.ci.encrypted) { sb_s(&b, !s.is_p2 && s.p1.vc == P25_VC_DECRYPTED ? " ENC key ok" : !s.is_p2 && s.p1.vc == P25_VC_NOKEY ? " ENC no key" : " ENC"); }
    } else {
        sb_s(&b, "P25 no call");
    }
    sb_c(&b, '\n');
    if(s.trunking && s.trunk_line[0]) { sb_s(&b, s.trunk_line); }
    else {
        if(s.frames && !s.is_p2) { sb_s(&b, "NAC "); sb_x(&b, s.nac, 3); sb_c(&b, ' '); }
        else if(s.frames) { sb_s(&b, "P2 WACN "); sb_x(&b, s.cfg.wacn, 5); sb_c(&b, ' '); }
        sb_s(&b, "v "); sb_u(&b, s.voice);
        if(s.tsbk) { sb_s(&b, " tsbk "); sb_u(&b, s.tsbk); }
    }
    return sb_len(&b, buf);
}

static const cfw_proto_t k_p25p1 = {
    .name = "P25",
    .mode = CFW_MODE_P25P1,
    .air = { CFW_MOD_4FSK, 4800, 2, 1800, 12500, false },
    .ctx = &s,
    .init = p25_init,
    .rx_reset = p25_reset,
    .rx_symbols = p1_symbols,
    .tx_start = p25_tx_start,
    .tx_symbols = p25_tx_symbols,
    .tx_stop = p25_tx_stop,
    .tick = p1_tick,
    .status = p25_status,
    .configure = p1_configure,
};
CFW_PROTO_REGISTER(k_p25p1);

static const cfw_proto_t k_p25p2 = {
    .name = "P25P2",
    .mode = CFW_MODE_P25P2,
    .air = { CFW_MOD_HDQPSK, 6000, 2, 0, 12500, true },
    .ctx = &s,
    .init = p25_init,
    .rx_reset = p25_reset,
    .rx_symbols = p2_symbols,
    .tx_start = p25_tx_start,
    .tx_symbols = p25_tx_symbols,
    .tx_stop = p25_tx_stop,
    .status = p25_status,
    .configure = p2_configure,
};
CFW_PROTO_REGISTER(k_p25p2);
