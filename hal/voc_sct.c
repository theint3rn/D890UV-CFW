/* SCT3288 voice backend. Evidence and the mode table: docs/SCT3288_VOCODER.md.
 *
 * The codec (microphone, speaker) hangs on the SCT3288. The stock firmwares record, play back and encrypt voice with
 *   WORK_MODE 18 03 03 = "vocoder loop" (codec <-> vocoder, no RF)  +  VOCODER_IO_SET 3E <bits>
 *   bits (stock table 0x08150FC8 / D168UV 0x080E2F14, and what its handlers do):
 *     01 mic PCM -> MCU        02 decoded PCM -> MCU     04 demodulated frames -> MCU   08 MCU frames -> decoder
 *     10 encoder frames -> MCU 20 same, taken out of TX  40 MCU frames -> modulator     80 MCU PCM -> chip
 *   channel frames: ID 01, {24, 36 bytes} = 4 x 9 bytes (80 ms); an ID 01 answer without data = "send the next ones"
 *   PCM:            ID 00, {00 A0, 160 x s16 big endian};        an ID 00 answer 00        = "send the next ones"
 *   frame format:   ID 01, {FF, 00} = 72 bits with FEC (record / play), {FF, 06} = 49 bits (stock encryption path)
 * Nothing here transmits: no WORK_MODE TX, no call commands (hal/bb_allow.h refuses them).
 *
 * Probe (once, after the baseband is up): version, VOCODER_SEL, PROCESS_MODE reads, VOCODER_IO_SET 00, external
 * PRODID. No answer = that mode is not used; nothing waits in a loop. The emulator answers (models/Baseband*.cs) are a
 * stand-in: they prove framing and flow, not what the real chip does. */
#include <string.h>
#include "voc_sct.h"
#include "hal.h"
#include "dbg.h"
#include "modem_core.h"

#ifdef CFW_HW_BUILD
volatile uint8_t g_sct_tap;
#else
volatile uint8_t g_sct_tap = 1;
#endif
volatile uint8_t g_sct_force;

/* ---- command sequences ---- */
typedef struct { uint8_t b0, id, len, must; uint8_t d[3]; } sct_cmd_t;
#define SEQ_MAX 14
#define ACK_MS  200u                              /* stock FUN_0810c3b0(.., 200) */
static sct_cmd_t s_seq[SEQ_MAX];
static uint8_t s_seq_n, s_seq_i, s_try;
static bool s_wait, s_acked, s_seq_fail;
static uint32_t s_seq_t0;
static uint8_t s_ack_d[8], s_ack_len, s_ack_b0;

static sct_chan_cb s_chan_cb;
static sct_pcm_cb s_pcm_cb;
static bool s_want_chan, s_want_pcm, s_raw49;
static uint8_t s_fec_bad, s_raw_hits;

/* ---- probe result ---- */
enum { PR_IDLE, PR_RUN, PR_DONE };
static uint8_t s_pr_state;
static uint32_t s_pr_t0;
static bool s_have_io, s_have_ext;
static char s_prodid[24];

/* ---- session ---- */
static sct_sess_t s_req, s_cur;                   /* wanted / set up */
static bool s_up;                                 /* s_cur is running (sequence finished) */
static bool s_closing;
static uint32_t s_up_ms, s_data_ms, s_put_ms;     /* session start, last data from the chip, last packet to it */
static bool s_fed;
static uint8_t s_retry;
static uint8_t s_bad, s_bad1;                     /* bit per sct_sess_t: did not work (twice) / once */
#define STALL_MS 800u

/* ---- external AMBE ---- */
static sct_async_cb s_acb;
static bool s_async_busy;
static uint32_t s_async_t0;
static uint8_t s_want_b0, s_want_id;
static int s_rate, s_rate_req;

#ifdef CFW_DEBUG
static void bench_rx(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len);
#endif

