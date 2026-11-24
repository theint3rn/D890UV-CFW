/* cfw codeplug store: see cfwcp.h and cfw_cps/docs/CODEPLUG.md.
 *
 * Storage rules (same as drivers/settings.c):
 *   - internal flash 0x08180000-0x081DFFFF only; program_qw()/erase_page() refuse anything else, so a bug here
 *     cannot touch the bootloader, the activation key, the cfw image, the settings page or the stock cache;
 *   - make HW=1 (CFW_HW_NO_SAVE) compiles the flash writes out: a real radio is read-only until the owner OKs it;
 *     make HW=1 HW_SAVE=1 keeps them (same range checks), for that OK only (pre-flash audit F10);
 *   - the external NAND is never written (the stock CPS codeplug stays as it is). */
#include <string.h>
#include <stddef.h>
#include "hal.h"
#include "cfwcp.h"
#ifndef CFW_FLASH_EXTERNAL
#include "stm32u5.h"
#endif
#include "radio.h"
#include "voice.h"
#include "stockcp.h"

static int8_t s_state = -1;              /* -1 not checked, 0 invalid, 1 valid */
static const cp_settings_t *s_set;
static const cp_channel_t  *s_ch;
static const cp_contact_t  *s_ct;
static const cp_trunksys_t *s_ts;
static const uint32_t      *s_p32;
static const cp_chext_t    *s_cx;
static const cp_key_t      *s_key;
static const cp_bandplan_t *s_bp;
static uint32_t s_nch, s_nct, s_nts, s_np32, s_ncx, s_nkey, s_nbp;
static const uint8_t *s_stock;
static uint32_t s_nstock;
static const cp_list_t *s_zone, *s_tgl;
static const cp_scanlist_t *s_scl;
static const uint16_t *s_p16;
static uint32_t s_nzone, s_ntgl, s_nscl, s_np16;

#define HDR ((const cp_header_t *)(uintptr_t)CFWCP_BASE)

