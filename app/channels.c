/* Memory channels: CPS codeplug channels read from NAND, then built-in demo channels (one per mode).
 *
 * D890UV codeplug channel record (decoded from the K6RRR codeplug in emu/images/extflash.pages;
 * CPS address == NAND address): 128 bytes, 128 per bank, bank b at 0x01000000 + b * 0x80000, up to
 * 32 banks (4096 slots). Layout follows the AnyTone D878UV channel format with the name moved:
 *   +0x00  RX frequency, BCD big endian, 10 Hz units (44 65 80 00 = 446.58000 MHz)
 *   +0x04  TX offset, BCD, 10 Hz units
 *   +0x08  bits 1:0 type (0 analog, 1 digital, 2 A+D, 3 D+A; stock reads it as chan[8] & 3),
 *          bits 3:2 power, bit 4 = 25 kHz, bits 7:6 offset direction (0 none, 1 +, 2 -)
 *   +0x09  bit 0 RX CTCSS, bit 2 TX CTCSS   +0x0A TX tone index   +0x0B RX tone index
 *   +0x14  DMR contact index (16 bit LE)    +0x20 DMR colour code  +0x21 bit 0 = time slot 2
 *   +0x44  name, UTF-16LE, 16 characters
 * Not decoded yet: the talkgroup contact table (so the TG number of a contact is unknown), zones,
 * scan lists, power levels. Empty slots have an invalid / out-of-band frequency. */
#include <string.h>
#include "channels.h"
#include "hal.h"
#include "cfwcp.h"
#include "stockopt.h"
#include "rset.h"

#ifndef CFW_NAND_CODEPLUG
#define CFW_NAND_CODEPLUG 1      /* 0: board whose stock NAND channel layout is not decoded (D168UV) */
#endif
#if CFW_NAND_CODEPLUG
#define CP_BASE   0x01000000u
#define CP_BANK   0x80000u
#define CP_BANKS  32
#define CP_PER    128
#define CP_REC    128u
#else
#define CP_BASE   0u
#define CP_BANK   0u
#define CP_BANKS  1
#define CP_PER    1
#define CP_REC    128u
#endif

static const channel_t k_builtin[] = {
    { .name = "2M CALL",    .rx_hz = 146520000, .tx_hz = 146520000, .mode = CFW_MODE_FM, .wide = true },
    { .name = "70CM CALL",  .rx_hz = 446000000, .tx_hz = 446000000, .mode = CFW_MODE_FM, .wide = true },
    { .name = "RPT 146.97", .rx_hz = 146970000, .tx_hz = 146370000, .mode = CFW_MODE_FM, .ctcss_dhz = 1035, .wide = true },
    { .name = "NOAA WX1",   .rx_hz = 162550000, .tx_hz = 0,         .mode = CFW_MODE_FM, .wide = true },
    { .name = "DMR SMPLX",  .rx_hz = 441000000, .tx_hz = 441000000, .mode = CFW_MODE_DMR, .color_code = 1, .slot = 1, .talkgroup = 99 },
    { .name = "DMR TG91",   .rx_hz = 440512500, .tx_hz = 445512500, .mode = CFW_MODE_DMR, .color_code = 1, .slot = 1, .talkgroup = 91 },
    { .name = "DMR T3 CC",  .rx_hz = 439500000, .tx_hz = 434500000, .mode = CFW_MODE_DMR, .color_code = 1, .slot = 1, .talkgroup = 91, .trunk = true },
    { .name = "P25 SMPLX",  .rx_hz = 442000000, .tx_hz = 442000000, .mode = CFW_MODE_P25P1, .nac = 0x293, .talkgroup = 1 },
    { .name = "P25 TRUNK",  .rx_hz = 460500000, .tx_hz = 465500000, .mode = CFW_MODE_P25P1, .nac = 0x293, .trunk = true },
    { .name = "P25 PH2",    .rx_hz = 460550000, .tx_hz = 465550000, .mode = CFW_MODE_P25P2, .nac = 0x293, .wacn = 0xBEE00, .sysid = 0x3C1, .slot = 1 },
    { .name = "DSTAR SMP",  .rx_hz = 145670000, .tx_hz = 145670000, .mode = CFW_MODE_DSTAR, .ur = "CQCQCQ  " },
    { .name = "NXDN SMP",   .rx_hz = 441250000, .tx_hz = 441250000, .mode = CFW_MODE_NXDN, .ran = 1, .talkgroup = 1 },
    { .name = "YSF WIRES",  .rx_hz = 145375000, .tx_hz = 145375000, .mode = CFW_MODE_YSF, .dgid = 0 },
    /* appended (keep the offsets above stable: test tools address built-ins as first built-in + offset) */
    { .name = "P25P2 SMPLX", .rx_hz = 445500000, .tx_hz = 445500000, .mode = CFW_MODE_P25P2, .nac = 0x293, .wacn = 0xBEE00, .sysid = 0x3C1, .slot = 1 },
    { .name = "NXDN96 SMP",  .rx_hz = 441375000, .tx_hz = 441375000, .mode = CFW_MODE_NXDN, .ran = 1, .talkgroup = 1, .nxdn96 = true },
};
#define N_BUILTIN ((int)(sizeof k_builtin / sizeof k_builtin[0]))

