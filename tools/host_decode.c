/* Host check of the cfw proto glue (app/protos/*.c): feeds a symbol stream to one registered proto with a
 * fake platform and prints its status lines and call info.  tools/host_decode.sh MODE FILE [nac/cc/ran trunk] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem.h"

extern const cfw_proto_t *const cfw_proto_reg_k_dmr, *const cfw_proto_reg_k_nxdn, *const cfw_proto_reg_k_p25p1,
    *const cfw_proto_reg_k_p25p2, *const cfw_proto_reg_k_dstar, *const cfw_proto_reg_k_ysf;
static uint32_t now, voice, tunes;
static uint32_t pnow(void) { return now; }
static void plog(cfw_log_level_t l, const char *m) { (void)l; puts(m); }
static void pvoice(cfw_vocoder_t v, const uint8_t *b, size_t n, int e) { (void)v; (void)b; (void)n; (void)e; voice++; }
static void pdata(uint8_t t, const uint8_t *d, size_t n) { (void)t; (void)d; (void)n; }
static void pcall(const cfw_call_info_t *c) { printf("  [call] src %u dst %u %s>%s slot %u voice %d\n", c->src_id, c->dst_id, c->src_call, c->dst_call, c->slot, c->voice); }
static bool ptune(uint32_t rx, uint32_t tx) { tunes++; printf("  [tune] %u / %u\n", rx, tx); return true; }
static bool preq(void) { return false; }
static bool pcp(uint32_t a, void *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
static const cfw_platform_t pf = { CFW_MODEM_API_VERSION, pnow, plog, pvoice, pdata, pcall, ptune, preq, pcp };

int main(int argc, char **argv)
{
    if(argc < 3) { return 1; }
    const cfw_proto_t *p = !strcmp(argv[1], "dmr") ? cfw_proto_reg_k_dmr : !strcmp(argv[1], "nxdn") ? cfw_proto_reg_k_nxdn :
        !strcmp(argv[1], "p25") ? cfw_proto_reg_k_p25p1 : !strcmp(argv[1], "p25p2") ? cfw_proto_reg_k_p25p2 :
        !strcmp(argv[1], "dstar") ? cfw_proto_reg_k_dstar : cfw_proto_reg_k_ysf;
    FILE *f = fopen(argv[2], "rb");
    static uint8_t buf[4000000];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    cfw_chan_cfg_t cfg = { .rx_hz = 460500000, .tx_hz = 465500000, .my_id = 1, .color_code = 1, .slot = 1, .wacn = 0xBEE00, .sysid = 0x3C1,
                           .nac = 0x293, .ran = 1, .talkgroup = argc > 5 ? (uint32_t)atoi(argv[5]) : 91, .chan_step_hz = 12500 };
    if(argc > 3) { cfg.trunk = atoi(argv[3]) != 0; }
    if(argc > 4) { cfg.dgid = (uint8_t)atoi(argv[4]); }
    p->init(p->ctx, &pf);
    p->rx_reset(p->ctx);
    if(p->configure) { p->configure(p->ctx, &cfg); }
    char st[128];
    for(size_t i = 0; i < n; i += 48) {
        size_t k = n - i < 48 ? n - i : 48;
        p->rx_symbols(p->ctx, buf + i, NULL, k);
        now = (uint32_t)((i + k) * 1000ull / p->air.symbol_rate);
        if(p->tick) { p->tick(p->ctx, now); }
        if(i % (p->air.symbol_rate) < 48) { p->status(p->ctx, st, sizeof st); for(char *c = st; *c; c++) { if(*c == '\n') { *c = '|'; } } printf("%5u ms: %s\n", now, st); }
    }
    p->status(p->ctx, st, sizeof st);
    printf("END: %s  (voice frames to vocoder %u, tunes %u)\n", st, voice, tunes);
    return 0;
}