static void listener(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len)
{
#ifdef CFW_DEBUG
    bench_rx(b0, id, d, len);
#endif
    if(b0 & 0x80) {                                                    /* external AMBE packets */
        if(b0 == 0x80 && id == 0x30 && !s_have_ext) {
            uint16_t n = len < sizeof s_prodid - 1 ? len : (uint16_t)(sizeof s_prodid - 1);
            memcpy(s_prodid, d, n);
            s_prodid[n] = 0;
            s_have_ext = true;
        }
        if(s_wait && s_seq[s_seq_i].b0 == 0x80 && s_seq[s_seq_i].id == id) { s_acked = true; s_ack_len = 0; return; }
        if(s_async_busy && b0 == s_want_b0 && id == s_want_id) {
            s_async_busy = false;
            if(b0 == 0x80) { s_rate = (len && d[0] == 0) ? s_rate_req : SCT_RATE_NONE; }
            else if(s_acb) { s_acb(b0, d, len); }
        }
        return;
    }
    if(b0 != 0x40 && id == 0x01 && len >= 28 && (d[0] == 0x24 || d[0] == 0x1B) && len > d[0]) {
        s_data_ms = hal_millis();
        if(s_up && s_chan_cb) { s_chan_cb(d + 1, d[0] / 9u); }
        return;
    }
    if(b0 != 0x40 && id == 0x00 && len >= 322 && d[0] == 0x00 && d[1] == 0xA0) {
        s_data_ms = hal_millis();
        if(s_up && s_pcm_cb) { s_pcm_cb(d + 2); }
        return;
    }
    if(s_wait) {                                                       /* answer to the running command? */
        const sct_cmd_t *c = &s_seq[s_seq_i];
        /* the codec answers with ID 0x17 in the SCT_Port templates ("40 17 00"); match on the type there */
        if((c->b0 == 0x40 && b0 == 0x40) || (c->b0 != 0x40 && b0 != 0x40 && c->id == id)) {
            s_acked = true;
            s_ack_b0 = b0;
            s_ack_len = len > sizeof s_ack_d ? (uint8_t)sizeof s_ack_d : (uint8_t)len;
            memcpy(s_ack_d, d, s_ack_len);
            return;
        }
    }
    if(b0 == 0x40) { return; }
    if(id == 0x01) { s_want_chan = true; s_retry = 0; }
    else if(id == 0x00 && len == 1 && d[0] == 0x00) { s_want_pcm = true; s_retry = 0; }
}

static void seq_add(uint8_t b0, uint8_t id, uint8_t len, uint8_t must, uint8_t a, uint8_t b, uint8_t c)
{
    if(s_seq_n >= SEQ_MAX) { return; }
    s_seq[s_seq_n++] = (sct_cmd_t){ b0, id, len, must, { a, b, c } };
}
static void seq_start(void) { s_seq_n = s_seq_i = s_try = 0; s_wait = s_acked = s_seq_fail = false; }

/* one step; true = the sequence is finished */
static bool seq_run(void)
{
    if(s_seq_i >= s_seq_n) { return true; }
    const sct_cmd_t *c = &s_seq[s_seq_i];
    if(!s_wait) {
        s_acked = false;
        s_ack_len = 0;
        s_wait = true;
        s_seq_t0 = hal_millis();
        if(!bb_send(c->b0, c->id, c->d, c->len)) { s_wait = false; s_seq_fail |= c->must; s_seq_i++; }
        return false;
    }
    if(!s_acked && hal_millis() - s_seq_t0 < ACK_MS) { return false; }
    if(!s_acked && ++s_try < 2 && s_pr_state == PR_DONE) { s_wait = false; return false; }   /* one repeat */
    /* a non-zero status byte of a write command counts as a refusal */
    bool ok = s_acked && !(c->must && c->b0 < 0x20 && s_ack_len == 1 && s_ack_d[0] != 0);
    if(!ok && c->must) { s_seq_fail = true; }
    if(s_pr_state == PR_RUN) {
        DBG("SCT probe %02x %02x: %s", c->b0, c->id, s_acked ? "answer" : "no answer");
        for(unsigned i = 0; s_acked && i < s_ack_len; i++) { DBG(" %02x", s_ack_d[i]); }
        DBG("%s", "\n");
        if(c->id == 0x3E && ok) { s_have_io = true; }
    } else if(!ok) {
        DBG("SCT cmd %02x %02x: %s\n", c->b0, c->id, s_acked ? "refused" : "no answer");
    }
    s_wait = false;
    s_try = 0;
    s_seq_i++;
    return s_seq_i >= s_seq_n;
}

