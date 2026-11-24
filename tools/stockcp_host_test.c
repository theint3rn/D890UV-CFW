/* Host test of app/stockcp.c (with the real cfwcp.c): the stock CPS DMR / NXDN sections the firmware uses.
 * Driven by tools/stockcp_host_test.py.   stockcp_host_test IN.bin CH...
 * Prints the stock keys, per channel the radio ID and keys, NX Setting values, NX Talk Group lookups and user
 * database names, one fact per line. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "cfwcp.h"
#include "stockcp.h"
#include "radio.h"
#include "modem.h"
#include "protos/glue.h"

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
    if(argc < 2) { fprintf(stderr, "usage: %s IN [CH|u:MODE:ID|g:ID]...\n", argv[0]); return 2; }
    uint8_t *m = mmap((void *)(uintptr_t)CFWCP_BASE, CFWCP_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if(m != (void *)(uintptr_t)CFWCP_BASE) { perror("mmap"); return 2; }
    memset(m, 0xFF, CFWCP_SIZE);
    FILE *f = fopen(argv[1], "rb");
    if(!f) { perror(argv[1]); return 2; }
    if(fread(m, 1, CFWCP_SIZE, f) == 0) { return 2; }
    fclose(f);
    if(!cfwcp_valid()) { puts("codeplug invalid"); return 1; }
    printf("own_keys %d keys %d users %d\n", cfwcp_own_key_count(), cfwcp_key_count(), stockcp_user_count());
    for(int i = 0; i < cfwcp_key_count(); i++) {
        const cp_key_t *k = cfwcp_key(i);
        printf("key %d %.16s kind %u id %u len %u ", i, k->name, k->kind, k->key_id, k->len);
        for(int j = 0; j < k->len; j++) { printf("%02X", k->key[j]); }
        printf("\n");
    }
    printf("nx_own %u moni %u any_ran %d\n", stockcp_nxdn_own_id(), stockcp_nxdn_moni(), cfw_nxdn_any_ran());
    for(int a = 2; a < argc; a++) {
        char nm[32];
        if(argv[a][0] == 'u') {                     /* u:MODE:ID  user / contact name */
            unsigned mode, id;
            if(sscanf(argv[a], "u:%u:%u", &mode, &id) != 2) { return 2; }
            bool ok = stockcp_id_name((uint8_t)mode, id, false, nm, sizeof nm);
            printf("user %u %u %s\n", mode, id, ok ? nm : "-");
        } else if(argv[a][0] == 'g') {              /* g:MODE:ID  group name (+ NX TxForbid) */
            unsigned mode, id;
            if(sscanf(argv[a], "g:%u:%u", &mode, &id) != 2) { return 2; }
            bool ok = stockcp_id_name((uint8_t)mode, id, true, nm, sizeof nm);
            printf("group %u %u %s forbid %d\n", mode, id, ok ? nm : "-", cfw_nxdn_tx_forbidden(id));
        } else {
            int ch = atoi(argv[a]);
            channel_t c;
            memset(&c, 0, sizeof c);
            cfwcp_channel(ch, &c);
            printf("ch %d mode %u rid %u key %d\n", ch, c.mode, stockcp_channel_radio_id(ch), c.key == 0xFF ? -1 : c.key);
        }
    }
    return 0;
}
