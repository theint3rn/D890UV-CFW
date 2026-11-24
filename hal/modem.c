#include <string.h>
#include "modem_core.h"
#include "hal.h"
#include "voice.h"

extern const cfw_proto_t *const __proto_table_start[];
extern const cfw_proto_t *const __proto_table_end[];

#define MAX_BACKENDS 4
#define MAX_VOCODERS 4

static const cfw_modem_backend_t *s_backends[MAX_BACKENDS];
static int s_nbackends;
static const cfw_vocoder_ops_t *s_vocoders[MAX_VOCODERS];
static int s_nvocoders;

static cfw_mode_t s_mode = CFW_MODE_FM;
static const cfw_proto_t *s_proto;
static const cfw_modem_backend_t *s_backend;
static bool s_tx;
static cfw_call_info_t s_call;
static uint32_t s_voice_frames, s_rx_syms;
static uint32_t s_rx_hz, s_tx_hz;

volatile bool g_modem_tx_request;
volatile uint32_t g_modem_tune_rx, g_modem_tune_tx;

static const char *const k_mode_names[CFW_MODE_COUNT] = {
    "FM", "DMR", "P25", "P25P2", "DSTAR", "NXDN", "YSF",
};

const char *modem_mode_name(cfw_mode_t m) { return (unsigned)m < CFW_MODE_COUNT ? k_mode_names[m] : "?"; }

/* ---- platform services handed to protos ---- */
static uint32_t pf_now(void) { return hal_millis(); }
static void pf_log(cfw_log_level_t lvl, const char *msg) { (void)lvl; (void)msg; }   /* TODO: UART log */
static void pf_voice_rx(cfw_vocoder_t voc, const uint8_t *bits, size_t nbits, int errs)
{
    s_voice_frames++;
    voice_rx_frame(voc, bits, nbits, errs);
}
static size_t pf_voice_tx(cfw_vocoder_t voc, uint8_t *bits, size_t nbits) { return voice_tx_frame(voc, bits, nbits); }
/* data from a proto (type 1 = DMR SMS: src u24 BE, dst u24 BE, u8 group, text): the app overrides this hook */
__attribute__((weak)) void modem_data_hook(uint8_t type, const uint8_t *d, size_t n) { (void)type; (void)d; (void)n; }
static void pf_data_rx(uint8_t type, const uint8_t *d, size_t n) { modem_data_hook(type, d, n); }
static void pf_call_info(const cfw_call_info_t *ci) { s_call = *ci; }
static bool pf_tune(uint32_t rx, uint32_t tx) { g_modem_tune_rx = rx; g_modem_tune_tx = tx; return true; }
static bool pf_request_tx(void) { g_modem_tx_request = true; return true; }

static const cfw_platform_t k_platform = {
    .api_version = CFW_MODEM_API_VERSION,
    .now_ms = pf_now,
    .log = pf_log,
    .voice_rx = pf_voice_rx,
    .data_rx = pf_data_rx,
    .call_info = pf_call_info,
    .tune = pf_tune,
    .request_tx = pf_request_tx,
    .codeplug_read = nand_read,
    .voice_tx = pf_voice_tx,
};

/* ---- registry ---- */
void modem_register_backend(const cfw_modem_backend_t *b)
{
    if(s_nbackends < MAX_BACKENDS) { s_backends[s_nbackends++] = b; }
}

void modem_register_vocoder(const cfw_vocoder_ops_t *v)
{
    if(s_nvocoders < MAX_VOCODERS) { s_vocoders[s_nvocoders++] = v; }
}

int modem_proto_count(void) { return (int)(__proto_table_end - __proto_table_start); }
const cfw_proto_t *modem_proto_at(int i) { return __proto_table_start[i]; }

const cfw_proto_t *modem_find(cfw_mode_t mode)
{
    for(int i = 0; i < modem_proto_count(); i++) {
        if(__proto_table_start[i]->mode == mode) { return __proto_table_start[i]; }
    }
    return NULL;
}