/* ---- probe ---- */
static void probe_poll(void)
{
    if(s_pr_state == PR_IDLE) {
        if(!bb_ready()) { return; }
        if(!s_pr_t0) { s_pr_t0 = hal_millis() | 1u; return; }
        if(hal_millis() - s_pr_t0 < 300u) { return; }
        bb_set_listener(listener);
        seq_start();
        seq_add(0x23, 0x32, 0, 0, 0, 0, 0);       /* software version */
        seq_add(0x20, 0x10, 0, 0, 0, 0, 0);       /* VOCODER_SEL */
        seq_add(0x20, 0x1A, 0, 0, 0, 0, 0);       /* PROCESS_MODE */
        seq_add(0x00, 0x3E, 1, 1, 0x00, 0, 0);    /* VOCODER_IO_SET off: accepted? */
        seq_add(0x80, 0x30, 0, 0, 0, 0, 0);       /* external AMBE chip PRODID */
        s_pr_state = PR_RUN;
        return;
    }
    if(s_pr_state == PR_RUN && seq_run()) {
        s_pr_state = PR_DONE;
        DBG("SCT probe done: vocoder IO %s, external AMBE %s\n", s_have_io ? "yes" : "no", s_have_ext ? s_prodid : "no");
    }
}

/* ---- sessions ---- */
#define SPK_GAIN_HI 0x1F                          /* digital speaker gain 0x1F40, mic gain 0x07D0: the D168UV stock */
#define SPK_GAIN_LO 0x40                          /* values (the D890UV image has no calibration to read them from) */
#define MIC_GAIN_HI 0x07
#define MIC_GAIN_LO 0xD0
#define MIC_AMP     0x20                          /* codec 0x10 (stock D890UV 0x1E, D168UV 0x20) */

static bool uses_mic(sct_sess_t s) { return s == SCT_ENC_MIC || s == SCT_MIC_PCM; }
static bool uses_spk(sct_sess_t s) { return s == SCT_DEC || s == SCT_PLAY_PCM; }

static uint8_t io_bits(sct_sess_t s)
{
    switch(s) {
    case SCT_DEC:      return g_sct_tap ? 0x0A : 0x08;
    case SCT_ENC_MIC:  return 0x10;
    case SCT_ENC_PCM:  return 0x90;
    case SCT_MIC_PCM:  return 0x01;
    case SCT_PLAY_PCM: return 0x80;
    default:           return 0x00;
    }
}

static void session_open(sct_sess_t s)            /* command order = stock FUN_080fad68 modes 1 and 5 */
{
    seq_start();
    s_raw49 = false;
    s_raw_hits = s_fec_bad = 0;
    seq_add(0x00, 0x01, 2, 1, 0xFF, 0x00, 0);     /* frames with FEC */
    seq_add(0x05, 0x19, 1, 0, 0x00, 0, 0);        /* carrier lost */
    seq_add(0x05, 0x18, 3, 1, 0x00, 0x00, 0x00);  /* idle */
    if(uses_mic(s)) {
        seq_add(0x00, 0x68, 2, 0, MIC_GAIN_HI, MIC_GAIN_LO, 0);
        seq_add(0x40, 0x0F, 1, 0, 0x90, 0, 0);
        seq_add(0x40, 0x10, 1, 0, MIC_AMP, 0, 0);
    }
    seq_add(0x05, 0x1A, 1, 1, 0x02, 0, 0);        /* digital process mode */
    seq_add(0x05, 0x18, 3, 1, 0x03, 0x03, 0x00);  /* duplex, vocoder loop */
    if(uses_spk(s)) {
        seq_add(0x00, 0x69, 2, 0, SPK_GAIN_HI, SPK_GAIN_LO, 0);
        seq_add(0x40, 0x08, 1, 0, audio_codec_att(), 0, 0);
    }
    seq_add(0x00, 0x3E, 1, 1, io_bits(s), 0, 0);
}

