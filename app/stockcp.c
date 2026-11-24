/* Stock CPS DMR / NXDN sections used by the cfw: see stockcp.h and cfw_cps/docs/CODEPLUG.md.
 * One walk over codeplug section 10 at codeplug check time fills small RAM caches (radio IDs, key tables, per-channel
 * stock columns); names (NX Talk Group) are looked up on demand, the user database (section 16) by binary search in
 * flash. Nothing here writes flash. */
#include <string.h>
#include "stockcp.h"
#include "modem.h"
#include "protos/glue.h"

/* table / column numbers: position in the model's stock .LST order and stock CSV column (cfw_cps/schema) */
static const stockcp_map_t k_map =
#if CFWCP_MODEL_ID == 1
    /* D168UV: RadioIDList 1, AESEncryptionCode 26, ARC4EncryptionCode 27, EncryptionCodeRdt 72 (the .rdt-only
     * Encryption Code dialog, cfw_cps/schema/rdt_dialogs.json); no NXDN */
    { 1, 72, 26, 27, 0xFF, 0xFF, 0xFF,   11, 27, 28, 52, 0xFF };
#else
    /* D890UV: RadioIDList 1, EncryptionCode 37, AESEncryptionCode 28, ARC4EncryptionCode 29, NXEncryptionCode 23,
     * NXTalkGroup 26, NXSetting 34; channel Radio ID 12, AES Digital Encryption 28, Digital Encryption 29, ARC4 53,
     * NxdnEncry 72 */
    { 1, 37, 28, 29, 23, 26, 34,   12, 28, 29, 53, 72 };
#endif
const stockcp_map_t *stockcp_map(void) { return &k_map; }

#define T_CHAN    0xF0u
#define MAX_RID   250
#define MAX_TKEY  32          /* AES / ARC4 rows kept */
#if CFWCP_MODEL_ID == 1
#define HAVE_NX   0           /* no NX tables: no caches for them */
#define BP_LIST   1           /* Encryption Code = list of set rows: No., hex key, decimal ID */
#define MAX_CH    384         /* channels with stock radio ID / key columns (D168UV: 224 KB RAM) */
#define MAX_KEYS  40          /* stock keys exported */
#else
#define HAVE_NX   1
#define BP_LIST   0           /* Encryption Code = 32 fixed rows, decimal key and ID */
#define MAX_CH    2048
#define MAX_KEYS  64
#endif

#define NBP  32               /* Encryption Code No. 1..32 */
#define NNX  (HAVE_NX ? 32 : 1)
/* per channel: Radio ID List row, the raw stock key columns (resolved to a key by stockcp_channel_key) */
typedef struct {
    uint16_t ch;
    uint8_t  rid, flags, digenc, arc4;
#if HAVE_NX
    uint8_t  nxenc;
#endif
} chx_t;
#define CHX_ENH 1u            /* AES Digital Encryption = Enhanced */
typedef struct { uint8_t id; uint8_t len; const uint8_t *hex; } tkey_t;

static uint32_t s_rid_id[MAX_RID];
static uint16_t s_rid_hash[MAX_RID];
static uint16_t s_nrid;
static uint16_t s_bp_key[NBP], s_bp_id[NBP];
#if BP_LIST
static uint8_t  s_bp_no[NBP];                                /* per list row: No., key, ID (moved to s_bp_key / _id by No.) */
static uint16_t s_bp_lkey[NBP], s_bp_lid[NBP];
#endif
static uint32_t s_bp_ref;
static tkey_t   s_aes[MAX_TKEY], s_arc4[MAX_TKEY];
static uint8_t  s_naes, s_narc4;
static uint16_t s_nx_code[NNX];
static uint8_t  s_nx_kid[NNX], s_nx_valid[NNX];
static chx_t    s_chx[MAX_CH];
static uint16_t s_nchx;
static struct { uint8_t kind, row; } s_kref[MAX_KEYS];       /* the key itself is built on demand: stockcp_key() */
static uint8_t  s_nkeys;
static uint8_t  s_bp_idx[NBP], s_nx_idx[NNX], s_aes_idx[MAX_TKEY], s_arc4_idx[MAX_TKEY];
static uint16_t s_nx_own;
static uint8_t  s_nx_moni;
static const cp_user_t *s_user;
static uint32_t s_nuser;