void modem_init(void)
{
    modem_register_backend(&g_backend_sct3288);
#ifdef CFW_LOOPBACK
    modem_register_backend(&g_backend_loopback);
#endif
#ifdef CFW_EMU
    modem_register_backend(&g_backend_emusyms);           /* Renode symbol FIFO, see hal/emu_syms.c */
#endif
    modem_register_backend(&g_backend_mcudsp);            /* ADC1 discriminator + cfw/dsp demodulators */
    for(int i = 0; i < modem_proto_count(); i++) {
        const cfw_proto_t *p = __proto_table_start[i];
        if(p->init) { p->init(p->ctx, &k_platform); }
    }
}

static void leave(void)
{
    if(s_tx && s_proto && s_proto->tx_stop) { s_proto->tx_stop(s_proto->ctx); }
    s_tx = false;
    if(s_backend) { s_backend->stop(); }
    s_backend = NULL;
    s_proto = NULL;
}

static cfw_chan_cfg_t s_cfg;
static modem_avail_t s_avail = MODEM_OK;

bool modem_set_mode(cfw_mode_t mode, const cfw_chan_cfg_t *cfg)
{
    leave();
    s_mode = mode;
    s_cfg = *cfg;
    s_rx_hz = cfg->rx_hz; s_tx_hz = cfg->tx_hz;
    memset(&s_call, 0, sizeof s_call);
    s_voice_frames = 0; s_rx_syms = 0;
    s_avail = MODEM_OK;
    if(mode == CFW_MODE_FM) { return true; }
    const cfw_proto_t *p = modem_find(mode);
    if(!p) { s_avail = MODEM_NO_PROTO; return false; }
    for(int i = 0; i < s_nbackends; i++) {
        if(s_backends[i]->supports(&p->air)) { s_backend = s_backends[i]; break; }
    }
    if(!s_backend) { s_avail = MODEM_NO_DEMOD; return false; }
    s_proto = p;
    if(p->rx_reset) { p->rx_reset(p->ctx); }
    if(p->configure) { p->configure(p->ctx, &s_cfg); }
    s_backend->start_rx(&p->air, s_rx_hz);
    return true;
}

void modem_configure(const cfw_chan_cfg_t *cfg)
{
    s_cfg = *cfg;
    if(s_proto && s_proto->configure) { s_proto->configure(s_proto->ctx, &s_cfg); }
}

modem_avail_t modem_availability(void) { return s_avail; }
uint32_t modem_rx_hz(void) { return s_proto ? s_rx_hz : 0; }

const char *modem_backend_for(cfw_mode_t mode)
{
    const cfw_proto_t *p = modem_find(mode);
    if(!p) { return NULL; }
    for(int i = 0; i < s_nbackends; i++) {
        if(s_backends[i]->supports(&p->air)) { return s_backends[i]->name; }
    }
    return NULL;
}

cfw_mode_t modem_mode(void) { return s_mode; }
bool modem_active(void) { return s_proto != NULL; }
const char *modem_backend_name(void) { return s_backend ? s_backend->name : "-"; }
bool modem_transmitting(void) { return s_tx; }
const cfw_call_info_t *modem_call_info(void) { return &s_call; }
uint32_t modem_voice_frames(void) { return s_voice_frames; }
uint32_t modem_rx_symbol_count(void) { return s_rx_syms; }

void modem_ptt(bool down, const cfw_tx_req_t *req)
{
    if(!s_proto) { return; }
    if(down && !s_tx) {
        cfw_tx_req_t r = { .voice = true };
        if(req) { r = *req; }
        if(s_proto->tx_start && s_proto->tx_start(s_proto->ctx, &r) == 0) {
            s_tx = true;
            s_backend->start_tx(&s_proto->air, s_tx_hz);
        }
    } else if(!down && s_tx) {
        if(s_proto->tx_stop) { s_proto->tx_stop(s_proto->ctx); }
        /* stays in TX until tx_symbols returns 0 (terminator sent) */
    }
}

void modem_rx_push(const uint8_t *sym, const int8_t *soft, size_t n)
{
    if(!s_proto || !s_proto->rx_symbols) { return; }
    s_rx_syms += (uint32_t)n;
    while(n) {
        size_t k = n > 64 ? 64 : n;
        s_proto->rx_symbols(s_proto->ctx, sym, soft, k);
        sym += k;
        if(soft) { soft += k; }
        n -= k;
    }
}

