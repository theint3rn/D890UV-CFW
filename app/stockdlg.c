/* Stock .rdt-only dialogs (see stockdlg.h): cell positions cached by one walk of codeplug section 10 + journal. */
#include <string.h>
#include "stockdlg.h"
#include "cfwcp.h"

typedef struct { const uint8_t *p; } cell_t;       /* points at the entry (u8 table, u8 col, u16 row, u16 len, text) */
#define N_ALARM 25
#define N_DTMF  17
#define N_2T    8
#define N_5T    20
#define N_ALIAS 4
static cell_t s_alarm[N_ALARM], s_dtmf[N_DTMF], s_2t[N_2T], s_5t[N_5T], s_alias[N_ALIAS];
static uint16_t s_rows[3];                         /* 5Tone info codes, friends, encryption codes */

static cell_t *rec(int t, int c)
{
    if(c < 0) { return NULL; }
    switch(t) {
    case SD_T_ALARM:  return c < N_ALARM ? &s_alarm[c] : NULL;
    case SD_T_DTMF:   return c < N_DTMF ? &s_dtmf[c] : NULL;
    case SD_T_2TONE:  return c < N_2T ? &s_2t[c] : NULL;
    case SD_T_5TONE:  return c < N_5T ? &s_5t[c] : NULL;
    case SD_T_ALIAS:  return c < N_ALIAS ? &s_alias[c] : NULL;
    default:          return NULL;
    }
}
static int list_ix(int t) { return t == SD_T_5TINFO ? 0 : t == SD_T_FRIENDS ? 1 : t == SD_T_ENC ? 2 : -1; }

static uint32_t num(const char *t)
{
    uint32_t v = 0;
    while(*t == ' ') { t++; }
    while(*t >= '0' && *t <= '9') { v = v * 10u + (uint32_t)(*t++ - '0'); }
    return v;
}

static void text(const uint8_t *e, char *b, int n)
{
    int k = e[4] | e[5] << 8;
    if(k > n - 1) { k = n - 1; }
    memcpy(b, e + 6, (size_t)k);
    b[k] = 0;
}

static bool walk(const uint8_t *e, uint32_t pos, void *u)
{
    (void)pos; (void)u;
    uint16_t row = (uint16_t)(e[2] | e[3] << 8);
    int l = list_ix(e[0]);
    if(l >= 0 && e[1] == 0xFF && row == 0) {
        char b[8];
        text(e, b, sizeof b);
        uint32_t v = num(b);
        s_rows[l] = (uint16_t)(v > 1000u ? 1000u : v);
    }
    cell_t *c = l < 0 && row == 0 ? rec(e[0], e[1]) : NULL;
    if(c) { c->p = e; }                            /* a later entry wins */
    return true;
}

void stockdlg_init(void)
{
    memset(s_alarm, 0, sizeof s_alarm); memset(s_dtmf, 0, sizeof s_dtmf); memset(s_2t, 0, sizeof s_2t);
    memset(s_5t, 0, sizeof s_5t); memset(s_alias, 0, sizeof s_alias); memset(s_rows, 0, sizeof s_rows);
    if(cfwcp_valid()) { cfwcp_stock_walk(walk, NULL); }
}

bool stockdlg_text(int table, int col, char *buf, int n)
{
    cell_t *c = rec(table, col);
    if(n < 1) { return false; }
    buf[0] = 0;
    if(!c || !c->p || !cfwcp_valid()) { return false; }
    text(c->p, buf, n);
    return true;
}

uint32_t stockdlg_num(int table, int col, uint32_t dflt)
{
    char b[16];
    if(!stockdlg_text(table, col, b, sizeof b) || b[0] < '0' || b[0] > '9') { return dflt; }
    return num(b);
}

int stockdlg_rows(int table) { int l = list_ix(table); return l < 0 || !cfwcp_valid() ? 0 : s_rows[l]; }

bool stockdlg_row_text(int table, int row, int col, char *buf, int n)
{
    if(n < 1) { return false; }
    buf[0] = 0;
    if(row < 0 || row >= stockdlg_rows(table)) { return false; }
    return cfwcp_stock_value_row(table, col, row, buf, n) && buf[0];
}

uint8_t stockdlg_alias_prio(void)
{
    if(!cfwcp_valid()) { return TA_PRIO_ALIAS; }
    uint32_t v = stockdlg_num(SD_T_ALIAS, TA_PRIO, TA_PRIO_OFF);
    return (uint8_t)(v <= TA_PRIO_ALIAS ? v : TA_PRIO_OFF);
}

uint8_t stockdlg_alias_format(void)
{
    uint32_t v = stockdlg_num(SD_T_ALIAS, TA_FORMAT, 0);
    return (uint8_t)(v <= 2u ? v : 0u);
}