uint32_t cfwcp_crc32(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while(n--) {
        c ^= *b++;
        for(int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    }
    return ~c;
}

bool cfwcp_check(void)
{
    const cp_header_t *h = HDR;
    s_state = 0;
    s_set = NULL; s_ch = NULL; s_ct = NULL; s_ts = NULL; s_p32 = NULL; s_cx = NULL; s_key = NULL; s_bp = NULL;
    s_nch = s_nct = s_nts = s_np32 = s_ncx = s_nkey = s_nbp = 0;
    s_stock = NULL; s_nstock = 0;
    s_zone = s_tgl = NULL; s_scl = NULL; s_p16 = NULL; s_nzone = s_ntgl = s_nscl = s_np16 = 0;
    if(h->magic != CFWCP_MAGIC || h->version != CFWCP_VERSION || h->header_size != sizeof(cp_header_t)) { return false; }
    if(h->flags != CFWCP_MODEL_ID) { return false; }                    /* codeplug for another radio model */
    if(h->total_len < sizeof *h || h->total_len > CFWCP_SIZE || h->n_sections > 32) { return false; }
    if(sizeof *h + h->n_sections * sizeof(cp_section_t) > h->total_len) { return false; }
    if(cfwcp_crc32((const uint8_t *)h + 16, h->total_len - 16) != h->crc) { return false; }
    const cp_section_t *sec = (const cp_section_t *)(h + 1);
    for(unsigned i = 0; i < h->n_sections; i++) {
        const cp_section_t *s = &sec[i];
        if(s->offset & 3u || s->offset > h->total_len || (uint64_t)s->rec_size * s->count > h->total_len - s->offset) { return false; }
        const void *p = (const uint8_t *)h + s->offset;
        switch(s->id) {
        case CP_SEC_SETTINGS:  if(s->rec_size == sizeof *s_set && s->count >= 1) { s_set = p; } break;
        case CP_SEC_CHANNELS:  if(s->rec_size == sizeof *s_ch) { s_ch = p; s_nch = s->count; } break;
        case CP_SEC_CONTACTS:  if(s->rec_size == sizeof *s_ct) { s_ct = p; s_nct = s->count; } break;
        case CP_SEC_TRUNKSYS:  if(s->rec_size == sizeof *s_ts) { s_ts = p; s_nts = s->count; } break;
        case CP_SEC_POOL32:    if(s->rec_size == 4) { s_p32 = p; s_np32 = s->count; } break;
        case CP_SEC_CHEXT:     if(s->rec_size == sizeof *s_cx) { s_cx = p; s_ncx = s->count; } break;
        case CP_SEC_KEYS:      if(s->rec_size == sizeof *s_key) { s_key = p; s_nkey = s->count; } break;
        case CP_SEC_BANDPLAN:  if(s->rec_size == sizeof *s_bp) { s_bp = p; s_nbp = s->count; } break;
        case CP_SEC_STOCK:     if(s->rec_size == 1) { s_stock = p; s_nstock = s->count; } break;
        case CP_SEC_ZONES:     if(s->rec_size == sizeof *s_zone) { s_zone = p; s_nzone = s->count; } break;
        case CP_SEC_TGLISTS:   if(s->rec_size == sizeof *s_tgl) { s_tgl = p; s_ntgl = s->count; } break;
        case CP_SEC_SCANLISTS: if(s->rec_size == sizeof *s_scl) { s_scl = p; s_nscl = s->count; } break;
        case CP_SEC_POOL16:    if(s->rec_size == 2) { s_p16 = p; s_np16 = s->count; } break;
        default: break;
        }
    }
    if(!s_ch || s_nch == 0 || s_nch > 4000) { stockcp_init(); return false; }
    s_state = 1;
    stockcp_init();                                            /* stock DMR / NXDN sections (stockcp.c) */
    return true;
}

const void *cfwcp_section(uint16_t id, uint16_t rec_size, uint32_t *count)
{
    if(s_state != 1) { return NULL; }
    const cp_header_t *h = HDR;
    const cp_section_t *sec = (const cp_section_t *)(h + 1);
    for(unsigned i = 0; i < h->n_sections; i++) {
        if(sec[i].id == id && sec[i].rec_size == rec_size) {
            if(count) { *count = sec[i].count; }
            return (const uint8_t *)h + sec[i].offset;
        }
    }
    return NULL;
}

bool cfwcp_valid(void) { if(s_state < 0) { cfwcp_check(); } return s_state == 1; }
int  cfwcp_channel_count(void) { return cfwcp_valid() ? (int)s_nch : 0; }
const cp_settings_t *cfwcp_settings(void) { return cfwcp_valid() ? s_set : NULL; }
uint32_t cfwcp_len(void) { return cfwcp_valid() ? HDR->total_len : 0; }
uint32_t cfwcp_crc(void) { return cfwcp_valid() ? HDR->crc : 0; }
uint32_t cfwcp_generation(void) { return cfwcp_valid() ? HDR->generation : 0; }

static void copy_str(char *dst, const char *src, int n)   /* dst has n+1 bytes */
{
    int i = 0;
    for(; i < n && src[i]; i++) { dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? src[i] : '?'; }
    dst[i] = 0;
}

static uint16_t ctcss_of(uint16_t tone) { return (tone && !(tone & 0x8000u)) ? tone : 0; }

bool cfwcp_channel(int i, channel_t *c)
{
    if(!cfwcp_valid() || i < 0 || (uint32_t)i >= s_nch) { return false; }
    const cp_channel_t *r = &s_ch[i];
    memset(c, 0, sizeof *c);
    c->src = CH_SRC_CODEPLUG;
    c->cp_slot = (uint16_t)i;
    copy_str(c->name, r->name, 16);
    c->rx_hz = r->rx_hz;
    c->tx_hz = r->tx_hz;
    c->mode = r->mode < CFW_MODE_COUNT ? (cfw_mode_t)r->mode : CFW_MODE_FM;
    c->wide = r->flags & 1u;
    c->trunk = (r->flags >> 1) & 1u;
    c->scan_skip = (r->flags >> 2) & 1u;
    c->power = r->power & 3u;
    c->scanlist = r->scanlist;
    c->ctcss_dhz = ctcss_of(r->tx_tone) ? ctcss_of(r->tx_tone) : ctcss_of(r->rx_tone);
    uint16_t ts = 0xFFFF, contact = 0xFFFF;
    switch(c->mode) {
    case CFW_MODE_DMR:
        c->color_code = r->m.dmr.cc & 0x0F;
        c->slot = r->m.dmr.slot == 2 ? 2 : 1;
        c->talkgroup = r->m.dmr.tg;
        contact = r->m.dmr.contact;
        ts = r->m.dmr.trunksys;
        break;
    case CFW_MODE_P25P1:
    case CFW_MODE_P25P2:
        c->nac = r->m.p25.nac & 0xFFF;
        c->slot = r->m.p25.slot == 2 ? 2 : 1;
        c->talkgroup = r->m.p25.tg;
        c->wacn = r->m.p25.wacn & 0xFFFFF;
        c->sysid = r->m.p25.sysid & 0xFFF;
        contact = r->m.p25.contact;
        ts = r->m.p25.trunksys;
        break;
    case CFW_MODE_NXDN:
        c->ran = r->m.nxdn.ran & 0x3F;
        c->talkgroup = r->m.nxdn.group;
        contact = r->m.nxdn.contact;
        ts = r->m.nxdn.trunksys;
        break;
    case CFW_MODE_DSTAR:
        copy_str(c->ur, r->m.dstar.ur, 8);
        copy_str(c->rpt1, r->m.dstar.rpt1, 8);
        copy_str(c->rpt2, r->m.dstar.rpt2, 8);
        break;
    case CFW_MODE_YSF:
        c->dgid = r->m.ysf.dgid;
        break;
    default:
        break;
    }
    if(!c->talkgroup && contact < s_nct) { c->talkgroup = s_ct[contact].id; }
    if(ts < s_nts) {                     /* trunk system: control channel, P25 WACN / system id / NAC */
        const cp_trunksys_t *t = &s_ts[ts];
        c->trunk = true;
        if(t->type == 1) {
            if(t->nac_cc) { c->nac = t->nac_cc & 0xFFF; }
            if(t->wacn) { c->wacn = t->wacn & 0xFFFFF; }
            if(t->sysid) { c->sysid = t->sysid & 0xFFF; }
        } else if(t->type == 2 && t->nac_cc) {
            c->color_code = t->nac_cc & 0x0F;
        } else if((t->type == 3 || t->type == 4) && t->nac_cc) {   /* NXDN Type-C / Type-D: system RAN */
            c->ran = t->nac_cc & 0x3F;
        }
        if(!c->rx_hz && t->cc_count && t->cc_first < s_np32) { c->rx_hz = s_p32[t->cc_first]; }
    }
    const cp_chext_t *x = cfwcp_chext(i);
    c->key = 0xFF; c->dstar_rpt = 0xFFFF;
    if(x) {                                                    /* channel digital extensions (cfw-platform) */
        c->talkaround = x->flags & 1u;                         /* TX on the RX frequency, direct-mode framing */
        c->wiresx = (x->flags >> 1) & 1u;
        c->dmr_tdma = (x->flags >> 2) & 1u;                    /* DMR: TDMA direct (dual capacity) simplex */
        c->vocoder = x->vocoder;
        c->key = x->key;
        c->nxdn96 = x->nxdn_rate == 1;
        c->dgid_tx = x->dgid_tx <= 99 ? (uint8_t)(x->dgid_tx + 1) : 0;
        c->ysf_mode = x->ysf_mode;
        c->dstar_rpt = x->dstar_rpt;
    }
    if(c->key == 0xFF) {                                       /* stock CPS channel encryption columns */
        uint8_t k = stockcp_channel_key(i, c->mode);
        if(k != 0xFF) { c->key = (uint8_t)(s_nkey + k); }
    }
    channel_mode_defaults(c);
    return true;
}

const cp_chext_t *cfwcp_chext(int ch)
{
    if(!cfwcp_valid()) { return NULL; }
    for(uint32_t k = 0; k < s_ncx; k++) {                     /* sorted by channel: CPS writes them in order */
        if(s_cx[k].channel == ch) { return &s_cx[k]; }
        if(s_cx[k].channel > ch) { break; }
    }
    return NULL;
}

/* walks the stock entries (CPS entries, then the radio edits journal); calls fn for each, stops at the zero tail */
static void stock_walk(bool (*fn)(const uint8_t *e, uint32_t pos, void *u), void *u)
{
    if(!cfwcp_valid() || !s_stock) { return; }
    for(uint32_t p = 0; p + 6 <= s_nstock; ) {                 /* entries: u8 table, u8 col, u16 row, u16 len, text */
        uint32_t al = (p + 3u) & ~3u;                            /* the journal starts 4-aligned after the CPS */
        if(al != p && al < s_nstock && s_stock[al] >= 0xE0u && s_stock[al] < 0xF0u) {   /* entries: skip their */
            bool pad = true;                                     /* 1-3 zero pad bytes (as cps/stock.py does) */
            for(uint32_t k = p; k < al; k++) { pad = pad && s_stock[k] == 0; }
            if(pad) { p = al; continue; }
        }
        const uint8_t *e = s_stock + p;
        uint16_t len = (uint16_t)(e[4] | e[5] << 8);
        if(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0 && len == 0) { break; }
        if(p + 6u + len > s_nstock) { break; }
        if(!fn(e, p, u)) { break; }
        p += 6u + len;
    }
}

void cfwcp_stock_walk(cfwcp_walk_fn fn, void *u) { stock_walk(fn, u); }

typedef struct { int table, col, row; char *buf; int n; bool found; } sv_t;
static bool sv_fn(const uint8_t *e, uint32_t pos, void *u)
{
    sv_t *v = u;
    (void)pos;
    uint16_t row = (uint16_t)(e[2] | e[3] << 8), len = (uint16_t)(e[4] | e[5] << 8);
    if(e[0] == v->table && e[1] == v->col && row == v->row) {   /* keep going: a later entry (radio edit) wins */
        int k = len < v->n - 1 ? len : v->n - 1;
        memcpy(v->buf, e + 6, (size_t)k);
        v->buf[k] = 0;
        v->found = true;
    }
    return true;
}

bool cfwcp_stock_value(int table, int col, char *buf, int n)
{
    if(n < 1) { return false; }
    sv_t v = { table, col, 0, buf, n, false };
    stock_walk(sv_fn, &v);
    return v.found;
}

bool cfwcp_stock_value_row(int table, int col, int row, char *buf, int n)
{
    if(n < 1) { return false; }
    sv_t v = { table, col, row, buf, n, false };
    stock_walk(sv_fn, &v);
    return v.found;
}

int cfwcp_zone_count(void) { return cfwcp_valid() ? (int)s_nzone : 0; }
const cp_list_t *cfwcp_zone(int i) { return (cfwcp_valid() && i >= 0 && (uint32_t)i < s_nzone) ? &s_zone[i] : NULL; }
int cfwcp_tglist_count(void) { return cfwcp_valid() ? (int)s_ntgl : 0; }
const cp_list_t *cfwcp_tglist(int i) { return (cfwcp_valid() && i >= 0 && (uint32_t)i < s_ntgl) ? &s_tgl[i] : NULL; }
int cfwcp_scanlist_count(void) { return cfwcp_valid() ? (int)s_nscl : 0; }
const cp_scanlist_t *cfwcp_scanlist(int i) { return (cfwcp_valid() && i >= 0 && (uint32_t)i < s_nscl) ? &s_scl[i] : NULL; }
int cfwcp_contact_count(void) { return cfwcp_valid() ? (int)s_nct : 0; }
const cp_contact_t *cfwcp_contact(int i) { return (cfwcp_valid() && i >= 0 && (uint32_t)i < s_nct) ? &s_ct[i] : NULL; }
uint16_t cfwcp_member(uint32_t first, int k)
{
    if(!cfwcp_valid() || k < 0 || first + (uint32_t)k >= s_np16) { return 0xFFFF; }
    return s_p16[first + (uint32_t)k];
}

/* ---------------- radio edits journal ---------------- */
typedef struct { uint32_t mark, zero_tail; } jpos_t;
static bool jpos_fn(const uint8_t *e, uint32_t pos, void *u)
{
    jpos_t *j = u;
    if(e[0] == CP_JT_MARK) { j->mark = pos; return false; }
    j->zero_tail = pos + 6u + (uint32_t)(e[4] | e[5] << 8);
    return true;
}

/* offset in section 10 where the journal starts (the marker, or the end of the CPS entries) */
static uint32_t journal_start(void)
{
    jpos_t j = { 0xFFFFFFFFu, 0 };
    stock_walk(jpos_fn, &j);
    if(j.mark != 0xFFFFFFFFu) { return j.mark; }
    return (j.zero_tail + 3u) & ~3u;
}

int cfwcp_journal_spare(void)
{
    if(!cfwcp_valid() || !s_stock) { return 0; }
    uint32_t st = journal_start();
    return st < s_nstock ? (int)(s_nstock - st) : 0;
}

int cfwcp_journal_read(cp_edit_t *out, int max)
{
    if(!cfwcp_valid() || !s_stock) { return 0; }
    uint32_t p = journal_start(), n = 0;
    if(p + 6 > s_nstock || s_stock[p] != CP_JT_MARK) { return 0; }
    for(p += 6; p + 6 <= s_nstock && (int)n < max; ) {
        const uint8_t *e = s_stock + p;
        uint16_t len = (uint16_t)(e[4] | e[5] << 8);
        if(e[0] == 0 || e[0] == CP_JT_CRCFIX || p + 6u + len > s_nstock) { break; }
        cp_edit_t *o = &out[n++];
        o->table = e[0]; o->col = e[1]; o->row = (uint16_t)(e[2] | e[3] << 8);
        int k = len < sizeof o->text - 1 ? len : (int)sizeof o->text - 1;
        memcpy(o->text, e + 6, (size_t)k);
        o->text[k] = 0;
        p += 6u + len;
    }
    return (int)n;
}

/* CRC-32 one bit backwards (inverse of c = (c >> 1) ^ (POLY & -(c & 1))) */
static uint32_t crc_unshift(uint32_t c)
{
    return (c & 0x80000000u) ? ((c ^ 0xEDB88320u) << 1) | 1u : c << 1;
}
static uint32_t crc_step(uint32_t c, uint8_t b)
{
    c ^= b;
    for(int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return c;
}
static uint32_t crc_unstep(uint32_t c, uint8_t b)
{
    for(int k = 0; k < 8; k++) { c = crc_unshift(c); }
    return c ^ b;
}

#define JOURNAL_MAX 2048u
static uint8_t s_jb[JOURNAL_MAX];
static uint8_t s_page[CFWCP_PAGE];

/* Power-loss safety (redo copy). A journal write rewrites one or two 8 KB codeplug pages; power lost during the
 * erase / program would leave the codeplug with a bad CRC. So the new page images go first into a redo area at the
 * end of the codeplug region (the last 3 pages, only used while total_len leaves them free), then a 32-byte record
 * (magic, generation, header CRC, pages, image CRC, own CRC) is written as the commit, then the codeplug pages are
 * rewritten, then the record page is erased. cfwcp_recover() (boot) finishes an interrupted rewrite from the copy. */
#define JBK_MAGIC      0x314B424Au          /* "JBK1" */
#define JBK_MAX_PAGES  2u
#define JBK_HDR_OFF    (CFWCP_SIZE - CFWCP_PAGE)
#define JBK_IMG_OFF    (CFWCP_SIZE - (1u + JBK_MAX_PAGES) * CFWCP_PAGE)
typedef struct {
    uint32_t magic, generation, hdr_crc, total_len;
    uint16_t n_pages, first_page;
    uint32_t img_crc, self_crc;
    uint32_t pad;
} jbk_hdr_t;
_Static_assert(sizeof(jbk_hdr_t) == 32, "redo record");

#ifdef CFW_EMU
/* emulator test hook (cfw/tools/powercut_test.py): != 0 = stop the CPU (as if power were cut) on the N-th flash
 * erase / program step of the codeplug region from now on; counts down. */
volatile uint32_t g_dbg_flash_cut;
volatile uint8_t g_dbg_cp_boot;            /* set by cfwcp_recover: b0 codeplug valid after it, b1 record applied, b7 ran */
#define CP_BOOT(v) (g_dbg_cp_boot = (uint8_t)(0x80u | (v)))
static void cut_point(void) { if(g_dbg_flash_cut && --g_dbg_flash_cut == 0) { for(;;) { } } }
#else
static inline void cut_point(void) { }
#define CP_BOOT(v) ((void)0)
#endif

/* page pg of the codeplug with the journal bytes [jabs, jabs + jlen) taken from s_jb, into s_page */
static void page_image(uint32_t pg, uint32_t jabs, uint32_t jlen)
{
    uint32_t po = pg * CFWCP_PAGE;
    memcpy(s_page, (const uint8_t *)(uintptr_t)CFWCP_BASE + po, CFWCP_PAGE);
    for(uint32_t i = 0; i < CFWCP_PAGE; i++) {
        uint32_t a = po + i;
        if(a >= jabs && a < jabs + jlen) { s_page[i] = s_jb[a - jabs]; }
    }
}

static const jbk_hdr_t *jbk_record(void)
{
    const jbk_hdr_t *h = (const jbk_hdr_t *)(uintptr_t)(CFWCP_BASE + JBK_HDR_OFF);
    if(h->magic != JBK_MAGIC || h->self_crc != cfwcp_crc32(h, offsetof(jbk_hdr_t, self_crc))) { return NULL; }
    if(h->n_pages == 0 || h->n_pages > JBK_MAX_PAGES || h->total_len > JBK_IMG_OFF ||
       (uint32_t)(h->first_page + h->n_pages) * CFWCP_PAGE > JBK_IMG_OFF) { return NULL; }
    if(cfwcp_crc32((const void *)(uintptr_t)(CFWCP_BASE + JBK_IMG_OFF), h->n_pages * CFWCP_PAGE) != h->img_crc) { return NULL; }
    return h;
}

/* copy the record's page images into the codeplug, then erase the record page */
static bool jbk_apply(const jbk_hdr_t *hp)
{
    jbk_hdr_t h = *hp;                             /* the record page is erased below */
    bool ok = true;
    cfwcp_write_begin();
    for(uint32_t k = 0; k < h.n_pages; k++) {
        memcpy(s_page, (const void *)(uintptr_t)(CFWCP_BASE + JBK_IMG_OFF + k * CFWCP_PAGE), CFWCP_PAGE);
        ok = cfwcp_write((h.first_page + k) * CFWCP_PAGE, s_page, CFWCP_PAGE) && ok;
    }
    if(ok) { cfwcp_erase_page_off(JBK_HDR_OFF); }
    return ok;
}

/* boot: finish a journal rewrite that a power loss interrupted. Applies the record when the codeplug is invalid
 * (torn page) or still has the same generation / CRC (the rewrite had not started); a record that belongs to an
 * older codeplug (a CPS write since) is dropped. Returns true if the codeplug changed. */
bool cfwcp_recover(void)
{
    if(!cfwcp_write_allowed()) { CP_BOOT(cfwcp_check() ? 1u : 0u); return false; }
    const jbk_hdr_t *h = jbk_record();
    if(!h) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)(CFWCP_BASE + JBK_HDR_OFF);
        if(w[0] == JBK_MAGIC) { cfwcp_erase_page_off(JBK_HDR_OFF); }   /* torn record: the rewrite never began */
        CP_BOOT(cfwcp_check() ? 1u : 0u);
        return false;
    }
    bool valid = cfwcp_check();
    bool mine = !valid || (HDR->generation == h->generation && HDR->crc == h->hdr_crc && HDR->total_len == h->total_len);
    if(mine) {
        jbk_apply(h);
        s_state = -1;
        CP_BOOT((cfwcp_check() ? 1u : 0u) | 2u);
        return true;
    }
    cfwcp_erase_page_off(JBK_HDR_OFF);
    CP_BOOT(cfwcp_check() ? 1u : 0u);
    return false;
}

