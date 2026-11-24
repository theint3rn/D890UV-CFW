/* D-STAR DV glue (GMSK 4800 bit/s, one bit per symbol): proto/dstar_ysf dstar_rx. Receive only. */
#include "glue.h"
#include "dstar.h"

typedef struct {
    const cfw_platform_t *pf;
    cfw_chan_cfg_t cfg;
    dstar_rx_t rx;
    dstar_header_t hdr;
    char text[DSTAR_TEXT_LEN + 1];
    cfw_call_info_t ci;
    uint32_t voice;
    bool have_hdr, inited, active;
    /* TX */
    txq_t q;
    dstar_tx_t tx;
    uint8_t tx_phase;
    bool tx_stop;
} g_dstar_t;

enum { STX_HDR, STX_VOICE, STX_END, STX_DONE };

static g_dstar_t s;

static void on_voice(void *user, const uint8_t ambe[DSTAR_VOICE_BYTES], uint8_t seq)
{
    (void)user; (void)seq;
    s.voice++;
    if(s.pf->voice_rx) { s.pf->voice_rx(CFW_VOC_AMBE_DSTAR, ambe, 72, 0); }  /* test codec: 49 bits lead */
}

static void on_event(void *user, const dstar_event_t *ev)
{
    (void)user;
    switch(ev->type) {
    case DSTAR_EV_HEADER:
        s.hdr = ev->u.header.hdr;
        s.have_hdr = true;
        s.active = true;
        memset(&s.ci, 0, sizeof s.ci);
        memcpy(s.ci.src_call, s.hdr.my, 8);
        memcpy(s.ci.dst_call, s.hdr.ur, 8);
        s.ci.voice = true;
        break;
    case DSTAR_EV_LATE_ENTRY: s.active = true; s.ci.voice = true; break;
    case DSTAR_EV_TEXT: memcpy(s.text, ev->u.text, sizeof s.text); return;
    case DSTAR_EV_END: case DSTAR_EV_LOST: s.active = false; s.ci.voice = false; break;
    default: return;
    }
    if(s.pf->call_info) { s.pf->call_info(&s.ci); }
}

static const dstar_rx_cb_t k_cb = { on_voice, on_event };

static int ds_init(void *ctx, const cfw_platform_t *pf) { (void)ctx; s.pf = pf; return 0; }

static void ds_reset(void *ctx)
{
    (void)ctx;
    memset(&s.ci, 0, sizeof s.ci);
    s.voice = 0;
    s.have_hdr = s.active = false;
    s.text[0] = 0;
    dstar_rx_init(&s.rx, &k_cb, NULL);
    s.inited = true;
}

static void ds_configure(void *ctx, const cfw_chan_cfg_t *cfg) { (void)ctx; s.cfg = *cfg; }

static void ds_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)ctx; (void)soft;
    if(s.inited) { dstar_rx_push_bits(&s.rx, sym, n); }
}

static int ds_tx_start(void *ctx, const cfw_tx_req_t *req)
{
    (void)ctx; (void)req;
    if(!s.pf->voice_tx) { return -1; }
    dstar_header_t h;
    memset(&h, 0, sizeof h);
    char my[9], ur[9], rpt[9];
    strncpy(my, s.cfg.my_call[0] ? s.cfg.my_call : "N0CALL", 8); my[8] = 0;
    strncpy(ur, s.cfg.ur[0] ? s.cfg.ur : "CQCQCQ", 8); ur[8] = 0;
    strncpy(rpt, s.cfg.rpt1, 8); rpt[8] = 0;
    bool via_rpt = !s.cfg.direct && rpt[0] && rpt[0] != ' ' && strncmp(rpt, "DIRECT", 6) != 0;
    if(via_rpt) {     /* repeater: RPT1 = repeater + module, RPT2 = gateway when the channel's RPT2 ends in G */
        bool gw = s.cfg.rpt2[7] == 'G';
        dstar_header_repeater(&h, my, "CFW", ur, rpt, gw);
    } else {          /* simplex / talkaround: RPT1 = RPT2 = DIRECT, flag1 = 0 */
        dstar_header_simplex(&h, my, "CFW", ur);
    }
    dstar_tx_init(&s.tx, &h);
    dstar_tx_set_text(&s.tx, "cfw TX test         ");
    txq_reset(&s.q);
    s.tx_phase = STX_HDR;
    s.tx_stop = false;
    return 0;
}

static void ds_build(void)
{
    static uint8_t bits[1024];
    switch(s.tx_phase) {
    case STX_HDR:
        txq_put(&s.q, bits, dstar_tx_header(&s.tx, 64, bits));
        s.tx_phase = STX_VOICE;
        break;
    case STX_VOICE: {
        uint8_t ambe[DSTAR_VOICE_BYTES];
        memset(ambe, 0, sizeof ambe);
        s.pf->voice_tx(CFW_VOC_AMBE_DSTAR, ambe, 72);   /* SCT3288: AMBE 2400 frame incl. FEC; sine test codec: 49 bits lead */
        txq_put(&s.q, bits, dstar_tx_frame(&s.tx, ambe, bits));
        if(s.tx_stop) { s.tx_phase = STX_END; }
        break;
    }
    case STX_END:
        txq_put(&s.q, bits, dstar_tx_end(&s.tx, bits));
        s.tx_phase = STX_DONE;
        break;
    default:
        break;
    }
}

static size_t ds_tx_symbols(void *ctx, uint8_t *sym, size_t max)
{
    (void)ctx;
    size_t n = 0;
    while(n < max) {
        if(txq_empty(&s.q)) {
            if(s.tx_phase == STX_DONE) { break; }
            ds_build();
        }
        n += txq_take(&s.q, sym + n, max - n);
    }
    return n;
}

static void ds_tx_stop(void *ctx) { (void)ctx; s.tx_stop = true; }

static size_t ds_status(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    sbuf_t b = sb_init(buf, len);
    if(s.have_hdr) {
        sb_sn(&b, s.hdr.my, 8);
        if(s.hdr.my_sfx[0] && s.hdr.my_sfx[0] != ' ') { sb_c(&b, '/'); sb_sn(&b, s.hdr.my_sfx, 4); }
        sb_s(&b, " > "); sb_sn(&b, s.hdr.ur, 8);
        sb_c(&b, '\n');
        if(s.text[0]) { sb_sn(&b, s.text, DSTAR_TEXT_LEN); }
        else { sb_s(&b, "R1 "); sb_sn(&b, s.hdr.rpt1, 8); sb_s(&b, " R2 "); sb_sn(&b, s.hdr.rpt2, 8); }
    } else {
        sb_s(&b, s.active ? "DSTAR late entry" : "DSTAR no call");
        sb_c(&b, '\n');
        sb_s(&b, "UR "); sb_sn(&b, s.cfg.ur, 8);
    }
    sb_s(&b, "\nvoice "); sb_u(&b, s.voice);
    return sb_len(&b, buf);
}

static const cfw_proto_t k_dstar = {
    .name = "DSTAR",
    .mode = CFW_MODE_DSTAR,
    .air = { CFW_MOD_GMSK, 4800, 1, 1200, 12500, false },
    .ctx = &s,
    .init = ds_init,
    .rx_reset = ds_reset,
    .rx_symbols = ds_symbols,
    .tx_start = ds_tx_start,
    .tx_symbols = ds_tx_symbols,
    .tx_stop = ds_tx_stop,
    .status = ds_status,
    .configure = ds_configure,
};
CFW_PROTO_REGISTER(k_dstar);
