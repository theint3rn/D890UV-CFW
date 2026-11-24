/* YSF (System Fusion C4FM, 4800 sym/s) glue: proto/dstar_ysf ysf_rx. Receive only. DG-ID filter:
 * channel DG-ID 0 = open (everything), else only calls with that DG-ID are shown. */
#include "glue.h"
#include "p25_p1.h"
#include "ysf.h"
#include "ambe_fec.h"

typedef struct {
    const cfw_platform_t *pf;
    cfw_chan_cfg_t cfg;
    ysf_rx_t rx;
    ysf_csd_t csd;
    cfw_call_info_t ci;
    uint32_t voice, frames, filtered;
    uint8_t dgid;
    bool have_csd, inited, active, passed;
    /* TX (V/D mode 2) */
    txq_t q;
    ysf_tx_t tx;
    uint8_t tx_phase;
    bool tx_stop;
} g_ysf_t;

enum { YTX_HDR, YTX_VOICE, YTX_TERM, YTX_DONE };

static g_ysf_t s;

static bool dgid_ok(uint8_t dgid) { return s.cfg.dgid == 0 || dgid == s.cfg.dgid; }

static void on_voice(void *user, ysf_voice_kind_t kind, const uint8_t *frame, unsigned idx, const ysf_fich_t *f)
{
    (void)user; (void)idx;
    if(!dgid_ok(ysf_fich_dgid(f))) { s.filtered++; return; }
    s.voice++;
    if(!s.pf->voice_rx) { return; }
    if(kind == YSF_VOICE_FR) {                          /* VW: IMBE 144 bits with P25 FEC + interleave */
        uint8_t u[11];
        int e = p25_imbe_decode(frame, u);
        s.pf->voice_rx(CFW_VOC_IMBE_88, u, 88, e);
        return;
    }
    if(kind == YSF_VOICE_VD1) {
        uint8_t d49[7];
        int e = ambe72_decode(frame, d49);
        s.pf->voice_rx(CFW_VOC_AMBE2_49, d49, 49, e);
    } else {
        s.pf->voice_rx(CFW_VOC_AMBE2_49, frame, 49, 0);
    }
}

static void on_event(void *user, const ysf_event_t *ev)
{
    (void)user;
    switch(ev->type) {
    case YSF_EV_FICH:
        s.frames++;
        if(ev->fich_ok) { s.dgid = ysf_fich_dgid(&ev->fich); s.passed = dgid_ok(s.dgid); }
        return;
    case YSF_EV_HEADER:
    case YSF_EV_CSD:
        if(!dgid_ok(ysf_fich_dgid(&ev->fich))) { return; }
        s.csd = ev->csd;
        s.have_csd = true;
        s.active = true;
        memset(&s.ci, 0, sizeof s.ci);
        memcpy(s.ci.src_call, s.csd.src, 10 < sizeof s.ci.src_call ? 10 : sizeof s.ci.src_call - 1);
        memcpy(s.ci.dst_call, s.csd.dest, 10 < sizeof s.ci.dst_call ? 10 : sizeof s.ci.dst_call - 1);
        s.ci.voice = true;
        break;
    case YSF_EV_TERMINATOR:
    case YSF_EV_LOST:
        s.active = false;
        s.ci.voice = false;
        break;
    default:
        return;
    }
    if(s.pf->call_info) { s.pf->call_info(&s.ci); }
}

static const ysf_rx_cb_t k_cb = { on_voice, on_event };

static int ys_init(void *ctx, const cfw_platform_t *pf) { (void)ctx; s.pf = pf; return 0; }

static void ys_reset(void *ctx)
{
    (void)ctx;
    memset(&s.ci, 0, sizeof s.ci);
    s.voice = s.frames = s.filtered = 0;
    s.have_csd = s.active = false;
    ysf_rx_init(&s.rx, &k_cb, NULL);
    s.inited = true;
}

static void ys_configure(void *ctx, const cfw_chan_cfg_t *cfg) { (void)ctx; s.cfg = *cfg; }