bool cfwcp_journal_write(const cp_edit_t *e, int n)
{
    if(!cfwcp_write_allowed() || !cfwcp_valid() || !s_stock) { return false; }
    const uint8_t *base = (const uint8_t *)(uintptr_t)CFWCP_BASE;
    uint32_t total = HDR->total_len, crc = HDR->crc;
    uint32_t js = journal_start();
    uint32_t jabs = (uint32_t)(s_stock - base) + js, jlen = s_nstock - js;
    if(jlen > JOURNAL_MAX) { jlen = JOURNAL_MAX; }
    /* build: marker, edits, CRC fix entry; the rest stays zero */
    memset(s_jb, 0, sizeof s_jb);
    uint32_t q = 0;
    if(q + 6u > jlen) { return false; }
    s_jb[q] = CP_JT_MARK; q += 6u;
    for(int i = 0; i < n; i++) {
        uint32_t len = (uint32_t)strlen(e[i].text);
        if(q + 6u + len + 10u > jlen) { return false; }
        s_jb[q] = e[i].table; s_jb[q + 1] = e[i].col;
        s_jb[q + 2] = (uint8_t)e[i].row; s_jb[q + 3] = (uint8_t)(e[i].row >> 8);
        s_jb[q + 4] = (uint8_t)len; s_jb[q + 5] = 0;
        memcpy(&s_jb[q + 6], e[i].text, len);
        q += 6u + len;
    }
    if(q + 10u > jlen) { return false; }
    s_jb[q] = CP_JT_CRCFIX; s_jb[q + 4] = 4;
    uint32_t fix = q + 6u;                         /* 4 bytes chosen so the blob CRC stays = header CRC */
    /* forward CRC up to the fix bytes, backward from the wanted final value down to just after them */
    uint32_t c = 0xFFFFFFFFu;
    for(uint32_t i = 16; i < jabs; i++) { c = crc_step(c, base[i]); }
    for(uint32_t i = 0; i < fix; i++) { c = crc_step(c, s_jb[i]); }
    uint32_t r = ~crc;
    for(uint32_t i = total; i > jabs + jlen; i--) { r = crc_unstep(r, base[i - 1]); }
    for(uint32_t i = jlen; i > fix + 4u; i--) { r = crc_unstep(r, s_jb[i - 1]); }
    for(int k = 0; k < 32; k++) { r = crc_unshift(r); }
    uint32_t x = c ^ r;
    s_jb[fix] = (uint8_t)x; s_jb[fix + 1] = (uint8_t)(x >> 8); s_jb[fix + 2] = (uint8_t)(x >> 16); s_jb[fix + 3] = (uint8_t)(x >> 24);
    if(memcmp(base + jabs, s_jb, jlen) == 0) { return true; }      /* nothing changed */
    uint32_t pg0 = jabs / CFWCP_PAGE, pg1 = (jabs + jlen - 1u) / CFWCP_PAGE;
    if(pg1 - pg0 + 1u > JBK_MAX_PAGES || total > JBK_IMG_OFF) { return false; }   /* no room for the redo copy */
    /* 1. redo copy: the new page images, then the record (written last = commit) */
    jbk_hdr_t h;
    memset(&h, 0, sizeof h);
    h.magic = JBK_MAGIC; h.generation = HDR->generation; h.hdr_crc = crc; h.total_len = total;
    h.n_pages = (uint16_t)(pg1 - pg0 + 1u); h.first_page = (uint16_t)pg0;
    cfwcp_write_begin();
    bool ok = true;
    uint32_t ic = 0xFFFFFFFFu;
    for(uint32_t k = 0; k < h.n_pages; k++) {
        page_image(pg0 + k, jabs, jlen);
        for(uint32_t i = 0; i < CFWCP_PAGE; i++) { ic = crc_step(ic, s_page[i]); }
        ok = cfwcp_write(JBK_IMG_OFF + k * CFWCP_PAGE, s_page, CFWCP_PAGE) && ok;
    }
    h.img_crc = ~ic;
    h.self_crc = cfwcp_crc32(&h, offsetof(jbk_hdr_t, self_crc));
    memset(s_page, 0xFF, 32);
    memcpy(s_page, &h, sizeof h);
    ok = ok && cfwcp_write(JBK_HDR_OFF, s_page, 32);
    if(!ok) { return false; }                      /* codeplug untouched */
    /* 2. rewrite the codeplug page(s) from the copy; 3. retire the record */
    ok = jbk_apply(&h);
    return cfwcp_check() && ok;
}

