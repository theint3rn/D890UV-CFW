/* Emulator test glue (built only with `make EMU=1`, and only while proto/dmr_nxdn has no cfw.mk of its own):
 * a receive-only DMR proto around proto/dmr_nxdn's dmr_rx. With the emu-syms backend (hal/emu_syms.c) it
 * decodes a dibit file played by the Renode symbol FIFO (`syms Load @emu/streams/dmr_call.dibits 4800`)
 * and shows the call on the main screen. It is a test harness, not the product glue (that belongs to the
 * proto/dmr_nxdn owners): no TX, no vocoder, no trunking. */
#include <string.h>
#include "modem.h"
#include "dmr.h"

typedef struct {
    const cfw_platform_t *pf;
    dmr_rx_t rx;
    cfw_call_info_t ci;
    uint32_t frames, calls, csbk;
    bool seen_call;
} emu_dmr_t;

static emu_dmr_t s_ctx;

static void on_voice(void *user, uint8_t slot, const uint8_t frame[9], uint8_t idx, uint8_t crypt)
{
    emu_dmr_t *c = user;
    (void)slot; (void)idx; (void)crypt;
    c->frames++;
    if(c->pf && c->pf->voice_rx) { c->pf->voice_rx(CFW_VOC_AMBE2_49, frame, 72, 0); }
}

static void on_event(void *user, const dmr_event_t *ev)
{
    emu_dmr_t *c = user;
    switch(ev->type) {
    case DMR_EV_VOICE_START:
        c->calls++;
        c->seen_call = true;
        c->ci.src_id = ev->lc.src;
        c->ci.dst_id = ev->lc.dst;
        c->ci.slot = (uint8_t)(ev->slot + 1);
        c->ci.color_code = ev->cc;
        c->ci.voice = true;
        break;
    case DMR_EV_VOICE_END:
        c->ci.voice = false;
        break;
    case DMR_EV_CSBK:
        c->csbk++;
        return;
    default:
        return;
    }
    if(c->pf && c->pf->call_info) { c->pf->call_info(&c->ci); }
}

static int dmr_init(void *ctx, const cfw_platform_t *pf) { ((emu_dmr_t *)ctx)->pf = pf; return 0; }

static void dmr_reset(void *ctx)
{
    emu_dmr_t *c = ctx;
    dmr_rx_cfg_t cfg = { 4, -1, true };            /* sync errors, any colour code, repeater output (CACH) */
    dmr_cb_t cb = { on_voice, on_event, c };
    dmr_rx_init(&c->rx, &cfg, &cb);
}

static void dmr_symbols(void *ctx, const uint8_t *sym, const int8_t *soft, size_t n)
{
    (void)soft;
    dmr_rx_dibits(&((emu_dmr_t *)ctx)->rx, sym, n);
}

static int dmr_tx_start(void *ctx, const cfw_tx_req_t *req) { (void)ctx; (void)req; return -1; }

static char *put_s(char *p, char *end, const char *s) { while(*s && p < end) { *p++ = *s++; } return p; }
static char *put_u(char *p, char *end, uint32_t v)
{
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while(v);
    while(n && p < end) { *p++ = t[--n]; }
    return p;
}

/* "TG91<9990201 TS1 V36" (V = voice frames) or "DMR rx: no call" */
static size_t dmr_status(void *ctx, char *buf, size_t len)
{
    emu_dmr_t *c = ctx;
    if(!len) { return 0; }
    char *p = buf, *end = buf + len - 1;
    if(!c->seen_call) {
        p = put_s(p, end, "DMR rx: no call");
    } else {
        p = put_s(p, end, "TG"); p = put_u(p, end, c->ci.dst_id);
        p = put_s(p, end, "<"); p = put_u(p, end, c->ci.src_id);
        p = put_s(p, end, " TS"); p = put_u(p, end, c->ci.slot);
        p = put_s(p, end, c->ci.voice ? " V" : " end V"); p = put_u(p, end, c->frames);
    }
    *p = 0;
    return (size_t)(p - buf);
}

static const cfw_proto_t k_emu_dmr = {
    .name = "DMRemu",
    .mode = CFW_MODE_DMR,
    .air = { CFW_MOD_4FSK, 4800, 2, 1944, 12500, true },
    .ctx = &s_ctx,
    .init = dmr_init,
    .rx_reset = dmr_reset,
    .rx_symbols = dmr_symbols,
    .tx_start = dmr_tx_start,
    .status = dmr_status,
};
CFW_PROTO_REGISTER(k_emu_dmr);