static uint16_t s_cp[CP_BANKS * CP_PER];     /* valid codeplug slot numbers, in order */
static int s_ncp;
static bool s_cfwcp;           /* channels come from the cfw codeplug in internal flash (CPS) */
static ch_override_t s_ovr[CH_MAX_OVERRIDES];
static int s_novr;

static const uint16_t k_ctcss[] = {
    625, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230,
    1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899,
    1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};
#define N_CTCSS ((int)(sizeof k_ctcss / sizeof k_ctcss[0]))

uint16_t ctcss_from_index(uint8_t i) { return i < N_CTCSS ? k_ctcss[i] : 0; }
int ctcss_count(void) { return N_CTCSS; }
uint16_t ctcss_at(int i) { return (i >= 0 && i < N_CTCSS) ? k_ctcss[i] : 0; }

bool freq_in_band(uint32_t hz)
{
    return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u);
}

static bool bcd32(const uint8_t *b, uint32_t *out)
{
    uint32_t v = 0;
    for(int i = 0; i < 4; i++) {
        uint8_t hi = b[i] >> 4, lo = b[i] & 0xF;
        if(hi > 9 || lo > 9) { return false; }
        v = v * 100u + hi * 10u + lo;
    }
    *out = v * 10u;
    return true;
}

static uint32_t cp_addr(uint16_t slot) { return CP_BASE + (slot / CP_PER) * CP_BANK + (slot % CP_PER) * CP_REC; }

void channels_init(void)
{
    s_ncp = 0;
    s_cfwcp = false;
    if(cfwcp_valid()) { s_ncp = cfwcp_channel_count(); s_cfwcp = true; return; }   /* CPS codeplug (cfw_cps) */
    if(!CFW_NAND_CODEPLUG || !nand_id()) { return; }
    for(int bank = 0; bank < CP_BANKS; bank++) {
        int found_in_bank = 0;
        for(int pg = 0; pg < CP_PER * (int)CP_REC / 4096; pg++) {
            uint32_t page = CP_BASE + (uint32_t)bank * CP_BANK + (uint32_t)pg * 4096u;
            if(!nand_page_load(page)) { return; }
            for(int k = 0; k < 4096 / (int)CP_REC; k++) {
                uint8_t f[4];
                uint32_t hz;
                nand_cache_read((uint32_t)k * CP_REC, f, 4);
                if(bcd32(f, &hz) && freq_in_band(hz)) {
                    s_cp[s_ncp++] = (uint16_t)(bank * CP_PER + pg * (4096 / CP_REC) + k);
                    found_in_bank++;
                }
            }
            if(pg == 0 && !found_in_bank) { break; }   /* bank starts empty: CPS fills banks in order */
        }
    }
}

int channels_codeplug_count(void) { return s_ncp; }
int channels_count(void) { return s_ncp + N_BUILTIN; }

void channel_mode_defaults(channel_t *c)
{
    c->wide = c->mode == CFW_MODE_FM ? c->wide : false;
    if(c->mode == CFW_MODE_DMR && !c->slot) { c->slot = 1; }
    if(c->mode == CFW_MODE_DMR && !c->talkgroup) { c->talkgroup = 91; }
    if((c->mode == CFW_MODE_P25P1 || c->mode == CFW_MODE_P25P2) && !c->nac) { c->nac = 0x293; }
    if(c->mode == CFW_MODE_P25P2 && !c->slot) { c->slot = 1; }
    if(c->mode == CFW_MODE_P25P2 && !c->wacn) { c->wacn = 0xBEE00; c->sysid = 0x3C1; }
    if(c->mode == CFW_MODE_DSTAR && !c->ur[0]) { strcpy(c->ur, "CQCQCQ  "); }
}