int cfwcp_own_key_count(void) { return cfwcp_valid() ? (int)s_nkey : 0; }
int cfwcp_key_count(void) { return cfwcp_valid() ? (int)s_nkey + stockcp_key_count() : 0; }
const cp_key_t *cfwcp_key(int i)
{
    if(!cfwcp_valid() || i < 0) { return NULL; }
    if((uint32_t)i < s_nkey) { return &s_key[i]; }
    return stockcp_key(i - (int)s_nkey);
}

int cfwcp_bandplans(int ts, const cp_bandplan_t **out, int max)
{
    int n = 0;
    if(!cfwcp_valid()) { return 0; }
    for(uint32_t k = 0; k < s_nbp && n < max; k++) {
        if(s_bp[k].trunksys == ts) { out[n++] = &s_bp[k]; }
    }
    return n;
}

void cfwcp_apply_radio(void)
{
    const cp_settings_t *s = cfwcp_settings();
    if(!s) { return; }
    if(s->dmr_id) { g_radio.my_id = s->dmr_id; }
    if(s->callsign[0] && s->callsign[0] != ' ') { copy_str(g_radio.my_call, s->callsign, 8); }
    if(s->step_hz >= 1000 && s->step_hz <= 100000) { g_radio.step_hz = s->step_hz; }
    g_radio.rx_invert = s->flags & 1u;
    g_radio.deemph_us = s->deemph_us <= 2000 ? s->deemph_us : 0;
    if(s->test_tone_hz <= 3000) { voice_set_test_tone(s->test_tone_hz); }
    if(s->start_channel != 0xFFFF && s->start_channel < (uint32_t)channels_count()) { g_radio.channel = s->start_channel; }
    if(s->flags & 8u) { g_radio.vfo = true; }
}