static void session_close(sct_sess_t s)
{
    seq_start();
    seq_add(0x00, 0x3E, 1, 0, 0x00, 0, 0);
    seq_add(0x05, 0x18, 3, 0, 0x00, 0x00, 0x00);
    if(uses_mic(s)) {                             /* mic off (stock FUN_080f2594(0)) */
        seq_add(0x40, 0x0F, 1, 0, 0xB0, 0, 0);
        seq_add(0x40, 0x10, 1, 0, 0x2E, 0, 0);
    }
    if(uses_spk(s)) { seq_add(0x40, 0x08, 1, 0, 0x1F, 0, 0); }
}

static void session_fail(const char *why)
{
    (void)why;
    uint8_t m = (uint8_t)(1u << s_cur);
    if(s_bad1 & m) { s_bad |= m; }
    s_bad1 |= m;
    DBG("SCT session %d failed: %s\n", (int)s_cur, why);
    if(s_req == s_cur) { s_req = SCT_OFF; }
    s_up = false;
    s_closing = true;
    session_close(s_cur);
}

static void session_poll(void)
{
    uint32_t now = hal_millis();
    if(s_closing) {
        if(seq_run()) { s_closing = false; s_cur = SCT_OFF; }
        return;
    }
    if(s_up) {
        if(s_req != s_cur) { s_up = false; s_closing = true; session_close(s_cur); return; }
        bool gives = uses_mic(s_cur) || (s_cur == SCT_ENC_PCM && s_fed);
        if(gives && now - s_data_ms > STALL_MS) { session_fail("chip sends nothing"); }
        else if(s_fed && !s_want_chan && !s_want_pcm && now - s_put_ms > STALL_MS / 2) {
            if(++s_retry > 2) { session_fail("chip takes nothing"); }          /* a lost packet or answer: send on */
            else { s_want_chan = s_cur == SCT_DEC; s_want_pcm = !s_want_chan; s_put_ms = now; }
        }
        return;
    }
    if(s_cur == SCT_OFF) {
        if(s_req == SCT_OFF) { return; }
        s_cur = s_req;
        session_open(s_cur);
        return;
    }
    if(!seq_run()) { return; }                    /* opening */
    if(s_seq_fail) { session_fail("set-up refused"); return; }
    s_up = true;
    s_up_ms = s_data_ms = s_put_ms = now;
    s_want_chan = s_cur == SCT_DEC;               /* the first packet needs no request (stock sets its flag itself) */
    s_want_pcm = s_cur == SCT_ENC_PCM || s_cur == SCT_PLAY_PCM;
    s_fed = false;
    s_retry = 0;
}

void sct_set_sinks(sct_chan_cb chan, sct_pcm_cb pcm) { s_chan_cb = chan; s_pcm_cb = pcm; }
bool sct_chip_ok(void) { return s_have_io && s_pr_state == PR_DONE && g_sct_force != 3; }
bool sct_can(sct_sess_t s) { return sct_chip_ok() && !(s_bad & (1u << s)); }
void sct_session(sct_sess_t s) { s_req = (s != SCT_OFF && !sct_can(s)) ? SCT_OFF : s; }
sct_sess_t sct_session_now(void) { return s_up ? s_cur : SCT_OFF; }
bool sct_want_chan(void) { return s_up && s_cur == SCT_DEC && s_want_chan; }
bool sct_want_pcm(void) { return s_up && (s_cur == SCT_ENC_PCM || s_cur == SCT_PLAY_PCM) && s_want_pcm; }
bool sct_raw49(void) { return s_raw49; }

