/* Small helpers shared by the proto glue files (status text without printf). */
#ifndef CFW_GLUE_H
#define CFW_GLUE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include "modem.h"

typedef struct { char *p, *end; } sbuf_t;

static inline sbuf_t sb_init(char *buf, size_t len) { sbuf_t b = { buf, buf + (len ? len - 1 : 0) }; if(len) { *buf = 0; } return b; }
static inline void sb_c(sbuf_t *b, char c) { if(b->p < b->end) { *b->p++ = c; *b->p = 0; } }
static inline void sb_s(sbuf_t *b, const char *s) { while(*s) { sb_c(b, *s++); } }
static inline void sb_sn(sbuf_t *b, const char *s, size_t n)   /* fixed field, trailing spaces dropped */
{
    size_t k = n;
    while(k && (s[k - 1] == ' ' || s[k - 1] == 0)) { k--; }
    for(size_t i = 0; i < k; i++) { sb_c(b, (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?'); }
}
static inline void sb_u(sbuf_t *b, uint32_t v)
{
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while(v);
    while(n) { sb_c(b, t[--n]); }
}
static inline void sb_x(sbuf_t *b, uint32_t v, int digits)
{
    for(int i = digits - 1; i >= 0; i--) { sb_c(b, "0123456789ABCDEF"[(v >> (4 * i)) & 0xF]); }
}
static inline void sb_mhz(sbuf_t *b, uint32_t hz)   /* 851.0125 */
{
    sb_u(b, hz / 1000000u); sb_c(b, '.');
    uint32_t f = (hz % 1000000u) / 100u;
    sb_c(b, (char)('0' + f / 1000u)); sb_c(b, (char)('0' + f / 100u % 10u));
    sb_c(b, (char)('0' + f / 10u % 10u)); sb_c(b, (char)('0' + f % 10u));
}
static inline size_t sb_len(const sbuf_t *b, const char *buf) { return (size_t)(b->p - buf); }

/* TX symbol queue: the glue builds one burst / frame at a time into it, tx_symbols() drains it */
#define TXQ_MAX 2304
typedef struct { uint8_t b[TXQ_MAX]; uint16_t n, rd; } txq_t;
static inline void   txq_reset(txq_t *q) { q->n = q->rd = 0; }
static inline bool   txq_empty(const txq_t *q) { return q->rd >= q->n; }
static inline void   txq_put(txq_t *q, const uint8_t *d, size_t n)
{
    if(txq_empty(q)) { q->n = q->rd = 0; }
    if(q->n + n > TXQ_MAX) { n = TXQ_MAX - q->n; }
    memcpy(q->b + q->n, d, n);
    q->n = (uint16_t)(q->n + n);
}
static inline size_t txq_take(txq_t *q, uint8_t *out, size_t max)
{
    size_t k = q->n - q->rd;
    if(k > max) { k = max; }
    memcpy(out, q->b + q->rd, k);
    q->rd = (uint16_t)(q->rd + k);
    return k;
}
/* Encryption hooks (app/radio.c; weak defaults in protos/crypt_weak.c = none). Only used while Settings >
 * Encryption is on (off by default, illegal on amateur bands). kind = cfw_cps KEY_KINDS index (cp_key_t.kind):
 * 3 DMR AES-256 (cfw privacy scheme), 7 NXDN scrambler, 8 NXDN DES, 9 NXDN AES-256.
 * cfw_crypt_key: i-th usable key of kinds lo..hi (RX keyring), false = no more.
 * cfw_crypt_tx: key of the current channel for TX (gate on, key set, kind lo..hi, not an amateur band). */
bool cfw_crypt_key(int i, uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len);
bool cfw_crypt_tx(uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key, uint8_t *len);
uint32_t cfw_crypt_iv(void);                 /* 32 fresh bits for an IV */

/* space-padded fixed callsign field */
static inline void call_field(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for(; i < n && src[i] && src[i] != ' '; i++) { dst[i] = src[i]; }
    for(; i < n; i++) { dst[i] = ' '; }
}

#endif