/* ---------------- writing ---------------- */

static uint64_t s_erased;                /* bit per page erased in this write session */

bool cfwcp_write_allowed(void)
{
#ifdef CFW_HW_NO_SAVE
    return false;
#else
    return true;
#endif
}

void cfwcp_write_begin(void) { s_erased = 0; s_state = -1; }

#if !defined(CFW_HW_NO_SAVE) && defined(CFW_FLASH_EXTERNAL)
/* board flash driver (board_cfg.h): erase one CFWCP_PAGE page / program 16 bytes, both address-guarded there too */
static void unlock(void) { cfw_flash_unlock(); }
static void lock(void) { cfw_flash_lock(); }
static void erase_page(unsigned page)
{
    if(page >= CFWCP_SIZE / CFWCP_PAGE) { return; }
    cfw_flash_erase(CFWCP_BASE + page * CFWCP_PAGE);
}
static void program_qw(uint32_t addr, const uint32_t w[4])
{
    if(addr < CFWCP_BASE || addr + 16u > CFWCP_BASE + CFWCP_SIZE || (addr & 15u)) { return; }
    cfw_flash_program16(addr, w);
}
#elif !defined(CFW_HW_NO_SAVE)
static void lock(void) { FLASH_NSCR = 1u << 31; }
static void wait_idle(void)
{
    for(uint32_t n = 0; n < 1000000u && (FLASH_NSSR & (1u << 16)); n++) { wdg_refresh(); }   /* BSY (F3: erase is long) */
}