void sct_note_format(int errs, const uint8_t f[9])
{
    /* 49-bit frames sit in 6 bytes + 1 bit (stock encryption path), the rest of the 9 bytes is 0 */
    if(f[7] == 0 && f[8] == 0 && (f[6] & 0x7F) == 0) { if(s_raw_hits < 255) { s_raw_hits++; } }
    else { s_raw_hits = 0; }
    if(s_raw_hits >= 8) { s_raw49 = true; }       /* 8 in a row: not chance */
    if(errs != 0 && s_fec_bad < 255) { s_fec_bad++; }
}

bool sct_put_chan(const uint8_t d[36])
{
    if(!sct_want_chan()) { return false; }
    uint8_t p[37];
    p[0] = 0x24;
    memcpy(p + 1, d, 36);
    s_want_chan = false;
    s_fed = true;
    s_put_ms = hal_millis();
    return bb_send(0x00, 0x01, p, sizeof p);
}

static uint8_t s_pkt[322];                        /* PCM packet (also the external encode request) */

bool sct_put_pcm(const int16_t pcm[160])
{
    if(!sct_want_pcm()) { return false; }
    s_pkt[0] = 0x00;
    s_pkt[1] = 0xA0;
    for(int i = 0; i < 160; i++) { s_pkt[2 + 2 * i] = (uint8_t)((uint16_t)pcm[i] >> 8); s_pkt[3 + 2 * i] = (uint8_t)pcm[i]; }
    s_want_pcm = false;
    s_fed = true;
    s_put_ms = hal_millis();
    return bb_send(0x00, 0x00, s_pkt, sizeof s_pkt);
}

const char *sct_status_text(void)
{
    if(s_pr_state != PR_DONE) { return "probe"; }
    if(s_have_io && g_sct_force != 3) {
        uint8_t b = s_bad;
        if((b & (1u << SCT_DEC)) && (b & (1u << SCT_ENC_MIC))) { return "voc failed"; }
        if(b & (1u << SCT_DEC)) { return "voc no RX"; }
        if(b & (1u << SCT_ENC_MIC)) { return (b & (1u << SCT_ENC_PCM)) ? "voc no TX" : "voc B no mic"; }
        return "voc A";
    }
    return s_have_ext ? "voc C ext" : "no vocoder";
}

/* ---- external AMBE (mode C), one request in flight ---- */
bool sct_ext_available(void) { return s_have_ext && s_pr_state == PR_DONE && (g_sct_force == 0 || g_sct_force == 3); }
const char *sct_voc_prodid(void) { return s_prodid; }

bool sct_async_busy(void)
{
    if(s_async_busy && hal_millis() - s_async_t0 > 100u) { s_async_busy = false; }   /* lost reply */
    return s_async_busy;
}

static bool async_send(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len, uint8_t rb0, uint8_t rid, sct_async_cb cb)
{
    s_acb = cb;
    s_want_b0 = rb0; s_want_id = rid;
    s_async_busy = true;
    s_async_t0 = hal_millis();
    if(!bb_send(b0, id, d, len)) { s_async_busy = false; return false; }
    return true;
}

static bool rate_ok(int rate)                     /* false = the rate command was just sent: try again later */
{
    if(rate == s_rate) { return true; }
    s_rate_req = rate;
    if(rate == SCT_RATE_DSTAR) {
        static const uint8_t ratep[12] = { 0x01, 0x30, 0x07, 0x63, 0x40, 0, 0, 0, 0, 0, 0, 0x48 };
        (void)async_send(0x80, 0x0A, ratep, sizeof ratep, 0x80, 0x0A, NULL);
    } else {
        uint8_t r = (uint8_t)rate;
        (void)async_send(0x80, 0x09, &r, 1, 0x80, 0x09, NULL);
    }
    return false;
}