/* ---------------- text helpers ---------------- */
static uint32_t dec_n(const uint8_t *t, int n)
{
    uint32_t v = 0;
    int i = 0;
    while(i < n && t[i] == ' ') { i++; }
    for(; i < n && t[i] >= '0' && t[i] <= '9'; i++) { v = v * 10u + (uint32_t)(t[i] - '0'); }
    return v;
}
/* "Off" -> 0; "12", "Encrypt 12", "Encryption 12" -> 12 (the trailing number) */
static uint32_t tail_num(const uint8_t *t, int n)
{
    int e = n;
    while(e && t[e - 1] == ' ') { e--; }
    int b = e;
    while(b && t[b - 1] >= '0' && t[b - 1] <= '9') { b--; }
    return b < e ? dec_n(t + b, e - b) : 0;
}
static int hexv(uint8_t c)
{
    if(c >= '0' && c <= '9') { return c - '0'; }
    if(c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if(c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}
/* hex text -> bytes; returns the byte count, -1 on a bad digit / odd length / too long */
static int hex_bytes(const uint8_t *t, int n, uint8_t *out, int max)
{
    int k = 0, hi = -1;
    for(int i = 0; i < n; i++) {
        if(t[i] == ' ') { continue; }
        int v = hexv(t[i]);
        if(v < 0) { return -1; }
        if(hi < 0) { hi = v; continue; }
        if(k >= max) { return -1; }
        out[k++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    return hi < 0 ? k : -1;
}
static uint16_t name_hash(const uint8_t *t, int n)
{
    while(n && t[n - 1] == ' ') { n--; }
    while(n && *t == ' ') { t++; n--; }
    uint32_t h = 2166136261u;
    for(int i = 0; i < n; i++) { h = (h ^ t[i]) * 16777619u; }
    return (uint16_t)(h ^ (h >> 16)) | 1u;          /* never 0 */
}
static bool on_text(const uint8_t *t, int n)       /* "1", "On", "Yes" */
{
    return n > 0 && (t[0] == '1' || t[0] == 'O' || t[0] == 'o' || t[0] == 'Y' || t[0] == 'y') &&
           !(n >= 3 && (t[1] == 'f' || t[1] == 'F'));
}
static void put_name(char *dst, const char *pfx, uint32_t num)
{
    char d[10];
    int k = 0, i = 0;
    while(pfx[i] && k < 10) { dst[k++] = pfx[i++]; }
    i = 0;
    do { d[i++] = (char)('0' + num % 10u); num /= 10u; } while(num && i < 10);
    while(i && k < 16) { dst[k++] = d[--i]; }
    while(k < 16) { dst[k++] = 0; }
}

/* ---------------- the walk ---------------- */
static chx_t *chx_for(uint16_t ch)
{
    if(s_nchx && s_chx[s_nchx - 1].ch == ch) { return &s_chx[s_nchx - 1]; }
    for(int i = 0; i < s_nchx; i++) { if(s_chx[i].ch == ch) { return &s_chx[i]; } }   /* entries come in row order */
    if(s_nchx >= MAX_CH) { return NULL; }
    chx_t *c = &s_chx[s_nchx++];
    memset(c, 0, sizeof *c);
    c->ch = ch; c->rid = 0xFF;
    return c;
}

static tkey_t *tkey_row(tkey_t *a, uint8_t *n, uint16_t row)
{
    if(row >= MAX_TKEY) { return NULL; }
    while(*n <= row) { memset(&a[*n], 0, sizeof a[0]); (*n)++; }
    return &a[row];
}

static bool walk_fn(const uint8_t *e, uint32_t pos, void *u)
{
    (void)pos; (void)u;
    const stockcp_map_t *m = &k_map;
    uint8_t t = e[0], c = e[1];
    uint16_t row = (uint16_t)(e[2] | e[3] << 8);
    int len = e[4] | e[5] << 8;
    const uint8_t *v = e + 6;
    if(c == 0xFF) { return true; }                                  /* list row count */
    if(t == m->t_radioid && row < MAX_RID) {
        if(row >= s_nrid) { for(uint16_t r = s_nrid; r <= row; r++) { s_rid_id[r] = 0; s_rid_hash[r] = 0; } s_nrid = row + 1u; }
        if(c == 1) { s_rid_id[row] = dec_n(v, len); }
        if(c == 2) { s_rid_hash[row] = name_hash(v, len); }
#if BP_LIST
    } else if(t == m->t_bp && row < NBP) {           /* No. first in each row; kept per list row, a later radio edit may redo one */
        if(c == 0) { uint32_t n = dec_n(v, len); s_bp_no[row] = n >= 1 && n <= NBP ? (uint8_t)n : 0; }
        if(c == 1) { uint8_t b[2]; s_bp_lkey[row] = hex_bytes(v, len, b, 2) == 2 ? (uint16_t)(b[0] << 8 | b[1]) : 0xFFFF; }
        if(c == 2) { s_bp_lid[row] = len ? (uint16_t)dec_n(v, len) : 0xFFFF; }
#else
    } else if(t == m->t_bp && row < NBP) {
        if(c == 1) { s_bp_key[row] = (uint16_t)dec_n(v, len); }
        if(c == 2) { s_bp_id[row] = (uint16_t)dec_n(v, len); }
#endif
    } else if(t == m->t_aes || t == m->t_arc4) {
        bool aes = t == m->t_aes;
        tkey_t *k = aes ? tkey_row(s_aes, &s_naes, row) : tkey_row(s_arc4, &s_narc4, row);
        if(k) {
            if(c == 0) { k->id = (uint8_t)dec_n(v, len); }
            if(c == (aes ? 2 : 1)) { k->hex = v; k->len = (uint8_t)(len > 255 ? 255 : len); }
        }
    } else if(t == m->t_nxenc && row < NNX) {
        if(c == 0) { s_nx_valid[row] = on_text(v, len); }
        if(c == 1) { s_nx_code[row] = (uint16_t)dec_n(v, len); }
        if(c == 2) { s_nx_kid[row] = (uint8_t)dec_n(v, len); }
    } else if(t == m->t_nxset && row == 0) {
        if(c == 0) { uint32_t x = dec_n(v, len); s_nx_own = x > 1 && x <= 0xFFEFu ? (uint16_t)x : 0; }
        if(c == 40) { s_nx_moni = on_text(v, len) ? (uint8_t)(s_nx_moni | STOCKCP_NXMONI_ON) : (uint8_t)(s_nx_moni & ~STOCKCP_NXMONI_ON); }
        if(c == 41) { s_nx_moni = dec_n(v, len) == 0 ? (uint8_t)(s_nx_moni | STOCKCP_NXMONI_ANY_RAN) : (uint8_t)(s_nx_moni & ~STOCKCP_NXMONI_ANY_RAN); }
        if(c == 42) { s_nx_moni = dec_n(v, len) == 0 ? (uint8_t)(s_nx_moni | STOCKCP_NXMONI_ANY_ID) : (uint8_t)(s_nx_moni & ~STOCKCP_NXMONI_ANY_ID); }
    } else if(t == T_CHAN && (c == m->c_rid || c == m->c_enctype || c == m->c_digenc || c == m->c_arc4 || c == m->c_nxenc)) {
        chx_t *x = chx_for(row);
        if(!x) { return true; }
        if(c == m->c_rid) {
            uint16_t h = name_hash(v, len);
            for(uint16_t r = 0; r < s_nrid; r++) { if(s_rid_hash[r] == h && s_rid_id[r]) { x->rid = (uint8_t)r; break; } }
        }
        if(c == m->c_enctype) {
            bool enh = len > 0 && (v[0] == 'E' || v[0] == 'e' || v[0] == '1');
            x->flags = (uint8_t)(enh ? x->flags | CHX_ENH : x->flags & ~CHX_ENH);
        }
        if(c == m->c_digenc) { uint32_t n = tail_num(v, len); x->digenc = n <= 255 ? (uint8_t)n : 0; }
        if(c == m->c_arc4) { uint32_t n = tail_num(v, len); x->arc4 = n <= 255 ? (uint8_t)n : 0; }
#if HAVE_NX
        if(c == m->c_nxenc) { uint32_t n = tail_num(v, len); x->nxenc = n <= 32 ? (uint8_t)n : 0; }
#endif
    }
    return true;
}

static uint8_t add_key(uint8_t kind, int row)
{
    if(s_nkeys >= MAX_KEYS || cfwcp_own_key_count() + s_nkeys >= 0xFE) { return 0xFF; }
    s_kref[s_nkeys].kind = kind; s_kref[s_nkeys].row = (uint8_t)row;
    return s_nkeys++;
}

/* one shared buffer, overwritten by the next call: every caller copies the key at once (main loop only) */
const cp_key_t *stockcp_key(int i)
{
    static cp_key_t k;
    if(!cfwcp_valid() || i < 0 || i >= s_nkeys) { return NULL; }
    uint8_t r = s_kref[i].row;
    memset(&k, 0, sizeof k);
    k.kind = s_kref[i].kind;
    switch(k.kind) {
    case 0:                                          /* BP key id = Encryption ID if <= 255, else the row number */
        k.key_id = s_bp_id[r] <= 255 ? s_bp_id[r] : (uint16_t)(r + 1);
        k.len = 2; k.key[0] = (uint8_t)(s_bp_key[r] >> 8); k.key[1] = (uint8_t)s_bp_key[r];
        put_name(k.name, "BP ", (uint32_t)r + 1u);
        break;
    case 7:
        k.key_id = s_nx_kid[r];
        k.len = 2; k.key[0] = (uint8_t)(s_nx_code[r] >> 8 & 0x7F); k.key[1] = (uint8_t)s_nx_code[r];
        put_name(k.name, "NX SCR ", (uint32_t)r + 1u);
        break;
    default: {
        const tkey_t *t = k.kind == 1 ? &s_arc4[r] : &s_aes[r];
        int n = hex_bytes(t->hex, t->len, k.key, k.kind == 1 ? 16 : 32);
        if(n < 0) { return NULL; }
        k.key_id = t->id; k.len = (uint8_t)n;
        put_name(k.name, k.kind == 1 ? "ARC4 " : "AES ", t->id);
        break; }
    }
    return &k;
}

void stockcp_init(void)
{
    const stockcp_map_t *m = &k_map;
    s_nrid = 0; s_naes = s_narc4 = 0; s_nchx = 0; s_nkeys = 0; s_bp_ref = 0;
    s_nx_own = 0; s_nx_moni = STOCKCP_NXMONI_ANY_RAN | STOCKCP_NXMONI_ANY_ID;   /* stock defaults: off, Any, Any */
    for(int r = 0; r < NBP; r++) {
        s_bp_key[r] = s_bp_id[r] = 0xFFFF;
        s_bp_idx[r] = 0xFF;
#if BP_LIST
        s_bp_no[r] = 0; s_bp_lkey[r] = s_bp_lid[r] = 0xFFFF;
#endif
    }
    for(int r = 0; r < NNX; r++) {
        s_nx_valid[r] = 0; s_nx_code[r] = 0; s_nx_kid[r] = (uint8_t)(r + 1);
        s_nx_idx[r] = 0xFF;
    }
    memset(s_aes_idx, 0xFF, sizeof s_aes_idx);
    memset(s_arc4_idx, 0xFF, sizeof s_arc4_idx);
    uint32_t cnt = 0;
    const void *p = cfwcp_section(CP_SEC_USERDB, sizeof(cp_user_t), &cnt);
    s_user = p; s_nuser = p ? cnt : 0;
    if(!cfwcp_valid()) { return; }
    cfwcp_stock_walk(walk_fn, NULL);
#if BP_LIST
    for(int r = 0; r < NBP; r++) {
        if(s_bp_no[r]) { s_bp_key[s_bp_no[r] - 1u] = s_bp_lkey[r]; s_bp_id[s_bp_no[r] - 1u] = s_bp_lid[r]; }
    }
#endif
    for(int i = 0; i < s_nchx; i++) { if(s_chx[i].digenc && s_chx[i].digenc <= 32 && !(s_chx[i].flags & CHX_ENH)) { s_bp_ref |= 1u << (s_chx[i].digenc - 1u); } }
    /* keys: basic privacy rows that are set (key != 65535) or used by a channel; AES / ARC4 rows with a valid
     * key; NX rows marked valid. */
    uint8_t kb[32];
    if(m->t_bp != 0xFF) {
        for(int r = 0; r < NBP; r++) {
            if(s_bp_key[r] == 0xFFFF && !(s_bp_ref & (1u << r))) { continue; }
            s_bp_idx[r] = add_key(0, r);
        }
    }
    for(int r = 0; r < s_narc4; r++) {
        int n = s_arc4[r].hex ? hex_bytes(s_arc4[r].hex, s_arc4[r].len, kb, 16) : -1;
        if(n == 5) { s_arc4_idx[r] = add_key(1, r); }
    }
    for(int r = 0; r < s_naes; r++) {
        int n = s_aes[r].hex ? hex_bytes(s_aes[r].hex, s_aes[r].len, kb, 32) : -1;
        if(n == 16 || n == 32) { s_aes_idx[r] = add_key(n == 16 ? 2 : 3, r); }
    }
    if(m->t_nxenc != 0xFF) {
        for(int r = 0; r < NNX; r++) {
            if(s_nx_valid[r]) { s_nx_idx[r] = add_key(7, r); }
        }
    }
    memset(kb, 0, sizeof kb);
}

int stockcp_key_count(void) { return cfwcp_valid() ? s_nkeys : 0; }

static const chx_t *chx_find(int ch)
{
    if(!cfwcp_valid()) { return NULL; }
    int lo = 0, hi = s_nchx - 1;                     /* sorted: the CPS writes channel entries in channel order */
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        if(s_chx[mid].ch == ch) { return &s_chx[mid]; }
        if(s_chx[mid].ch < ch) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return NULL;
}

uint32_t stockcp_channel_radio_id(int ch)
{
    const chx_t *x = chx_find(ch);
    return x && x->rid != 0xFF && x->rid < s_nrid ? s_rid_id[x->rid] : 0;
}

/* ARC4 N (ARC4 key id N) wins; else Digital Encryption N: Enhanced = AES key id N, Normal = Encryption Code row N.
 * NXDN: NxdnEncry N = NX Encryption Code row N. */
uint8_t stockcp_channel_key(int ch, uint8_t mode)
{
    const chx_t *x = chx_find(ch);
    if(!x) { return 0xFF; }
#if HAVE_NX
    if(x->nxenc) { return mode == CFW_MODE_NXDN ? s_nx_idx[x->nxenc - 1u] : 0xFF; }
#endif
    if(mode != CFW_MODE_DMR) { return 0xFF; }
    if(x->arc4) {
        for(int r = 0; r < s_narc4; r++) { if(s_arc4[r].id == x->arc4 && s_arc4_idx[r] != 0xFF) { return s_arc4_idx[r]; } }
    } else if(x->digenc && (x->flags & CHX_ENH)) {
        for(int r = 0; r < s_naes; r++) { if(s_aes[r].id == x->digenc && s_aes_idx[r] != 0xFF) { return s_aes_idx[r]; } }
    } else if(x->digenc && x->digenc <= NBP) {
        return s_bp_idx[x->digenc - 1u];
    }
    return 0xFF;
}

uint16_t stockcp_nxdn_own_id(void) { return cfwcp_valid() ? s_nx_own : 0; }
uint8_t  stockcp_nxdn_moni(void) { return cfwcp_valid() ? s_nx_moni : 0; }

/* ---------------- NX Talk Group (on demand) ---------------- */
typedef struct { uint32_t id; int16_t row; bool forbid; char name[17]; } tg_t;
static bool tg_fn(const uint8_t *e, uint32_t pos, void *u)
{
    (void)pos;
    tg_t *g = u;
    uint16_t row = (uint16_t)(e[2] | e[3] << 8);
    int len = e[4] | e[5] << 8;
    if(e[0] != k_map.t_nxtg || e[1] == 0xFF) { return true; }
    if(e[1] == 0) {                                   /* RADIO_ID: first column of a row */
        if(g->row >= 0 && row != g->row) { return false; }       /* past the matching row */
        if(dec_n(e + 6, len) == g->id && g->row < 0) { g->row = (int16_t)row; }
        return true;
    }
    if(row != g->row) { return true; }
    if((e[1] == 1 || (e[1] == 2 && !g->name[0])) && len) {      /* CALLSIGN, else FIRST_NAME */
        int k = len < 16 ? len : 16;
        memcpy(g->name, e + 6, (size_t)k); g->name[k] = 0;
        while(k && g->name[k - 1] == ' ') { g->name[--k] = 0; }
    }
    if(e[1] == 8) { g->forbid = on_text(e + 6, len); }
    return true;
}

bool stockcp_nxdn_tg(uint32_t id, char *name, int n, bool *tx_forbid)
{
    static tg_t c = { 0xFFFFFFFFu, -1, false, "" };
    static uint32_t gen = 0xFFFFFFFFu;
    if(!cfwcp_valid() || k_map.t_nxtg == 0xFF) { return false; }
    if(c.id != id || gen != cfwcp_crc()) {
        memset(&c, 0, sizeof c);
        c.id = id; c.row = -1;
        cfwcp_stock_walk(tg_fn, &c);
        gen = cfwcp_crc();
    }
    if(c.row < 0) { return false; }
    if(name && n > 0) { int k = 0; while(k < n - 1 && c.name[k]) { name[k] = c.name[k]; k++; } name[k] = 0; }
    if(tx_forbid) { *tx_forbid = c.forbid; }
    return true;
}

/* ---------------- user database (section 16) ---------------- */
int stockcp_user_count(void) { return cfwcp_valid() ? (int)s_nuser : 0; }

const cp_user_t *stockcp_user_find(uint8_t mode, uint32_t id)
{
    if(!cfwcp_valid() || !s_user) { return NULL; }
    uint32_t key = (uint32_t)mode << 24 | (id & 0xFFFFFFu);
    uint32_t lo = 0, hi = s_nuser;
    while(lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if(s_user[mid].key == key) { return &s_user[mid]; }
        if(s_user[mid].key < key) { lo = mid + 1; } else { hi = mid; }
    }
    return NULL;
}

static int cat(char *out, int k, int n, const char *s, int max)
{
    for(int i = 0; i < max && s[i] && k < n - 1; i++) { out[k++] = s[i]; }
    while(k && out[k - 1] == ' ') { k--; }
    out[k] = 0;
    return k;
}

bool stockcp_id_name(uint8_t mode, uint32_t id, bool group, char *out, int n)
{
    if(n < 1 || !id || !cfwcp_valid()) { return false; }
    out[0] = 0;
    if(!group) {
        const cp_user_t *u = stockcp_user_find(mode == CFW_MODE_NXDN ? CP_USER_NXDN : CP_USER_DMR, id);
        if(u) {
            int k = cat(out, 0, n, u->call, 8);
            if(u->name[0] && k < n - 2) { if(k) { out[k++] = ' '; } cat(out, k, n, u->name, 12); }
            return out[0] != 0;
        }
    } else if(mode == CFW_MODE_NXDN && stockcp_nxdn_tg(id, out, n, NULL) && out[0]) {
        return true;
    }
    for(int i = 0; i < cfwcp_contact_count(); i++) {             /* cfw contacts (section 5) */
        const cp_contact_t *c = cfwcp_contact(i);
        if(!c || c->id != id || (c->kind == 1) == group) { continue; }
        if(c->mode != 0xFF && c->mode != mode) { continue; }
        cat(out, 0, n, c->name, 16);
        return out[0] != 0;
    }
    return false;
}

/* glue.h hooks for proto_nxdn */
bool cfw_nxdn_any_ran(void)
{
    uint8_t m = stockcp_nxdn_moni();
    return (m & STOCKCP_NXMONI_ON) && (m & STOCKCP_NXMONI_ANY_RAN);
}
bool cfw_nxdn_tx_forbidden(uint32_t group)
{
    bool f = false;
    return stockcp_nxdn_tg(group, NULL, 0, &f) && f;
}

/* caller line for the call screen: "<callsign> <name>" of the source (user database / contacts), else the name of
 * the destination group; false = nothing known */
bool stockcp_call_line(uint8_t mode, uint32_t src, uint32_t dst, bool private_call, char *out, int n)
{
    if(stockcp_id_name(mode, src, false, out, n)) { return true; }
    return dst && stockcp_id_name(mode, dst, !private_call, out, n);
}