static void unlock(void)
{
    if(FLASH_NSCR & (1u << 31)) {
        FLASH_NSKEYR = 0x45670123u;
        FLASH_NSKEYR = 0xCDEF89ABu;
    }
}

static void erase_page(unsigned page)            /* page 0..47 of the codeplug region */
{
    if(page >= CFWCP_SIZE / CFWCP_PAGE) { return; }
    wait_idle();
    FLASH_NSSR = 0xFFFFFFFFu;
    FLASH_NSCR = (1u << 1) | ((CFWCP_FIRST_PAGE + page) << 3) | (1u << 11);   /* PER, PNB, BKER = bank 2 */
    FLASH_NSCR |= 1u << 16;                                                    /* STRT */
    wait_idle();
    FLASH_NSCR = 0;
}

static void program_qw(uint32_t addr, const uint32_t w[4])
{
    if(addr < CFWCP_BASE || addr + 16u > CFWCP_BASE + CFWCP_SIZE || (addr & 15u)) { return; }
    FLASH_NSSR = 0xFFFFFFFFu;
    FLASH_NSCR = 1u;                                                           /* PG */
    for(int i = 0; i < 4; i++) { *(volatile uint32_t *)(uintptr_t)(addr + 4u * (uint32_t)i) = w[i]; }
    wait_idle();
    FLASH_NSCR = 0;
}
#endif

