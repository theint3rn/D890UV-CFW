#include <string.h>
#include "calllog.h"
#include "hal.h"
#include "modem_core.h"
#include "radio.h"

static call_rec_t s_log[CALLLOG_N];
static int s_nlog;
static sms_rec_t s_in[SMS_N], s_out[SMS_N];
static int s_nin, s_nout;
static cfw_call_info_t s_last;
static bool s_in_call;

static void push_log(const call_rec_t *r)
{
    memmove(&s_log[1], &s_log[0], sizeof s_log[0] * (CALLLOG_N - 1));
    s_log[0] = *r;
    if(s_nlog < CALLLOG_N) { s_nlog++; }
}

void calllog_poll(void)
{
    if(!modem_active()) { s_in_call = false; return; }
    const cfw_call_info_t *ci = modem_call_info();
    if(!ci->voice) { s_in_call = false; return; }
    bool same = s_in_call && ci->src_id == s_last.src_id && ci->dst_id == s_last.dst_id &&
                strncmp(ci->src_call, s_last.src_call, sizeof ci->src_call) == 0;
    if(same) {
        /* talker alias arriving later in the same call: fill it in */
        if(ci->alias[0] && !s_log[0].call[0] && s_nlog) { memcpy(s_log[0].call, ci->alias, 8); s_log[0].call[8] = 0; }
        return;
    }
    if(!ci->src_id && !ci->src_call[0] && !ci->dst_id) { return; }     /* nothing to log yet */
    s_last = *ci;
    s_in_call = true;
    call_rec_t r;
    memset(&r, 0, sizeof r);
    r.t = rtc_now();
    r.src = ci->src_id;
    r.dst = ci->dst_id;
    const char *c = ci->src_call[0] ? ci->src_call : ci->alias;
    for(int i = 0; i < 8 && c[i]; i++) { r.call[i] = c[i]; }
    r.mode = (uint8_t)modem_mode();
    r.flags = (uint8_t)((ci->private_call ? CL_PRIVATE : 0) | (ci->encrypted ? CL_ENC : 0));
    push_log(&r);
}

void calllog_add_tx(cfw_mode_t mode, uint32_t dst, bool priv)
{
    call_rec_t r;
    memset(&r, 0, sizeof r);
    r.t = rtc_now();
    r.src = g_radio.my_id;
    r.dst = dst;
    r.mode = (uint8_t)mode;
    r.flags = (uint8_t)(CL_OUT | (priv ? CL_PRIVATE : 0));
    if(mode == CFW_MODE_DSTAR || mode == CFW_MODE_YSF) { memcpy(r.call, g_radio.my_call, 8); }
    push_log(&r);
}

int calllog_count(void) { return s_nlog; }
const call_rec_t *calllog_at(int i) { return (i >= 0 && i < s_nlog) ? &s_log[i] : NULL; }
void calllog_clear(void) { s_nlog = 0; }

/* ---- SMS ---- */
static void push_sms(sms_rec_t *box, int *n, const sms_rec_t *r)
{
    memmove(&box[1], &box[0], sizeof box[0] * (SMS_N - 1));
    box[0] = *r;
    if(*n < SMS_N) { (*n)++; }
}

void sms_rx(uint32_t src, uint32_t dst, bool group, const uint8_t *text, int len)
{
    sms_rec_t r;
    memset(&r, 0, sizeof r);
    r.t = rtc_now();
    r.peer = src;
    r.flags = (uint8_t)(SMS_UNREAD | (group ? SMS_GROUP : 0));
    (void)dst;
    int k = 0;
    for(int i = 0; i < len && k < SMS_MAX_TEXT; i++) {
        uint8_t c = text[i];
        if(c == 0) { break; }
        r.text[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    r.len = (uint8_t)k;
    push_sms(s_in, &s_nin, &r);
}

void sms_add_out(uint32_t dst, bool group, const char *text, bool sent)
{
    sms_rec_t r;
    memset(&r, 0, sizeof r);
    r.t = rtc_now();
    r.peer = dst;
    r.flags = (uint8_t)(SMS_OUT | (group ? SMS_GROUP : 0) | (sent ? SMS_SENT : 0));
    strncpy(r.text, text, SMS_MAX_TEXT);
    r.len = (uint8_t)strlen(r.text);
    push_sms(s_out, &s_nout, &r);
}

int sms_count(bool out) { return out ? s_nout : s_nin; }
sms_rec_t *sms_at(bool out, int i)
{
    if(i < 0 || i >= (out ? s_nout : s_nin)) { return NULL; }
    return out ? &s_out[i] : &s_in[i];
}
int sms_unread(void)
{
    int n = 0;
    for(int i = 0; i < s_nin; i++) { if(s_in[i].flags & SMS_UNREAD) { n++; } }
    return n;
}
void sms_clear(bool out) { if(out) { s_nout = 0; } else { s_nin = 0; } }

void msgs_export(msgs_store_t *m)
{
    memcpy(m->log, s_log, sizeof s_log);
    memcpy(m->in, s_in, sizeof s_in);
    memcpy(m->out, s_out, sizeof s_out);
    m->nlog = (uint8_t)s_nlog; m->nin = (uint8_t)s_nin; m->nout = (uint8_t)s_nout;
}

void msgs_import(const msgs_store_t *m)
{
    memcpy(s_log, m->log, sizeof s_log);
    memcpy(s_in, m->in, sizeof s_in);
    memcpy(s_out, m->out, sizeof s_out);
    s_nlog = m->nlog <= CALLLOG_N ? m->nlog : 0;
    s_nin = m->nin <= SMS_N ? m->nin : 0;
    s_nout = m->nout <= SMS_N ? m->nout : 0;
    for(int i = 0; i < s_nin; i++) { s_in[i].text[SMS_MAX_TEXT] = 0; }
    for(int i = 0; i < s_nout; i++) { s_out[i].text[SMS_MAX_TEXT] = 0; }
}

/* DMR SMS from proto_dmr (hal/modem.c data hook, type 1) */
void modem_data_hook(uint8_t type, const uint8_t *d, size_t n)
{
    if(type != 1 || n < 7) { return; }
    uint32_t src = (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2];
    uint32_t dst = (uint32_t)d[3] << 16 | (uint32_t)d[4] << 8 | d[5];
    sms_rx(src, dst, d[6] != 0, d + 7, (int)n - 7);
}