bool sct_async_encode(int rate, const int16_t pcm[160], sct_async_cb cb)
{
    if(sct_async_busy() || !sct_ext_available() || !rate_ok(rate)) { return false; }
    s_pkt[0] = 0xA0;
    for(int i = 0; i < 160; i++) { s_pkt[1 + 2 * i] = (uint8_t)((uint16_t)pcm[i] >> 8); s_pkt[2 + 2 * i] = (uint8_t)pcm[i]; }
    return async_send(0x82, 0x00, s_pkt, 321, 0x81, 0x01, cb);
}

bool sct_async_decode(int rate, const uint8_t *bits, int nbits, sct_async_cb cb)
{
    if(sct_async_busy() || !sct_ext_available() || !rate_ok(rate)) { return false; }
    uint8_t d[1 + 9];
    int nbytes = (nbits + 7) / 8;
    if(nbytes > 9) { return false; }
    d[0] = (uint8_t)nbits;
    memcpy(d + 1, bits, (size_t)nbytes);
    return async_send(0x81, 0x01, d, (uint16_t)(1 + nbytes), 0x82, 0x00, cb);
}

#ifdef CFW_DEBUG
/* ---- bench check (make DEBUG=1 only; docs/SCT3288_VOCODER.md section 6). Receive side only: it runs the probes and
 * the sessions above, all without RF (the allow-list has no transmit command), and stops when a transmission starts.
 * Start: g_sct_bench = 1 from the debugger (hw.py / gdb), or RTT key 'b'. Every frame the chip sends is logged. */
volatile uint8_t g_sct_bench;
static uint8_t s_bn_step;
static uint32_t s_bn_t0, s_bn_last;
static uint16_t s_bn_pk, s_bn_pcm, s_bn_max;
static uint8_t s_bn_rec[8][36], s_bn_nrec, s_bn_rd;
static sct_chan_cb s_bn_cc;
static sct_pcm_cb s_bn_pc;

bool sct_bench_active(void) { return s_bn_step != 0; }

static void bench_rx(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len)
{
    if(!s_bn_step) { return; }
    uint32_t now = hal_millis();
    bool bulk = len >= 28;
    if(!bulk || s_bn_pk + s_bn_pcm < 6) {         /* long streams: the first 6 packets only */
        DBG("  +%u ms  %02x %02x len %u:", (unsigned)(now - s_bn_last), b0, id, len);
        for(unsigned i = 0; i < len && i < (bulk ? 38u : 12u); i++) { DBG(" %02x", d[i]); }
        DBG("%s", "\n");
    }
    s_bn_last = now;
}

static void bench_chan(const uint8_t *f, unsigned n)
{
    if(s_bn_nrec < 8 && n == 4) { memcpy(s_bn_rec[s_bn_nrec++], f, 36); }
    s_bn_pk++;
}
static void bench_pcm(const uint8_t *be)
{
    for(int i = 0; i < 160; i++) {
        int v = (int16_t)((be[2 * i] << 8) | be[2 * i + 1]);
        if(v < 0) { v = -v; }
        if(v > s_bn_max) { s_bn_max = (uint16_t)v; }
    }
    s_bn_pcm++;
}

static void bench_next(const char *what)
{
    if(s_bn_step > 1) {
        DBG("  -> %u frame packets, %u PCM packets, PCM peak %u, FEC errors %u, %s\n", s_bn_pk, s_bn_pcm, s_bn_max, s_fec_bad,
            s_raw49 ? "49-bit" : "72-bit");
    }
    DBG("SCT bench %u: %s\n", s_bn_step, what);
    s_bn_step++;
    s_bn_t0 = s_bn_last = hal_millis();
    s_bn_pk = s_bn_pcm = s_bn_max = 0;
}

static bool bench_tone(void)                      /* 1 kHz, -12 dBFS */
{
    if(!sct_want_pcm()) { return false; }
    static const int16_t sn[8] = { 0, 5793, 8192, 5793, 0, -5793, -8192, -5793 };
    int16_t pcm[160];
    for(int i = 0; i < 160; i++) { pcm[i] = sn[i & 7]; }
    return sct_put_pcm(pcm);
}

