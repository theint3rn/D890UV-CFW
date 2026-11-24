/* Host tool: builds symbol test streams for every cfw mode with the cfw/proto transmitters, for the emulator
 * symbol FIFO (`syms Load @file rate`, cfw `make EMU=1`). One symbol per byte: dibits 0..3, D-STAR bits 0/1.
 * Build + run: cfw/tools/make_streams.sh  ->  cfw/build_streams/*.sym
 *
 *   dmr_call.sym     4800  DMR BS voice call TG 91 < 9990201, CC1 TS1
 *   dmr_t3.sym       4800  DMR Tier III TSCC: C_ALOHA, then TV_GRANT TG 91 -> LPCN 5, voice on TS1, ALOHA again
 *   p25_call.sym     4800  P25 P1 voice call NAC 293, TG 100 < 1234567
 *   p25_trunk.sym    4800  P25 control channel (IDEN_UP, NET/RFSS status, GRP_V_GRANT TG 100 -> 460.5500)
 *                          then the granted voice call, then the control channel again
 *   p25p2_call.sym   6000  P25 Phase 2 voice, channel 0 (slot 1), MAC_PTT src 703710 grp 4660, WACN BEE00 SYS 3C1 NAC 293
 *   nxdn_call.sym    2400  NXDN 4800 voice call RAN 1, 1001 -> 300
 *   dstar_call.sym   4800  D-STAR header MY N0CALL / UR CQCQCQ / RPT1 XX0XYZ B / RPT2 XX0XYZ G, text, voice
 *   ysf_call.sym     4800  YSF V/D mode 2 call N0CALL -> ALL, DG-ID 0 (and a second call with DG-ID 20)
 * Voice payloads are counter patterns (nothing to listen to; the decoders count frames). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr.h"
#include "dmr_t3.h"
#include "nxdn.h"
#include "p25_p1.h"
#include "p25_p2.h"
#include "p25_tsbk.h"
#include "dstar.h"
#include "ysf.h"

static uint8_t g_out[2000000];
static size_t g_n;
static uint32_t g_seed = 12345;
static unsigned rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 16; }
static void put(const uint8_t *d, size_t n) { memcpy(g_out + g_n, d, n); g_n += n; }
static void noise(size_t n, unsigned mask) { while(n--) { g_out[g_n++] = (uint8_t)(rnd() & mask); } }

static void save(const char *dir, const char *name, unsigned rate, const char *what)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if(!f) { perror(path); exit(1); }
    fwrite(g_out, 1, g_n, f);
    fclose(f);
    printf("%-16s %7zu symbols  %5.1f s @ %u/s  %s\n", name, g_n, (double)g_n / rate, rate, what);
    g_n = 0;
}

/* ---------------- DMR ---------------- */
static void dmr_slot_pair(const dmr_slc_t *slc, unsigned k, const uint8_t b1[DMR_BURST_DIBITS], uint8_t cc)
{
    uint8_t cach[DMR_CACH_DIBITS], idle[DMR_BURST_DIBITS];
    dmr_tx_cach_slc(slc, (2 * k) % 4u, 1, 0, cach);
    put(cach, DMR_CACH_DIBITS);
    put(b1, DMR_BURST_DIBITS);
    dmr_tx_cach_slc(slc, (2 * k + 1) % 4u, 1, 1, cach);
    dmr_tx_idle(cc, DMR_SYNC_BS_DATA, idle);
    put(cach, DMR_CACH_DIBITS);
    put(idle, DMR_BURST_DIBITS);
}

static unsigned dmr_voice_call(const dmr_slc_t *slc, unsigned k, uint32_t dst, uint32_t src, uint8_t cc, int nv)
{
    dmr_lc_t lc;
    dmr_lc_make_voice(&lc, true, dst, src, 0);
    dmr_voice_tx_t vt;
    dmr_voice_tx_init(&vt, &lc, cc, DMR_SYNC_BS_VOICE);
    uint8_t b[DMR_BURST_DIBITS];
    unsigned frame = 0;
    for(int i = 0; i < 2; i++) { dmr_tx_voice_header(&lc, cc, DMR_SYNC_BS_DATA, b); dmr_slot_pair(slc, k++, b, cc); }
    for(int i = 0; i < nv; i++) {
        uint8_t fr[27];
        for(int f = 0; f < 3; f++, frame++) { for(int j = 0; j < 9; j++) { fr[9 * f + j] = (uint8_t)(frame + j); } }
        dmr_voice_tx_burst(&vt, fr, b);
        dmr_slot_pair(slc, k++, b, cc);
    }
    dmr_tx_terminator(&lc, cc, DMR_SYNC_BS_DATA, b);
    dmr_slot_pair(slc, k++, b, cc);
    return k;
}