bool cfwcp_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    if((off & 15u) || (len & 15u) || len == 0 || off >= CFWCP_SIZE || len > CFWCP_SIZE - off) { return false; }
#ifdef CFW_HW_NO_SAVE
    (void)data;
    return false;
#else
    uint32_t p = irq_save();
    unlock();
    for(uint32_t pg = off / CFWCP_PAGE; pg <= (off + len - 1) / CFWCP_PAGE; pg++) {
        if(!(s_erased & (1ull << pg))) { cut_point(); erase_page(pg); s_erased |= 1ull << pg; }
    }
    for(uint32_t o = 0; o < len; o += 16u) {
        uint32_t w[4];
        memcpy(w, data + o, 16);
        if((o & 0x3FFu) == 0) { cut_point(); }   /* test hook: a cut point every 1 KB */
        program_qw(CFWCP_BASE + off + o, w);
    }
    lock();
    irq_restore(p);
    s_state = -1;
    return memcmp((const void *)(uintptr_t)(CFWCP_BASE + off), data, len) == 0;
#endif
}

/* erase the page holding offset off (redo record) */
void cfwcp_erase_page_off(uint32_t off)
{
#ifdef CFW_HW_NO_SAVE
    (void)off;
#else
    if(off >= CFWCP_SIZE) { return; }
    uint32_t p = irq_save();
    unlock();
    cut_point();
    erase_page(off / CFWCP_PAGE);
    lock();
    irq_restore(p);
    s_erased &= ~(1ull << (off / CFWCP_PAGE));
    s_state = -1;
#endif
}