static void bench_poll(void)
{
    uint32_t now = hal_millis(), dt = now - s_bn_t0;
    if(!s_bn_step) {
        if(dbg_getc() == 'b') { g_sct_bench = 1; }
        if(!g_sct_bench || s_pr_state != PR_DONE || s_cur != SCT_OFF || s_req != SCT_OFF) { return; }
        g_sct_bench = 0;
        s_bn_cc = s_chan_cb; s_bn_pc = s_pcm_cb;
        sct_set_sinks(bench_chan, bench_pcm);
        s_bad = s_bad1 = 0;
        s_bn_nrec = 0;
        s_bn_step = 1;
        bench_next("identity reads + external AMBE PRODID / VERSTRING");
        for(uint8_t id = 0x30; id <= 0x33; id++) { (void)bb_send(0x23, id, NULL, 0); }
        (void)bb_send(0x20, 0x10, NULL, 0);
        (void)bb_send(0x20, 0x1A, NULL, 0);
        (void)bb_send(0x80, 0x30, NULL, 0);
        (void)bb_send(0x80, 0x31, NULL, 0);
        return;
    }
    if(modem_transmitting()) { DBG("SCT bench %u: stopped, transmitter on\n", s_bn_step); goto end; }
    static const uint8_t sil[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
    uint8_t p[36];
    static const uint8_t sel[1] = { 0x0C }, r33[1] = { 0x21 };
    switch(s_bn_step) {
    case 2: if(dt > 1500u) { bench_next("VOCODER_SEL 0C (time to the answer)"); (void)bb_send(0x00, 0x10, sel, 1); } break;
    case 3: if(dt > 5000u) { bench_next("mic -> encoder -> frames, 2 s: speak"); sct_session(SCT_ENC_MIC); } break;
    case 4: if(dt > 2500u) { bench_next("play the recorded frames, then silence"); sct_session(SCT_DEC); s_bn_rd = 0; } break;
    case 5:
        if(sct_want_chan()) {
            if(s_bn_rd < s_bn_nrec) { memcpy(p, s_bn_rec[s_bn_rd++], 36); }
            else { for(int i = 0; i < 4; i++) { memcpy(p + 9 * i, sil, 9); } }
            (void)sct_put_chan(p);
        }
        if(dt > 2500u) { bench_next("1 kHz PCM -> encoder -> frames"); sct_session(SCT_ENC_PCM); }
        break;
    case 6: (void)bench_tone(); if(dt > 2500u) { bench_next("mic PCM to the MCU, 2 s: speak"); sct_session(SCT_MIC_PCM); } break;
    case 7: if(dt > 2500u) { bench_next("1 kHz PCM -> speaker"); sct_session(SCT_PLAY_PCM); } break;
    case 8: (void)bench_tone(); if(dt > 2500u) { bench_next("sessions off; external AMBE: rate 33 + one PCM packet"); sct_session(SCT_OFF); } break;
    case 9:
        if(dt > 800u && dt < 2000u) {
            (void)bb_send(0x80, 0x09, r33, 1);
            memset(s_pkt, 0, sizeof s_pkt);
            s_pkt[0] = 0xA0;
            (void)bb_send(0x82, 0x00, s_pkt, 321);
            s_bn_t0 -= 2000u;
        } else if(dt > 3500u) { bench_next("done"); goto end; }
        break;
    default: goto end;
    }
    return;
end:
    DBG("SCT bench end: failed sessions mask %02x, refused commands %u\n", s_bad1, bb_refused(NULL, NULL));
    sct_session(SCT_OFF);
    sct_set_sinks(s_bn_cc, s_bn_pc);
    s_bn_step = 0;
}
#endif

void sct_poll(void)
{
#ifdef CFW_DEBUG
    if(s_pr_state == PR_DONE) { bench_poll(); }
#endif
    if(s_pr_state != PR_DONE) { probe_poll(); return; }
    session_poll();
}