static void ys_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    if(s.inited) { ysf_rx_push_dibits(&s.rx, sym, n); }
}

static int ys_tx_start(void *ctx, const cfw_tx_req_t *req)
{
    (void)ctx; (void)req;
    if(!s.pf->voice_tx) { return -1; }
    ysf_csd_t csd;
    memset(&csd, 0, sizeof csd);
    call_field(csd.dest, "ALL", 10);
    call_field(csd.src, s.cfg.my_call[0] ? s.cfg.my_call : "N0CALL", 10);
    call_field(csd.down, "", 10);
    call_field(csd.up, "", 10);
    csd.valid = 0x0F;
    ysf_tx_init(&s.tx, YSF_DT_VD2, &csd, s.cfg.dgid_tx ? (uint8_t)(s.cfg.dgid_tx - 1) : s.cfg.dgid);
    s.tx.base.mr = YSF_MR_DIRECT;   /* MR = direct from a mobile; a repeater rewrites it on its downlink */
    s.tx.base.dev = 0;
    txq_reset(&s.q);
    s.tx_phase = YTX_HDR;
    s.tx_stop = false;
    return 0;
}

static void ys_build(void)
{
    static uint8_t d[YSF_FRAME_DIBITS];
    switch(s.tx_phase) {
    case YTX_HDR:
        ysf_tx_header(&s.tx, d);
        txq_put(&s.q, d, sizeof d);
        s.tx_phase = YTX_VOICE;
        break;
    case YTX_VOICE: {
        uint8_t v[5 * 7];
        memset(v, 0, sizeof v);
        for(int i = 0; i < 5; i++) { s.pf->voice_tx(CFW_VOC_AMBE2_49, v + 7 * i, 49); }
        ysf_tx_voice(&s.tx, v, d);
        txq_put(&s.q, d, sizeof d);
        if(s.tx_stop) { s.tx_phase = YTX_TERM; }
        break;
    }
    case YTX_TERM:
        ysf_tx_terminator(&s.tx, d);
        txq_put(&s.q, d, sizeof d);
        s.tx_phase = YTX_DONE;
        break;
    default:
        break;
    }
}

static size_t ys_tx_symbols(void *ctx, uint8_t *sym, size_t max)
{
    (void)ctx;
    size_t n = 0;
    while(n < max) {
        if(txq_empty(&s.q)) {
            if(s.tx_phase == YTX_DONE) { break; }
            ys_build();
        }
        n += txq_take(&s.q, sym + n, max - n);
    }
    return n;
}

static void ys_tx_stop(void *ctx) { (void)ctx; s.tx_stop = true; }

static size_t ys_status(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    sbuf_t b = sb_init(buf, len);
    if(s.have_csd) {
        sb_sn(&b, s.csd.src, 10); sb_s(&b, " > "); sb_sn(&b, s.csd.dest, 10);
    } else {
        sb_s(&b, "YSF no call");
    }
    sb_s(&b, "\nDG-ID "); sb_u(&b, s.frames ? s.dgid : s.cfg.dgid);
    if(s.cfg.dgid) { sb_s(&b, " (want "); sb_u(&b, s.cfg.dgid); sb_c(&b, ')'); }
    if(s.frames && !s.passed) { sb_s(&b, " muted"); }
    sb_s(&b, "\nvoice "); sb_u(&b, s.voice);
    return sb_len(&b, buf);
}

static const cfw_proto_t k_ysf = {
    .name = "YSF",
    .mode = CFW_MODE_YSF,
    .air = { CFW_MOD_4FSK, 4800, 2, 2700, 12500, false },
    .ctx = &s,
    .init = ys_init,
    .rx_reset = ys_reset,
    .rx_symbols = ys_symbols,
    .tx_start = ys_tx_start,
    .tx_symbols = ys_tx_symbols,
    .tx_stop = ys_tx_stop,
    .status = ys_status,
    .configure = ys_configure,
};
CFW_PROTO_REGISTER(k_ysf);
