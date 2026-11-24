/* cfw settings in one 8 KB internal-flash page: 0x081E0000 (bank 2, page 112).
 * This is inside the cfw's own image area (see ld/stm32u575_cfw.ld); the guard below refuses any other
 * address so a bug here can never erase the bootloader (0x08000000-0x0800BFFF), the activation key
 * (0x0800A000) or the stock table cache (0x081E4000+).
 * Records are appended (16-byte header + blob, padded to 16 bytes = one flash quad-word); the last record
 * with a good CRC wins; the page is erased only when the next record does not fit.
 * Flash programming = 128-bit quad-words (RM0456 7.3.7). */
#include <string.h>
#include "stm32u5.h"
#include "hal.h"

#define PAGE_ADDR   0x081E0000u
#define PAGE_SIZE   0x2000u
#define BANK        1u          /* bank 2 */
#define PAGE_NUM    112u
#define MAGIC       0x43465732u /* "CFW2" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t len;
    uint32_t crc;               /* over the blob */
} rec_hdr_t;

static uint32_t crc32(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while(n--) {
        c ^= *b++;
        for(int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    }
    return ~c;
}

static uint32_t rec_size(uint32_t len) { return (uint32_t)sizeof(rec_hdr_t) + ((len + 15u) & ~15u); }

/* walks the records; returns the address after the last one (= where the next record goes) */
static uint32_t walk(uint32_t version, const rec_hdr_t **best)
{
    uint32_t a = PAGE_ADDR;
    *best = NULL;
    while(a + sizeof(rec_hdr_t) <= PAGE_ADDR + PAGE_SIZE) {
        const rec_hdr_t *h = (const rec_hdr_t *)(uintptr_t)a;
        if(h->magic != MAGIC || h->len > SETTINGS_MAX || a + rec_size(h->len) > PAGE_ADDR + PAGE_SIZE) { break; }
        if(h->version == version && h->crc == crc32(h + 1, h->len)) { *best = h; }
        a += rec_size(h->len);
    }
    return a;
}

uint32_t settings_load(uint32_t version, void *buf, uint32_t max)
{
    const rec_hdr_t *h;
    walk(version, &h);
    if(!h) { return 0; }
    uint32_t n = h->len < max ? h->len : max;
    memcpy(buf, h + 1, n);
    return n;
}

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

static void erase_page(void)
{
    wait_idle();
    FLASH_NSSR = 0xFFFFFFFFu;
    FLASH_NSCR = (1u << 1) | (PAGE_NUM << 3) | (BANK << 11);   /* PER, PNB, BKER */
    FLASH_NSCR |= 1u << 16;                                    /* STRT */
    wait_idle();
    FLASH_NSCR = 0;
}

/* programs 16-byte quad-words; only inside the settings page */
static void program_qw(uint32_t addr, const uint32_t w[4])
{
    if(addr < PAGE_ADDR || addr + 16u > PAGE_ADDR + PAGE_SIZE || (addr & 15u)) { return; }
    FLASH_NSSR = 0xFFFFFFFFu;
    FLASH_NSCR = 1u;                                           /* PG */
    for(int i = 0; i < 4; i++) { *(volatile uint32_t *)(uintptr_t)(addr + 4u * (uint32_t)i) = w[i]; }
    wait_idle();
    FLASH_NSCR = 0;
}

bool settings_save(uint32_t version, const void *buf, uint32_t len)
{
#ifdef CFW_HW_NO_SAVE
    /* hardware build (make HW=1): no flash writes until the owner OKs it (CLAUDE.md hard rule) */
    (void)version; (void)buf; (void)len;
    return false;
#endif
    if(len > SETTINGS_MAX) { return false; }
    const rec_hdr_t *h;
    uint32_t a = walk(version, &h);
    uint32_t p = irq_save();
    unlock();
    if(a + rec_size(len) > PAGE_ADDR + PAGE_SIZE) { erase_page(); a = PAGE_ADDR; }
    rec_hdr_t hdr = { MAGIC, version, len, crc32(buf, len) };
    program_qw(a, (const uint32_t *)&hdr);
    const uint8_t *b = buf;
    for(uint32_t o = 0; o < len; o += 16u) {
        uint32_t w[4];
        memset(w, 0xFF, sizeof w);
        memcpy(w, b + o, len - o < 16u ? len - o : 16u);
        program_qw(a + sizeof hdr + o, w);
    }
    FLASH_NSCR = 1u << 31;                                     /* lock */
    irq_restore(p);
    walk(version, &h);
    return h && h->len == len && memcmp(h + 1, buf, len) == 0;
}