static void load_codeplug(uint16_t slot, channel_t *c)
{
    uint8_t r[CP_REC];
    memset(c, 0, sizeof *c);
    c->src = CH_SRC_CODEPLUG;
    c->cp_slot = slot;
    c->scanlist = 0xFF;
    if(!nand_read(cp_addr(slot), r, sizeof r)) { strcpy(c->name, "NAND ERR"); return; }
    uint32_t off = 0;
    bcd32(&r[0], &c->rx_hz);
    bcd32(&r[4], &off);
    switch(r[8] >> 6) {
    case 1:  c->tx_hz = c->rx_hz + off; break;
    case 2:  c->tx_hz = c->rx_hz - off; break;
    default: c->tx_hz = c->rx_hz; break;
    }
    c->wide = (r[8] >> 4) & 1;
    c->power = (r[8] >> 2) & 3u;
    c->mode = (r[8] & 3) ? CFW_MODE_DMR : CFW_MODE_FM;
    if(r[9] & 0x04) { c->ctcss_dhz = ctcss_from_index(r[10]); }
    c->color_code = r[0x20] & 0x0F;
    c->slot = (uint8_t)((r[0x21] & 1) + 1);
    c->cp_contact = (uint16_t)(r[0x14] | (r[0x15] << 8));
    for(int i = 0; i < 16; i++) {
        uint16_t u = (uint16_t)(r[0x44 + 2 * i] | (r[0x45 + 2 * i] << 8));
        if(!u) { break; }
        c->name[i] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
    }
    channel_mode_defaults(c);
}

bool channel_load(int i, channel_t *out)
{
    if(i < 0 || i >= channels_count()) { return false; }
    for(int k = 0; k < s_novr; k++) {
        if(s_ovr[k].index == i) { *out = s_ovr[k].ch; goto power; }
    }
    if(i < s_ncp) { if(s_cfwcp) { cfwcp_channel(i, out); stockopt_channel_apply(i, out); } else { load_codeplug(s_cp[i], out); } }
    else { *out = k_builtin[i - s_ncp]; out->src = CH_SRC_BUILTIN; out->scanlist = 0xFF; }
power:
    rset_channel_power(i, &out->power);           /* TX power edited in the radio menu (codeplug journal) */
    return true;
}

/* ---- zones ---- */
int zone_count(void) { return cfwcp_zone_count(); }

const char *zone_name(int z)
{
    static char n[17];
    const cp_list_t *l = cfwcp_zone(z);
    if(!l) { return "All channels"; }
    int i = 0;
    for(; i < 16 && l->name[i]; i++) { n[i] = (l->name[i] >= 0x20 && l->name[i] < 0x7F) ? l->name[i] : '?'; }
    n[i] = 0;
    return n;
}

int zone_size(int z)
{
    const cp_list_t *l = cfwcp_zone(z);
    return l ? l->count : channels_count();
}

int zone_channel(int z, int k)
{
    const cp_list_t *l = cfwcp_zone(z);
    if(!l) { return (k >= 0 && k < channels_count()) ? k : -1; }
    if(k < 0 || k >= l->count) { return -1; }
    uint16_t ch = cfwcp_member(l->first, k);
    return ch < channels_count() ? ch : -1;
}

int zone_find(int z, int ch)
{
    const cp_list_t *l = cfwcp_zone(z);
    if(!l) { return (ch >= 0 && ch < channels_count()) ? ch : -1; }
    for(int k = 0; k < l->count; k++) { if(cfwcp_member(l->first, k) == ch) { return k; } }
    return -1;
}

void channel_store(int i, const channel_t *c)
{
    for(int k = 0; k < s_novr; k++) {
        if(s_ovr[k].index == i) { s_ovr[k].ch = *c; return; }
    }
    if(s_novr == CH_MAX_OVERRIDES) {                 /* full: drop the oldest */
        memmove(&s_ovr[0], &s_ovr[1], sizeof s_ovr[0] * (CH_MAX_OVERRIDES - 1));
        s_novr--;
    }
    s_ovr[s_novr].index = (uint16_t)i;
    s_ovr[s_novr].ch = *c;
    s_novr++;
}

int channels_overrides(const ch_override_t **list) { *list = s_ovr; return s_novr; }

void channels_set_overrides(const ch_override_t *list, int n)
{
    s_novr = 0;
    for(int k = 0; k < n && k < CH_MAX_OVERRIDES; k++) {
        if(list[k].index < channels_count()) { s_ovr[s_novr++] = list[k]; }
    }
}

/* after a CPS write: re-scan (cfw codeplug if valid, else NAND) and drop the per-channel overrides */
void channels_reload(void)
{
    s_novr = 0;
    channels_init();
}