static void gen_dmr(const char *dir)
{
    dmr_slc_t slc = { DMR_SLCO_C_SYS_PARMS, 0x4A5B6C };
    noise(2400, 3);
    unsigned k = dmr_voice_call(&slc, 0, 91, 9990201, 1, 30);
    uint8_t b[DMR_BURST_DIBITS];
    for(int i = 0; i < 4; i++) { dmr_tx_idle(1, DMR_SYNC_BS_DATA, b); dmr_slot_pair(&slc, k++, b, 1); }
    noise(2400, 3);
    save(dir, "dmr_call.sym", 4800, "DMR TG91 < 9990201 CC1 TS1");
}

static unsigned t3_aloha_bursts(const dmr_slc_t *slc, unsigned k, int n)
{
    t3_aloha_t a;
    memset(&a, 0, sizeof a);
    a.syscode = 0x4A58;
    a.ms_addr = T3_ADDR_ALLMSID;
    a.nrand_wait = 5;
    for(int i = 0; i < n; i++) {
        dmr_csbk_t c;
        uint8_t b[DMR_BURST_DIBITS];
        dmr_t3_build_aloha(&c, &a);
        dmr_tx_csbk(&c, 1, DMR_SYNC_BS_DATA, b);
        dmr_slot_pair(slc, k++, b, 1);
    }
    return k;
}

static void gen_dmr_t3(const char *dir)
{
    dmr_slc_t slc = { DMR_SLCO_C_SYS_PARMS, 0x4A58 };
    noise(1200, 3);
    unsigned k = t3_aloha_bursts(&slc, 0, 40);              /* 2.4 s of TSCC */
    for(int i = 0; i < 4; i++) {                             /* grant TG 91 from 9990201 on LPCN 5 */
        t3_grant_t g;
        memset(&g, 0, sizeof g);
        g.lpcn = 5; g.lcn = 0; g.target = 91; g.source = 9990201;
        dmr_csbk_t c;
        uint8_t b[DMR_BURST_DIBITS];
        dmr_t3_build_grant(&c, T3_TV_GRANT, &g);
        dmr_tx_csbk(&c, 1, DMR_SYNC_BS_DATA, b);
        dmr_slot_pair(&slc, k++, b, 1);
    }
    k = dmr_voice_call(&slc, k, 91, 9990201, 1, 24);        /* the traffic channel (stream continues there) */
    k = t3_aloha_bursts(&slc, k, 60);
    noise(1200, 3);
    save(dir, "dmr_t3.sym", 4800, "DMR Tier III TSCC + grant TG91 -> lpcn 5 + voice");
}

/* ---------------- P25 ---------------- */
static void p25_voice(uint16_t nac, uint16_t tg, uint32_t src, int ldus)
{
    static uint8_t air[P25_LDU_AIR + P25_TDU_AIR + P25_HDU_AIR];
    p25_p1_voice_tx_t tx;
    p25_lc_t lc;
    p25_lc_group_voice(&lc, 0, tg, src);
    put(air, p25_p1_voice_tx_start(&tx, air, nac, &lc, 0));
    for(int f = 0; f < 9 * ldus; f++) {
        uint8_t imbe[P25_IMBE_BYTES];
        for(int j = 0; j < P25_IMBE_BYTES; j++) { imbe[j] = (uint8_t)(f + j); }
        size_t n = p25_p1_voice_tx_push(&tx, air, imbe);
        if(n) { put(air, n); }
    }
    put(air, p25_p1_voice_tx_end(&tx, air, NULL));
}

static void gen_p25(const char *dir)
{
    noise(2400, 3);
    p25_voice(0x293, 100, 1234567, 12);
    noise(2400, 3);
    save(dir, "p25_call.sym", 4800, "P25 NAC293 TG100 < 1234567");
}

static void tsbk(uint16_t nac, const uint8_t blocks[][10], unsigned n)
{
    static uint8_t air[400];
    put(air, p25_p1_build_tsbk(air, nac, blocks, n, 1));
}

