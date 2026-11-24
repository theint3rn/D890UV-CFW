/* Host test of app/cfwcp.c (the real file, compiled for the PC): the codeplug region is an mmap at CFWCP_BASE, the
 * flash driver writes into it (the D168UV CFW_FLASH_EXTERNAL interface). Driven by tools/cfwcp_host_test.py.
 *
 *   cfwcp_host_test IN.bin OUT.bin [T,C,R,TEXT]...
 * loads IN.bin (a codeplug as the CPS writes it), writes the journal twice (first the first edit alone, then all of
 * them: the second write has to find the first one's marker), reads it back, prints "read N" + one line per edit,
 * saves the region to OUT.bin. Exit 1 on any failure. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include "cfwcp.h"
#include "radio.h"

radio_t g_radio;
void voice_set_test_tone(int hz) { (void)hz; }
int channels_count(void) { return 0; }
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t p) { (void)p; }
void channel_mode_defaults(channel_t *c) { (void)c; }
void cfw_flash_unlock(void) { }
void cfw_flash_lock(void) { }
void cfw_flash_erase(uint32_t a) { memset((void *)(uintptr_t)a, 0xFF, CFWCP_PAGE); }
void cfw_flash_program16(uint32_t a, const uint32_t w[4]) { memcpy((void *)(uintptr_t)a, w, 16); }

int main(int argc, char **argv)
{
    if(argc < 4) { fprintf(stderr, "usage: %s IN OUT T,C,R,TEXT...\n", argv[0]); return 2; }
    uint8_t *m = mmap((void *)(uintptr_t)CFWCP_BASE, CFWCP_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if(m != (void *)(uintptr_t)CFWCP_BASE) { perror("mmap"); return 2; }
    memset(m, 0xFF, CFWCP_SIZE);
    FILE *f = fopen(argv[1], "rb");
    if(!f) { perror(argv[1]); return 2; }
    size_t n = fread(m, 1, CFWCP_SIZE, f);
    fclose(f);
    (void)n;
    cp_edit_t e[16];
    int ne = 0;
    for(int i = 3; i < argc && ne < 16; i++, ne++) {
        unsigned t, c, r;
        char txt[8] = { 0 };
        if(sscanf(argv[i], "%x,%u,%u,%7s", &t, &c, &r, txt) != 4) { return 2; }
        e[ne].table = (uint8_t)t; e[ne].col = (uint8_t)c; e[ne].row = (uint16_t)r;
        memcpy(e[ne].text, txt, sizeof e[ne].text);
    }
    if(!cfwcp_valid()) { puts("codeplug invalid"); return 1; }
    if(!cfwcp_journal_write(e, 1)) { puts("write 1 failed"); return 1; }
    if(!cfwcp_valid()) { puts("invalid after write 1"); return 1; }
    if(!cfwcp_journal_write(e, ne)) { puts("write 2 failed"); return 1; }
    cfwcp_write_begin();                       /* forget the cached state: parse the region again, as after a boot */
    if(!cfwcp_valid()) { puts("invalid after write 2"); return 1; }
    cp_edit_t r[16];
    int k = cfwcp_journal_read(r, 16);
    printf("read %d\n", k);
    for(int i = 0; i < k; i++) { printf("%02X,%u,%u,%s\n", r[i].table, r[i].col, r[i].row, r[i].text); }
    f = fopen(argv[2], "wb");
    if(!f) { perror(argv[2]); return 2; }
    fwrite(m, 1, CFWCP_SIZE, f);
    fclose(f);
    return k == ne ? 0 : 1;
}