size_t modem_tx_pull(uint8_t *sym, size_t max)
{
    if(!s_tx || !s_proto || !s_proto->tx_symbols) { return 0; }
    size_t n = s_proto->tx_symbols(s_proto->ctx, sym, max);
    if(n == 0) { s_tx = false; }      /* the backend finishes its modulator/DAC tail, then modem_tx_done() */
    return n;
}

void modem_poll(void)
{
    if(g_modem_tune_rx || g_modem_tune_tx) {
        if(g_modem_tune_rx) { s_rx_hz = g_modem_tune_rx; }
        if(g_modem_tune_tx) { s_tx_hz = g_modem_tune_tx; }
        g_modem_tune_rx = g_modem_tune_tx = 0;
        if(s_backend && !s_tx) { s_backend->start_rx(&s_proto->air, s_rx_hz); }
        /* no rx_reset here: the proto asked for the retune (trunking) and keeps its own state */
    }
    if(g_modem_tx_request) {
        g_modem_tx_request = false;
        cfw_tx_req_t r = { .voice = false };
        modem_ptt(true, &r);
    }
    if(s_backend) { s_backend->poll(); }
}

void modem_tick(void)
{
    if(s_proto && s_proto->tick) { s_proto->tick(s_proto->ctx, hal_millis()); }
}

void modem_tx_done(void)
{
    if(s_backend && s_proto) { s_backend->start_rx(&s_proto->air, s_rx_hz); }
}

size_t modem_status(char *buf, size_t len)
{
    if(len) { buf[0] = 0; }
    if(s_proto && s_proto->status) { return s_proto->status(s_proto->ctx, buf, len); }
    return 0;
}

/* ================= backends ================= */

/* SCT3288: the baseband demodulates 4FSK and runs AMBE+2 for DMR in the stock firmware. Its raw-symbol
 * / vocoder message IDs on the SPI2 link are not decoded yet, so this backend reports no support and
 * the mode shows "no modem" until cfw/docs + the baseband work define them. */
static bool sct_supports(const cfw_air_t *a) { (void)a; return false; }
static void sct_start_rx(const cfw_air_t *a, uint32_t hz) { (void)a; rf_set_freq(0, hz); }
static void sct_start_tx(const cfw_air_t *a, uint32_t hz) { (void)a; rf_set_freq(0, hz); }
static void sct_stop(void) { }
static void sct_poll(void) { }
const cfw_modem_backend_t g_backend_sct3288 = {
    "SCT3288", sct_supports, sct_start_rx, sct_start_tx, sct_stop, sct_poll,
};

/* Loopback: symbols the proto transmits are fed straight back to its receiver at the symbol rate.
 * No RF. Lets a proto be exercised end to end in the emulator (build with LOOPBACK=1). */
static uint32_t lb_rate, lb_last_ms;
static bool lb_tx;
static bool lb_supports(const cfw_air_t *a) { (void)a; return true; }
static void lb_start_rx(const cfw_air_t *a, uint32_t hz) { (void)hz; lb_rate = a->symbol_rate; lb_tx = false; }
static void lb_start_tx(const cfw_air_t *a, uint32_t hz) { (void)hz; lb_rate = a->symbol_rate; lb_tx = true; lb_last_ms = hal_millis(); }
static void lb_stop(void) { lb_tx = false; }
static void lb_poll(void)
{
    if(!lb_tx) { return; }   /* (loopback has no tail: RX continues by itself) */
    uint32_t now = hal_millis();
    uint32_t due = (now - lb_last_ms) * lb_rate / 1000u;
    if(!due) { return; }
    lb_last_ms = now;
    uint8_t buf[128];
    while(due) {
        size_t want = due > sizeof buf ? sizeof buf : due;
        size_t n = modem_tx_pull(buf, want);
        if(!n) { lb_tx = false; return; }
        modem_rx_push(buf, NULL, n);
        due -= (uint32_t)n;
    }
}
const cfw_modem_backend_t g_backend_loopback = {
    "loopback", lb_supports, lb_start_rx, lb_start_tx, lb_stop, lb_poll,
};