static void p25_cc(uint16_t nac, int reps, bool grant)
{
    uint8_t b[3][10];
    /* IDEN 1: base 460.0000, 12.5 kHz spacing, +5 MHz SU TX; this site's CC = iden 1 ch 40 = 460.5000 */
    for(int r = 0; r < reps; r++) {
        p25_osp_iden_up_vu(b[0], 1, 4, 5000000, 12500, 460000000);
        p25_osp_net_sts(b[1], 0, 0xBEE00, 0x3C1, (1u << 12) | 40u, 0x70);
        p25_osp_rfss_sts(b[2], 0, 0x3C1, 1, 2, (1u << 12) | 40u, 0x70);
        tsbk(nac, (const uint8_t (*)[10])b, 3);
        if(grant) {
            p25_osp_grp_v_grant(b[0], 0, (1u << 12) | 44u, 100, 1234567);   /* TG 100 -> 460.5500 */
            tsbk(nac, (const uint8_t (*)[10])b, 1);
        }
    }
}

static void gen_p25_trunk(const char *dir)
{
    noise(1200, 3);
    p25_cc(0x293, 25, false);         /* ~1.5 s control channel */
    p25_cc(0x293, 3, true);           /* grants */
    p25_voice(0x293, 100, 1234567, 10);
    noise(600, 3);
    p25_cc(0x293, 40, false);
    noise(1200, 3);
    save(dir, "p25_trunk.sym", 4800, "P25 CC 460.5 + grant TG100 -> 460.55 + voice");
}

static void gen_p25p2(const char *dir)
{
    p25_p2_ess_t ess = { 0x80, 0, { 0 } };
    uint8_t essb[16], essa[28];
    p25_p2_ess_encode(&ess, essb, essa);
    static uint8_t o[P25_P2_SLOT];
    static p25_p2_scrambler_t scr;
    p25_p2_scrambler_init(&scr, 0xBEE00, 0x3C1, 0x293);
    noise(2000, 3);
    unsigned fc = 0;
    for(int sf = 0; sf < 10; sf++) {
        int vb = 0;
        for(unsigned slot = 0; slot < P25_P2_SLOTS; slot++) {
            int isch = p25_p2_isch_for_slot(slot, (unsigned)sf);
            unsigned ch = p25_p2_slot_channel(slot);
            p25_mac_pdu_t m;
            if(ch == 0 && slot < 10) {
                uint8_t fr[4][P25_AMBE_BYTES];
                for(int i = 0; i < 4; i++, fc++) { for(int j = 0; j < P25_AMBE_BYTES; j++) { fr[i][j] = (uint8_t)(fc + j); } }
                if(vb < 4) { p25_p2_build_4v(o, slot, isch, &scr, (const uint8_t (*)[P25_AMBE_BYTES])fr, essb + 4 * vb); }
                else { p25_p2_build_2v(o, slot, isch, &scr, (const uint8_t (*)[P25_AMBE_BYTES])fr, essa); }
                vb++;
            } else if(ch == 0) {
                if(sf < 9) { p25_mac_ptt(&m, true, 0x0ABCDE, 0x1234); }
                else { p25_mac_end_ptt(&m, true, 0x293, 0x0ABCDE, 0x1234); }
                p25_p2_build_mac(o, slot, isch, &scr, &m);
            } else {
                p25_mac_idle(&m, slot == 10);
                p25_p2_build_mac(o, slot, isch, slot == 10 ? NULL : &scr, &m);
            }
            put(o, P25_P2_SLOT);
        }
    }
    noise(2000, 3);
    save(dir, "p25p2_call.sym", 6000, "P25 Phase 2 ch0 703710 -> TG 4660");
}

