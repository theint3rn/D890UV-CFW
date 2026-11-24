/* SEGGER RTT compatible log ring (see dbg.h). Only compiled in with CFW_RTT (make DEBUG=1). */
#include "dbg.h"

#ifdef CFW_RTT
#include <stdarg.h>
#include <stdbool.h>

#ifndef CFW_RTT_UP_SIZE
#define CFW_RTT_UP_SIZE 2048u
#endif

typedef struct {
    const char *name;
    char *buf;
    uint32_t size;
    volatile uint32_t wr;           /* written by the target */
    volatile uint32_t rd;           /* written by the probe */
    uint32_t flags;                 /* 0 = skip (drop data that does not fit) */
} rtt_ring_t;

/* layout identical to SEGGER_RTT_CB with 1 up / 1 down buffer (the down buffer is empty: size 0) */
typedef struct {
    char id[16];
    int32_t max_up;
    int32_t max_down;
    rtt_ring_t up[1];
    rtt_ring_t down[1];
} rtt_cb_t;

/* not static: tools/hw/hw.py and RTT viewers find it by name / by the "SEGGER RTT" id */
rtt_cb_t _SEGGER_RTT __attribute__((used, aligned(16)));
static char s_rtt_up[CFW_RTT_UP_SIZE];

void dbg_init(void)
{
    rtt_cb_t *cb = &_SEGGER_RTT;
    cb->max_up = 1;
    cb->max_down = 1;
    cb->up[0].name = "Terminal";
    cb->up[0].buf = s_rtt_up;
    cb->up[0].size = sizeof s_rtt_up;
    cb->up[0].wr = 0;
    cb->up[0].rd = 0;
    cb->up[0].flags = 0;
    cb->down[0].name = "Terminal";
    cb->down[0].buf = 0;
    cb->down[0].size = 0;
    cb->down[0].wr = cb->down[0].rd = cb->down[0].flags = 0;
    /* id last (and back to front, like SEGGER), so a probe scanning RAM never sees a half-built block. The id
     * template is const (flash), so the only RAM copy is the live control block. */
    static const char id[] = { 'S', 'E', 'G', 'G', 'E', 'R', ' ', 'R', 'T', 'T' };
    for(int i = 15; i >= 0; i--) {
        __asm volatile("" ::: "memory");
        cb->id[i] = i < (int)sizeof id ? id[i] : 0;
    }
    __asm volatile("dmb" ::: "memory");
}

void dbg_write(const char *s, unsigned n)
{
    rtt_ring_t *r = &_SEGGER_RTT.up[0];
    if(r->size == 0) { return; }                   /* dbg_init not run yet */
    uint32_t wr = r->wr, rd = r->rd;
    uint32_t free = rd > wr ? rd - wr - 1 : r->size - (wr - rd) - 1;
    if(n > free) { n = free; }                     /* skip mode: drop the tail */
    while(n--) {
        r->buf[wr] = *s++;
        if(++wr == r->size) { wr = 0; }
    }
    __asm volatile("dmb" ::: "memory");
    r->wr = wr;
}

void dbg_puts(const char *s)
{
    unsigned n = 0;
    while(s[n]) { n++; }
    dbg_write(s, n);
}

static char *put_num(char *p, char *end, uint32_t v, unsigned base, bool upper, int width, char pad, bool neg)
{
    char tmp[12];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dig[v % base]; v /= base; } while(v && n < (int)sizeof tmp);
    if(neg) { width--; }
    if(neg && pad == '0' && p < end) { *p++ = '-'; neg = false; }
    for(int w = n; w < width && p < end; w++) { *p++ = pad; }
    if(neg && p < end) { *p++ = '-'; }
    while(n && p < end) { *p++ = tmp[--n]; }
    return p;
}

void dbg_printf(const char *fmt, ...)
{
    char out[128];
    char *p = out, *end = out + sizeof out;
    va_list ap;
    va_start(ap, fmt);
    while(*fmt && p < end) {
        if(*fmt != '%') { *p++ = *fmt++; continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if(*fmt == '0') { pad = '0'; fmt++; }
        while(*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        while(*fmt == 'l') { fmt++; }
        char c = *fmt ? *fmt++ : 0;
        switch(c) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            p = put_num(p, end, v < 0 ? (uint32_t)-v : (uint32_t)v, 10, false, width, pad, v < 0);
            break;
        }
        case 'u': p = put_num(p, end, va_arg(ap, uint32_t), 10, false, width, pad, false); break;
        case 'x': p = put_num(p, end, va_arg(ap, uint32_t), 16, false, width, pad, false); break;
        case 'X': p = put_num(p, end, va_arg(ap, uint32_t), 16, true, width, pad, false); break;
        case 'p': p = put_num(p, end, (uint32_t)va_arg(ap, void *), 16, false, 8, '0', false); break;
        case 'c': *p++ = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if(!s) { s = "(null)"; }
            while(*s && p < end) { *p++ = *s++; }
            break;
        }
        case '%': *p++ = '%'; break;
        default: break;
        }
    }
    va_end(ap);
    dbg_write(out, (unsigned)(p - out));
}
#endif
