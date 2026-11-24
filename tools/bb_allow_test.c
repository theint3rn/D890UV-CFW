/* Host test of hal/bb_allow.h + the 72 <-> 49 bit frame conversion used by hal/voice.c.
 * Run: tools/bb_allow_test.sh (without the dictionary only the exhaustive part runs).
 * Takes every command of the decoded SCT_Port dictionary (tools/sct3288/sct3288_commands.json: name + write frame) through
 * stdin as "hex name" lines; prints the ones that pass and fails on any that must never pass. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bb_allow.h"
#include "ambe_fec.h"

static const char *const k_never[] = { "FLASH", "MEM", "BOOT", "boot", "PROG", "ACT", "RESET", "WORK_MODE_TX", "CALL_START", "CONNECT",
                                       "SEND_", "HPI", "BER", "SINEWAVE", "DEBUG", "LOW", "DUPLEX", "CARRIER_READY", "LOOP" };

int main(void)
{
    char line[4096];
    int n = 0, ok = 0, bad = 0;
    while(fgets(line, sizeof line, stdin)) {
        char hex[2048], name[256];
        if(sscanf(line, "%2047s %255[^\n]", hex, name) != 2) { continue; }
        uint8_t f[1024];
        size_t m = 0;
        const char *h = strncmp(hex, "84a9", 4) == 0 ? hex + 4 : hex;
        for(; h[0] && h[1] && m < sizeof f; h += 2) { char t[3] = { h[0], h[1], 0 }; f[m++] = (uint8_t)strtol(t, NULL, 16); }
        if(m < 5 || f[0] != 0x61) { continue; }
        unsigned len = (unsigned)(f[1] << 8 | f[2]);
        if(len < 1 || 4u + len > m) { continue; }
        n++;
        if(!bb_allowed(f[3], f[4], f + 5, (uint16_t)(len - 1))) { continue; }
        ok++;
        printf("pass %-28s %s\n", name, hex);
        for(unsigned i = 0; i < sizeof k_never / sizeof k_never[0]; i++) {
            if(strstr(name, k_never[i])) { printf("  MUST NOT PASS\n"); bad++; }
        }
    }
    /* every one of the 65536 TYPE/ID pairs with no data, and the memory / flash opcodes with data */
    uint8_t d[400];
    memset(d, 0, sizeof d);
    unsigned pairs = 0;
    for(unsigned b0 = 0; b0 < 256; b0++) {
        for(unsigned id = 0; id < 256; id++) {
            bool any = false;
            for(unsigned len = 0; len <= 400; len++) { any |= bb_allowed((uint8_t)b0, (uint8_t)id, d, (uint16_t)len); }
            pairs += any;
            if(any && (id == 0x3F || id == 0x96 || id == 0x14 || id == 0x20 || id == 0x43 || id == 0x60)) { printf("pair %02x %02x MUST NOT PASS\n", b0, id); bad++; }
        }
    }
    for(unsigned v = 0; v < 65536; v++) {                    /* WORK_MODE: two values only */
        uint8_t w[3] = { (uint8_t)(v >> 8), (uint8_t)v, 0 };
        bool a = bb_allowed(0x05, 0x18, w, 3);
        if(a != (v == 0x0000 || v == 0x0303)) { printf("WORK_MODE %04x\n", v); bad++; }
    }
    printf("%d dictionary commands, %d pass; %u TYPE/ID pairs can pass; %d wrong\n", n, ok, pairs, bad);
    static const uint8_t sil72[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
    static const uint8_t sil49[7] = { 0xF8, 0x01, 0xA9, 0x9F, 0x8C, 0xE0, 0x80 };
    uint8_t d49[7], o72[9];
    int e = ambe72_decode(sil72, d49);
    ambe72_encode(sil49, o72);
    bool sil = e == 0 && !memcmp(d49, sil49, 7) && !memcmp(o72, sil72, 9);
    printf("DMR silence frame 72 <-> 49: %s (errors %d)\n", sil ? "ok" : "WRONG", e);
    for(int i = 0; i < 7; i++) { printf("%02X", d49[i]); }
    printf("\n");
    return bad || !sil;
}