/* ---------------- NXDN ---------------- */
static void gen_nxdn(const char *dir)
{
    uint8_t vcall[10], rel[10], sacch[9], info[4], frame[NXDN_FRAME_DIBITS];
    const uint8_t ran = 1;
    nxdn_msg_build_vcall(vcall, NXDN_MSG_VCALL, NXDN_CALL_CONFERENCE, 1001, 300);
    nxdn_msg_build_vcall(rel, NXDN_MSG_TX_REL, NXDN_CALL_CONFERENCE, 1001, 300);
    memset(sacch, 0, sizeof sacch);
    memcpy(sacch, vcall, 8);
    noise(1200, 3);
    nxdn_sacch_quarter(sacch, 0, ran, false, info);
    nxdn_tx_voice_frame(0x40, info, vcall, vcall, NULL, false, frame);
    put(frame, sizeof frame);
    for(int k = 0; k < 40; k++) {
        uint8_t vch[36];
        for(int i = 0; i < 36; i++) { vch[i] = (uint8_t)(k * 4 + i); }
        nxdn_sacch_quarter(sacch, (unsigned)k % 4u, ran, true, info);
        nxdn_tx_voice_frame(0x56, info, NULL, NULL, vch, false, frame);
        put(frame, sizeof frame);
    }
    nxdn_sacch_quarter(sacch, 0, ran, false, info);
    nxdn_tx_voice_frame(0x40, info, rel, rel, NULL, false, frame);
    put(frame, sizeof frame);
    noise(1200, 3);
    save(dir, "nxdn_call.sym", 2400, "NXDN RAN1 1001 -> 300");
}

/* ---------------- D-STAR ---------------- */
static void call8(char *dst, const char *s) { memset(dst, ' ', 8); memcpy(dst, s, strlen(s) < 8 ? strlen(s) : 8); }

static void gen_dstar(const char *dir)
{
    dstar_header_t h;
    memset(&h, 0, sizeof h);
    call8(h.rpt2, "XX0XYZ G");
    call8(h.rpt1, "XX0XYZ B");
    call8(h.ur, "CQCQCQ");
    call8(h.my, "N0CALL");
    memcpy(h.my_sfx, "D890", 4);
    static dstar_tx_t tx;
    dstar_tx_init(&tx, &h);
    dstar_tx_set_text(&tx, "cfw D-STAR test     ");
    static uint8_t bits[2048];
    noise(2400, 1);
    put(bits, dstar_tx_header(&tx, 64, bits));
    for(int f = 0; f < 21 * 6; f++) {
        uint8_t ambe[DSTAR_VOICE_BYTES];
        for(int j = 0; j < (int)DSTAR_VOICE_BYTES; j++) { ambe[j] = (uint8_t)(f + j); }
        put(bits, dstar_tx_frame(&tx, ambe, bits));
    }
    put(bits, dstar_tx_end(&tx, bits));
    noise(2400, 1);
    save(dir, "dstar_call.sym", 4800, "D-STAR N0CALL/D890 > CQCQCQ via XX0XYZ");
}

/* ---------------- YSF ---------------- */
static void ysf_call(const char *src, const char *dest, uint8_t dgid, int frames)
{
    ysf_csd_t csd;
    memset(&csd, 0, sizeof csd);
    memset(csd.dest, ' ', 10); memcpy(csd.dest, dest, strlen(dest));
    memset(csd.src, ' ', 10); memcpy(csd.src, src, strlen(src));
    memset(csd.down, ' ', 10); memset(csd.up, ' ', 10);
    csd.valid = 0x0F;
    static ysf_tx_t tx;
    static uint8_t d[YSF_FRAME_DIBITS];
    ysf_tx_init(&tx, YSF_DT_VD2, &csd, dgid);
    ysf_tx_header(&tx, d); put(d, sizeof d);
    for(int f = 0; f < frames; f++) {
        uint8_t v[5 * 7];
        for(int j = 0; j < (int)sizeof v; j++) { v[j] = (uint8_t)(f + j); }
        ysf_tx_voice(&tx, v, d); put(d, sizeof d);
    }
    ysf_tx_terminator(&tx, d); put(d, sizeof d);
}

static void gen_ysf(const char *dir)
{
    noise(2400, 3);
    ysf_call("N0CALL", "ALL", 0, 40);
    noise(2400, 3);
    ysf_call("XX0ABC", "ALL", 20, 30);
    noise(2400, 3);
    save(dir, "ysf_call.sym", 4800, "YSF N0CALL -> ALL DG-ID 0, then XX0ABC DG-ID 20");
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    gen_dmr(dir);
    gen_dmr_t3(dir);
    gen_p25(dir);
    gen_p25_trunk(dir);
    gen_p25p2(dir);
    gen_nxdn(dir);
    gen_dstar(dir);
    gen_ysf(dir);
    return 0;
}
