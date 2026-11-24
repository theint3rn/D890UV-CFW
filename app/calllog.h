/* Call log (last calls heard / made, digital modes) and DMR SMS in/out boxes. RAM rings, persisted with the radio's
 * settings blob (power off / Save now). */
#ifndef CFW_CALLLOG_H
#define CFW_CALLLOG_H
#include <stdint.h>
#include <stdbool.h>
#include "modem.h"

typedef struct {
    uint32_t t;                 /* rtc_now() */
    uint32_t src, dst;          /* ids (DMR / P25 / NXDN), dst = talkgroup or private target */
    char     call[9];           /* D-STAR / YSF source callsign, or the DMR talker alias (first 8 chars) */
    uint8_t  mode;              /* cfw_mode_t */
    uint8_t  flags;             /* CL_* */
    uint8_t  pad;
} call_rec_t;
enum { CL_OUT = 1, CL_PRIVATE = 2, CL_ENC = 4 };
#define CALLLOG_N 16

void     calllog_poll(void);                     /* every 100 ms: new call heard -> log */
void     calllog_add_tx(cfw_mode_t mode, uint32_t dst, bool priv);
int      calllog_count(void);
const call_rec_t *calllog_at(int i);             /* 0 = newest */
void     calllog_clear(void);

typedef struct {
    uint32_t t;
    uint32_t peer;              /* sender (inbox) / target (outbox) */
    uint8_t  flags;             /* SMS_* */
    uint8_t  len;
    char     text[66];
} sms_rec_t;
enum { SMS_OUT = 1, SMS_GROUP = 2, SMS_UNREAD = 4, SMS_SENT = 8 };
#define SMS_N 8
#define SMS_MAX_TEXT 64

void     sms_rx(uint32_t src, uint32_t dst, bool group, const uint8_t *text, int len);   /* from the DMR glue */
int      sms_count(bool out);
sms_rec_t *sms_at(bool out, int i);              /* 0 = newest */
void     sms_add_out(uint32_t dst, bool group, const char *text, bool sent);
int      sms_unread(void);
void     sms_clear(bool out);

/* settings blob */
typedef struct { call_rec_t log[CALLLOG_N]; uint8_t nlog, nin, nout, pad; sms_rec_t in[SMS_N], out[SMS_N]; } msgs_store_t;
void     msgs_export(msgs_store_t *m);
void     msgs_import(const msgs_store_t *m);
#endif
