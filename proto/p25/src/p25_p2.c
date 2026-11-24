/*
 * p25_p2.c - P25 Phase 2 TDMA MAC layer (see p25_p2.h).
 * Tables (ISCH generator, DUID code, AMBE interleave, MAC message lengths,
 * burst field positions) follow TIA-102.BBAC/BBAD; they were cross-checked
 * against the values published in OP25 (boatbod/op25 p25p2_*.cc) by the host
 * tests. No OP25 code is copied.
 */
#include "p25_p2.h"
#include "p25_fec.h"
#include "p25_p1.h"   /* enum p25_voice_crypt */
#include <string.h>

/* ------------------------------------------------------------------ ISCH */
/* The 128 I-ISCH code words form an affine code: cw(v) = C0 ^ sum(v_i * B_i) */
static const uint64_t isch_c0 = 0x184229d461ULL;
static const uint64_t isch_basis[7] = {
    0x343d8597ULL, 0x58cbaa4eULL, 0x9da3a171ULL, 0x9048d9b72ULL, 0x20807f7ffULL, 0xc00ded18eULL, 0x100f4b1758ULL
};

uint64_t p25_p2_isch_encode(unsigned v)
{
    uint64_t c = isch_c0;
    for (int i = 0; i < 7; i++) if (v & (1u << i)) c ^= isch_basis[i];
    return c;
}

int p25_p2_isch_decode(uint64_t cw, int *dist)
{
    cw &= 0xFFFFFFFFFFULL;
    int best = -1, bd = 99;
    int ds = (int)p25_popcount64(cw ^ P25_P2_SYNC);
    if (ds <= 7) { best = -2; bd = ds; }
    for (unsigned v = 0; v < 128 && bd; v++) {
        int d = (int)p25_popcount64(cw ^ p25_p2_isch_encode(v));
        if (d < bd) { bd = d; best = (int)v; }
    }
    if (dist) *dist = bd;
    return bd <= 7 ? best : -1;
}

/* DUID (8,4): info in the high nibble, parity = rows 0x8d 0x4b 0x2e 0x17 */
uint8_t p25_p2_duid_encode(unsigned d)
{
    static const uint8_t rows[4] = { 0x8d, 0x4b, 0x2e, 0x17 };
    uint8_t c = 0;
    for (int i = 0; i < 4; i++) if (d & (8u >> i)) c ^= rows[i];
    return c;
}

int p25_p2_duid_decode(uint8_t cw)
{
    for (unsigned d = 0; d < 16; d++)
        if (p25_popcount32((uint32_t)(p25_p2_duid_encode(d) ^ cw)) <= 1) return (int)d;
    return -1;
}

/* ------------------------------------------------------------ scrambler */
void p25_p2_scrambler_init(p25_p2_scrambler_t *s, uint32_t wacn, uint16_t sysid, uint16_t nac)
{
    const uint64_t M44 = (1ULL << 44) - 1;
    uint64_t seed = (((uint64_t)(wacn & 0xFFFFF)) << 24) | ((uint64_t)(sysid & 0xFFF) << 12) | (nac & 0xFFF);
    /* load the seed into the segmented (Galois-style) register so that the
     * first 44 output bits equal the seed */
    uint64_t reg = (seed ^ (seed >> 4) ^ (seed >> 9) ^ (seed >> 15) ^ (seed >> 20) ^ (seed >> 34)) & M44;
    memset(s->bits, 0, sizeof s->bits);
    for (unsigned i = 0; i < 4320; i++) {
        if ((reg >> 43) & 1u) s->bits[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
        unsigned s1 = (unsigned)(reg >> 40) & 0xF, s2 = (unsigned)(reg >> 35) & 0x1F, s3 = (unsigned)(reg >> 29) & 0x3F;
        unsigned s4 = (unsigned)(reg >> 24) & 0x1F, s5 = (unsigned)(reg >> 10) & 0x3FFF, s6 = (unsigned)reg & 0x3FF;
        unsigned c1 = s1 >> 3, c2 = s2 >> 4, c3 = s3 >> 5, c4 = s4 >> 4, c5 = s5 >> 13, c6 = s6 >> 9;
        s1 = ((s1 << 1) & 0xF) | (c1 ^ c2);
        s2 = ((s2 << 1) & 0x1F) | (c1 ^ c3);
        s3 = ((s3 << 1) & 0x3F) | (c1 ^ c4);
        s4 = ((s4 << 1) & 0x1F) | (c1 ^ c5);
        s5 = ((s5 << 1) & 0x3FFF) | (c1 ^ c6);
        s6 = ((s6 << 1) & 0x3FF) | c1;
        reg = ((uint64_t)s1 << 40) | ((uint64_t)s2 << 35) | ((uint64_t)s3 << 29) | ((uint64_t)s4 << 24) |
              ((uint64_t)s5 << 10) | s6;
    }
    s->valid = true;
}

/* ------------------------------------------------------------ AMBE FEC */
/* air bit k (0..71) <- code word c (bits 7..5) bit b (bits 4..0, LSB = 0) */
#define CB(c, b) (uint8_t)(((c) << 5) | (b))
static const uint8_t ambe_map[72] = {
    CB(0,23), CB(0,5),  CB(1,10), CB(2,3),  CB(0,22), CB(0,4),  CB(1,9),  CB(2,2),
    CB(0,21), CB(0,3),  CB(1,8),  CB(2,1),  CB(0,20), CB(0,2),  CB(1,7),  CB(2,0),
    CB(0,19), CB(0,1),  CB(1,6),  CB(3,13), CB(0,18), CB(0,0),  CB(1,5),  CB(3,12),
    CB(0,17), CB(1,22), CB(1,4),  CB(3,11), CB(0,16), CB(1,21), CB(1,3),  CB(3,10),
    CB(0,15), CB(1,20), CB(1,2),  CB(3,9),  CB(0,14), CB(1,19), CB(1,1),  CB(3,8),
    CB(0,13), CB(1,18), CB(1,0),  CB(3,7),  CB(0,12), CB(1,17), CB(2,10), CB(3,6),
    CB(0,11), CB(1,16), CB(2,9),  CB(3,5),  CB(0,10), CB(1,15), CB(2,8),  CB(3,4),
    CB(0,9),  CB(1,14), CB(2,7),  CB(3,3),  CB(0,8),  CB(1,13), CB(2,6),  CB(3,2),
    CB(0,7),  CB(1,12), CB(2,5),  CB(3,1),  CB(0,6),  CB(1,11), CB(2,4),  CB(3,0)
};

void p25_ambe_encode(const uint8_t f[P25_AMBE_BYTES], uint8_t dib[36])
{
    uint32_t u0 = p25_bits_get(f, 0, 12), u1 = p25_bits_get(f, 12, 12);
    uint32_t u2 = p25_bits_get(f, 24, 11), u3 = p25_bits_get(f, 35, 14);
    uint32_t pn = 16u * u0;
    uint32_t c[4];
    c[0] = p25_golay24_encode(u0);
    c[1] = p25_golay23_encode(u1) ^ p25_pn_bits(&pn, 23);
    c[2] = u2;
    c[3] = u3;
    for (int i = 0; i < 36; i++) {
        uint8_t a = ambe_map[2 * i], b = ambe_map[2 * i + 1];
        dib[i] = (uint8_t)((((c[a >> 5] >> (a & 31)) & 1u) << 1) | ((c[b >> 5] >> (b & 31)) & 1u));
    }
}

int p25_ambe_decode(const uint8_t dib[36], uint8_t f[P25_AMBE_BYTES])
{
    uint32_t c[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 72; k++) {
        unsigned bit = (dib[k >> 1] >> (1 - (k & 1))) & 1u;
        uint8_t m = ambe_map[k];
        c[m >> 5] |= (uint32_t)bit << (m & 31);
    }
    int e0, e1, bad = 0;
    int u0 = p25_golay24_decode(c[0], &e0);
    if (u0 < 0) { u0 = (int)p25_golay23_decode(c[0] >> 1, &e0); bad = 1; }
    uint32_t pn = 16u * (uint32_t)u0;
    uint32_t u1 = p25_golay23_decode(c[1] ^ p25_pn_bits(&pn, 23), &e1);
    memset(f, 0, P25_AMBE_BYTES);
    p25_bits_put(f, 0, 12, (uint32_t)u0);
    p25_bits_put(f, 12, 12, u1);
    p25_bits_put(f, 24, 11, c[2]);
    p25_bits_put(f, 35, 14, c[3]);
    return bad ? -1 : e0 + e1;
}

/* ------------------------------------------------------------------ ESS */
void p25_p2_ess_encode(const p25_p2_ess_t *e, uint8_t essb[16], uint8_t essa[28])
{
    uint8_t b[12];
    b[0] = e->algid;
    b[1] = (uint8_t)(e->kid >> 8);
    b[2] = (uint8_t)e->kid;
    memcpy(b + 3, e->mi, 9);
    for (int i = 0; i < 16; i++) essb[i] = (uint8_t)p25_bits_get(b, 6u * (unsigned)i, 6);
    p25_rs_encode(essb, 16, 28, essa);
}

int p25_p2_ess_decode(const uint8_t essb[16], const uint8_t essa[28], p25_p2_ess_t *e)
{
    uint8_t cw[44], b[12];
    memcpy(cw, essb, 16);
    memcpy(cw + 16, essa, 28);
    int r = p25_rs_decode(cw, 44, 28, NULL, 0);
    if (r < 0) return -1;
    for (int i = 0; i < 16; i++) p25_bits_put(b, 6u * (unsigned)i, 6, cw[i]);
    e->algid = b[0];
    e->kid = (uint16_t)((b[1] << 8) | b[2]);
    memcpy(e->mi, b + 3, 9);
    return r;
}

/* ------------------------------------------------------------ MAC PDUs */
/* message length by MAC opcode (TIA-102.BBAD tables; 0 = unknown) */
static const uint8_t mac_len[256] = {
     0,  7,  8,  7,  0, 16,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0, 14, 15,  0,  0, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     5,  7,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     9,  7,  9,  0,  9,  8,  9,  0, 10, 10,  9,  0, 10,  0,  0,  0,
     0,  0,  0,  0,  9,  7,  0,  0, 10,  0,  7,  0, 10,  8, 14,  7,
     9,  9,  0,  0,  9,  0,  0,  9, 10,  0,  7, 10, 10,  7,  0,  9,
     9, 29,  9,  9,  9,  9, 10, 13,  9,  9,  9, 11,  9,  9,  0,  0,
     8,  0,  0,  7, 11,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16,  0,  0, 11, 13, 11, 11, 11, 10,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    11,  0,  0,  8, 15, 12, 15, 32, 12, 12,  0, 27, 14, 29, 29, 32,
     0,  0,  0,  0,  0,  0,  9,  0, 14, 29, 11, 27, 14,  0, 40, 11,
    28,  0,  0, 14, 17, 14,  0,  0, 16,  8, 11,  0, 13, 19,  0,  0,
     0,  0, 16, 14,  0,  0, 12,  0, 22,  0, 11, 13, 11,  0, 15,  0
};

unsigned p25_mac_msg_len(const uint8_t *msg)
{
    uint8_t op = msg[0];
    if ((op >> 6) == 2) return msg[2] & 0x3Fu;      /* manufacturer: length in octet 3 */
    return mac_len[op];
}

int p25_mac_messages(const p25_mac_pdu_t *m, p25_mac_msg_cb cb, void *ctx)
{
    unsigned n = m->sacch ? P25_SACCH_BYTES : P25_FACCH_BYTES, i = 1, found = 0;
    if (m->opcode == P25_MAC_PTT || m->opcode == P25_MAC_END_PTT) return 0;
    while (i < n) {
        const uint8_t *msg = m->pdu + i;
        if (msg[0] == 0) break;                      /* null / fill */
        unsigned len = p25_mac_msg_len(msg);
        if (len == 0 || i + len > n) break;
        p25_tsbk_t t, *pt = NULL;
        uint8_t op = msg[0];
        if ((op >> 6) == 1 && len == 9) {             /* abbreviated: OSP layout, op = 0x40|tsbk */
            p25_osp_parse(op & 0x3F, 0x00, msg + 1, &t);
            pt = &t;
        } else if (op == 0x7B && len == 11) {         /* NET_STS_BCST (Phase 2: chT, chR) */
            /* rebuild in TSBK layout: lra(8) wacn(20) sysid(12) ch(16) ssc(8) */
            uint8_t tb[8];
            memcpy(tb, msg + 1, 7);                   /* lra .. chT (56 bits) */
            tb[7] = msg[10];                          /* ssc */
            p25_osp_parse(0x3B, 0x00, tb, &t);
            pt = &t;
        } else if (op == 0xC3 && len == 8) {          /* GRP_V_CH_GRANT_UPDT_EXP */
            uint8_t a[8] = { msg[1], 0, msg[2], msg[3], msg[4], msg[5], msg[6], msg[7] };
            p25_osp_parse(0x03, 0x00, a, &t);
            pt = &t;
        }
        if (cb) cb(ctx, op, msg, len, pt);
        found++;
        i += len;
    }
    return (int)found;
}

static void mac_hdr(p25_mac_pdu_t *m, bool sacch, uint8_t op)
{
    memset(m, 0, sizeof *m);
    m->sacch = sacch;
    m->opcode = op;
    m->pdu[0] = (uint8_t)(op << 5);
}

void p25_mac_ptt(p25_mac_pdu_t *m, bool sacch, uint32_t src, uint16_t grp)
{
    mac_hdr(m, sacch, P25_MAC_PTT);
    m->algid = m->pdu[10] = 0x80;
    m->src = src; m->grp = grp;
    m->pdu[13] = (uint8_t)(src >> 16); m->pdu[14] = (uint8_t)(src >> 8); m->pdu[15] = (uint8_t)src;
    m->pdu[16] = (uint8_t)(grp >> 8); m->pdu[17] = (uint8_t)grp;
}

void p25_mac_end_ptt(p25_mac_pdu_t *m, bool sacch, uint16_t colour, uint32_t src, uint16_t grp)
{
    mac_hdr(m, sacch, P25_MAC_END_PTT);
    m->colour = colour & 0xFFF; m->src = src; m->grp = grp;
    m->pdu[1] = (uint8_t)((colour >> 8) & 0x0F); m->pdu[2] = (uint8_t)colour;
    m->pdu[13] = (uint8_t)(src >> 16); m->pdu[14] = (uint8_t)(src >> 8); m->pdu[15] = (uint8_t)src;
    m->pdu[16] = (uint8_t)(grp >> 8); m->pdu[17] = (uint8_t)grp;
}

void p25_mac_idle(p25_mac_pdu_t *m, bool sacch)
{
    mac_hdr(m, sacch, P25_MAC_IDLE);
}

static void mac_unpack(p25_mac_pdu_t *m)
{
    const uint8_t *b = m->pdu;
    m->opcode = b[0] >> 5;
    m->offset = (b[0] >> 2) & 7;
    if (m->opcode == P25_MAC_PTT) {
        memcpy(m->mi, b + 1, 9);
        m->algid = b[10];
        m->kid = (uint16_t)((b[11] << 8) | b[12]);
    } else if (m->opcode == P25_MAC_END_PTT) {
        m->colour = (uint16_t)(((b[1] & 0x0F) << 8) | b[2]);
    }
    if (m->opcode == P25_MAC_PTT || m->opcode == P25_MAC_END_PTT) {
        m->src = ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | b[15];
        m->grp = (uint16_t)((b[16] << 8) | b[17]);
    }
}

/* -------------------------------------------------------- burst layout */
static const uint8_t duid_pos[4] = { 20, 57, 142, 179 };
static const uint8_t v4_pos[4] = { 21, 58, 106, 143 };
#define ESSB_POS 94
/* FACCH / SACCH data runs: {start, length} in dibits */
static const uint8_t facch_runs[4][2] = { { 21, 36 }, { 58, 31 }, { 110, 32 }, { 143, 36 } };
static const uint8_t sacch_runs[3][2] = { { 21, 36 }, { 58, 84 }, { 143, 36 } };

static bool is_duid_pos(unsigned p) { return p == 20 || p == 57 || p == 142 || p == 179; }

static bool burst_scrambled(unsigned t)
{
    return t == P25_P2_4V || t == P25_P2_2V || t == P25_P2_SACCH_S || t == P25_P2_FACCH_S || t == P25_P2_LCCH_S;
}

static void slot_header(uint8_t *out, int isch_value, unsigned duid)
{
    memset(out, 0, P25_P2_SLOT);
    uint64_t isch = isch_value < 0 ? P25_P2_SYNC : p25_p2_isch_encode((unsigned)isch_value);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)((isch >> (38 - 2 * i)) & 3);
    uint8_t d = p25_p2_duid_encode(duid);
    for (int i = 0; i < 4; i++) out[duid_pos[i]] = (d >> (6 - 2 * i)) & 3;
}

static void slot_scramble(uint8_t *s, unsigned slot, const p25_p2_scrambler_t *scr)
{
    if (!scr || !scr->valid) return;
    for (unsigned p = 20; p < P25_P2_SLOT; p++)
        if (!is_duid_pos(p)) s[p] ^= p25_p2_scramble_dibit(scr, slot, p);
}

int p25_p2_isch_for_slot(unsigned slot, unsigned cnt)
{
    slot %= 12;
    if ((slot & 2) != 0) return -1;                  /* 2,3,6,7,10,11: S-ISCH */
    return (int)p25_p2_isch_value(slot & 3, slot >> 2, 0, cnt);
}

void p25_p2_build_4v(uint8_t out[P25_P2_SLOT], unsigned slot, int isch, const p25_p2_scrambler_t *scr,
                     const uint8_t frames[4][P25_AMBE_BYTES], const uint8_t essb4[4])
{
    slot_header(out, isch, P25_P2_4V);
    for (int i = 0; i < 4; i++) p25_ambe_encode(frames[i], out + v4_pos[i]);
    for (int i = 0; i < 4; i++) {
        uint8_t h = essb4 ? essb4[i] : 0;
        out[ESSB_POS + 3 * i] = (h >> 4) & 3;
        out[ESSB_POS + 3 * i + 1] = (h >> 2) & 3;
        out[ESSB_POS + 3 * i + 2] = h & 3;
    }
    slot_scramble(out, slot, scr);
}

/* ESS-A: 28 hexbits over dibits 94..141 then 143..178 */
static unsigned essa_dibit_pos(unsigned j) { return j < 48 ? ESSB_POS + j : 143 + (j - 48); }

void p25_p2_build_2v(uint8_t out[P25_P2_SLOT], unsigned slot, int isch, const p25_p2_scrambler_t *scr,
                     const uint8_t frames[2][P25_AMBE_BYTES], const uint8_t essa[28])
{
    slot_header(out, isch, P25_P2_2V);
    for (int i = 0; i < 2; i++) p25_ambe_encode(frames[i], out + v4_pos[i]);
    for (unsigned i = 0; i < 28; i++) {
        uint8_t h = essa ? essa[i] : 0;
        out[essa_dibit_pos(3 * i)] = (h >> 4) & 3;
        out[essa_dibit_pos(3 * i + 1)] = (h >> 2) & 3;
        out[essa_dibit_pos(3 * i + 2)] = h & 3;
    }
    slot_scramble(out, slot, scr);
}

/* MAC payload <-> hexbit code word */
static void mac_runs(bool sacch, const uint8_t (**runs)[2], unsigned *nr)
{
    if (sacch) { *runs = sacch_runs; *nr = 3; }
    else { *runs = facch_runs; *nr = 4; }
}

void p25_p2_build_mac(uint8_t out[P25_P2_SLOT], unsigned slot, int isch, const p25_p2_scrambler_t *scr,
                      const p25_mac_pdu_t *m)
{
    bool sacch = m->sacch;
    unsigned nbytes = sacch ? P25_SACCH_BYTES : P25_FACCH_BYTES;
    unsigned k = sacch ? 30 : 26, tx_parity = sacch ? 22 : 19;
    uint8_t bits[24], hex[63];
    memset(bits, 0, sizeof bits);
    memcpy(bits, m->pdu, nbytes);
    p25_bits_put(bits, nbytes * 8, 12, p25_crc12(bits, nbytes * 8));
    for (unsigned i = 0; i < k; i++) hex[i] = (uint8_t)p25_bits_get(bits, 6 * i, 6);
    p25_rs_encode(hex, k, 28, hex + k);
    unsigned type = scr ? (sacch ? P25_P2_SACCH_S : P25_P2_FACCH_S) : (sacch ? P25_P2_SACCH : P25_P2_FACCH);
    slot_header(out, isch, type);
    const uint8_t (*runs)[2];
    unsigned nr, h = 0, ph = 0;
    mac_runs(sacch, &runs, &nr);
    for (unsigned r = 0; r < nr; r++)
        for (unsigned j = 0; j < runs[r][1]; j++) {
            out[runs[r][0] + j] = (hex[h] >> (4 - 2 * ph)) & 3;
            if (++ph == 3) { ph = 0; h++; }
        }
    (void)tx_parity;
    slot_scramble(out, slot, scr);
}

/* returns RS-corrected symbols or -1; fills m (CRC checked) */
static int mac_decode(const uint8_t *s, bool sacch, p25_mac_pdu_t *m)
{
    unsigned k = sacch ? 30 : 26, n_tx = sacch ? 52 : 45, n_full = sacch ? 58 : 54;
    uint8_t hex[63];
    memset(hex, 0, sizeof hex);
    const uint8_t (*runs)[2];
    unsigned nr, h = 0, ph = 0;
    mac_runs(sacch, &runs, &nr);
    for (unsigned r = 0; r < nr; r++)
        for (unsigned j = 0; j < runs[r][1]; j++) {
            hex[h] = (uint8_t)((hex[h] << 2) | (s[runs[r][0] + j] & 3));
            if (++ph == 3) { ph = 0; h++; }
        }
    (void)n_tx;
    int r = p25_rs_decode_punct(hex, n_full, 28, n_full - n_tx);
    if (r < 0) return -1;
    uint8_t bits[24];
    memset(bits, 0, sizeof bits);
    for (unsigned i = 0; i < k; i++) p25_bits_put(bits, 6 * i, 6, hex[i]);
    unsigned nbytes = sacch ? P25_SACCH_BYTES : P25_FACCH_BYTES;
    if (p25_crc12(bits, nbytes * 8) != p25_bits_get(bits, nbytes * 8, 12)) return -1;
    memset(m, 0, sizeof *m);
    m->sacch = sacch;
    memcpy(m->pdu, bits, nbytes);
    mac_unpack(m);
    return r;
}

/* ------------------------------------------------------------------- RX */
void p25_p2_rx_init(p25_p2_rx_t *rx, const p25_p2_cb_t *cb, void *ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->cb = cb;
    rx->ctx = ctx;
    rx->channel_filter = -1;
    rx->slot_id = -1;
}

void p25_p2_rx_set_scrambler(p25_p2_rx_t *rx, const p25_p2_scrambler_t *scr) { rx->scr = scr; }

void p25_p2_rx_set_crypt(p25_p2_rx_t *rx, const p25_keyring_t *keys, p25_crypt_t *crypt)
{
    rx->keys = keys;
    rx->crypt = crypt;
    rx->vc = P25_VC_CLEAR;
}

static void p2_crypt_set(p25_p2_rx_t *rx, uint8_t algid, uint16_t kid, const uint8_t mi[9])
{
    if (algid == P25_ALG_CLEAR) { rx->vc = P25_VC_CLEAR; return; }
    if (rx->crypt && rx->keys && p25_crypt_prepare(rx->crypt, rx->keys, algid, kid, mi, true))
        rx->vc = P25_VC_DECRYPTED;
    else
        rx->vc = P25_VC_NOKEY;
}

static void p2_emit(p25_p2_rx_t *rx, p25_p2_event_t *ev)
{
    if (rx->cb && rx->cb->on_event) rx->cb->on_event(rx->ctx, ev);
}

void p25_p2_rx_slot(p25_p2_rx_t *rx, const uint8_t in[P25_P2_SLOT], unsigned slot)
{
    uint8_t s[P25_P2_SLOT];
    p25_p2_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.slot = (uint8_t)slot;
    ev.channel = (uint8_t)p25_p2_slot_channel(slot);
    ev.rssi = rx->rssi_n ? (int16_t)(rx->rssi_sum / rx->rssi_n) : 0;
    unsigned ch = ev.channel;
    if (rx->channel_filter >= 0 && (int)ch != rx->channel_filter) return;
    uint8_t dc = 0;
    for (int i = 0; i < 4; i++) dc = (uint8_t)((dc << 2) | (in[duid_pos[i]] & 3));
    int type = p25_p2_duid_decode(dc);
    if (type < 0) return;
    ev.burst = (uint8_t)type;
    memcpy(s, in, P25_P2_SLOT);
    if (burst_scrambled((unsigned)type)) {
        if (!rx->scr || !rx->scr->valid) {
            if (type != P25_P2_4V && type != P25_P2_2V) { ev.type = P25_P2_EV_MAC_BAD; p2_emit(rx, &ev); }
            return;
        }
        slot_scramble(s, slot, rx->scr);
    }
    switch (type) {
    case P25_P2_4V:
    case P25_P2_2V: {
        int nv = type == P25_P2_4V ? 4 : 2;
        bool mine = rx->channel_filter == (int)ch;
        unsigned burst = type == P25_P2_4V ? rx->essb_n[ch] : 4u;   /* 4V index in the superframe */
        for (int i = 0; i < nv; i++) {
            uint8_t f[P25_AMBE_BYTES];
            int e = p25_ambe_decode(s + v4_pos[i], f);
            if (mine && (rx->vc == P25_VC_NOKEY || rx->vc == P25_VC_WAIT_MI)) continue;   /* muted */
            if (mine && rx->vc == P25_VC_DECRYPTED && burst <= 4) p25_crypt_ambe(rx->crypt, burst, (unsigned)i, f);
            if (rx->cb && rx->cb->on_voice) rx->cb->on_voice(rx->ctx, ch, f, e);
        }
        if (type == P25_P2_4V) {
            if (rx->essb_n[ch] < 4) {
                for (int i = 0; i < 4; i++)
                    rx->essb[ch][4 * rx->essb_n[ch] + i] =
                        (uint8_t)((s[ESSB_POS + 3 * i] << 4) | (s[ESSB_POS + 3 * i + 1] << 2) | s[ESSB_POS + 3 * i + 2]);
                rx->essb_n[ch]++;
            }
        } else {
            uint8_t essa[28];
            for (unsigned i = 0; i < 28; i++)
                essa[i] = (uint8_t)((s[essa_dibit_pos(3 * i)] << 4) | (s[essa_dibit_pos(3 * i + 1)] << 2) |
                                    s[essa_dibit_pos(3 * i + 2)]);
            if (rx->essb_n[ch] == 4) {
                int r = p25_p2_ess_decode(rx->essb[ch], essa, &ev.ess);
                if (r >= 0) {
                    ev.type = P25_P2_EV_ESS;
                    ev.errs = r;
                    p2_emit(rx, &ev);
                    /* ESS = MI of the next superframe */
                    if (rx->channel_filter == (int)ch) p2_crypt_set(rx, ev.ess.algid, ev.ess.kid, ev.ess.mi);
                }
            }
            rx->essb_n[ch] = 0;
        }
        break;
    }
    case P25_P2_FACCH: case P25_P2_FACCH_S: case P25_P2_SACCH: case P25_P2_SACCH_S: {
        bool sacch = type == P25_P2_SACCH || type == P25_P2_SACCH_S;
        int r = mac_decode(s, sacch, &ev.mac);
        ev.type = r < 0 ? P25_P2_EV_MAC_BAD : P25_P2_EV_MAC;
        ev.errs = r;
        if (r >= 0 && ev.mac.opcode == P25_MAC_PTT) {
            rx->essb_n[ch] = 0;
            /* call start: take ALGID/KID/MI unless this call is already keyed */
            if (rx->channel_filter == (int)ch && (rx->vc == P25_VC_CLEAR || !rx->crypt || rx->crypt->algid != ev.mac.algid))
                p2_crypt_set(rx, ev.mac.algid, ev.mac.kid, ev.mac.mi);
        }
        if (r >= 0 && ev.mac.opcode == P25_MAC_END_PTT && rx->channel_filter == (int)ch) rx->vc = P25_VC_CLEAR;
        p2_emit(rx, &ev);
        break;
    }
    default:
        break;                                  /* LCCH not handled */
    }
}

static bool isch_slot_ok(int v)
{
    unsigned chn = ((unsigned)v >> 5) & 3, loc = ((unsigned)v >> 3) & 3;
    return chn < 2 && loc < 3;
}

void p25_p2_rx_push(p25_p2_rx_t *rx, const uint8_t *dib, size_t n, int16_t rssi)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t x = dib[i] & 3;
        rx->sreg = ((rx->sreg << 2) | x) & 0xFFFFFFFFFFULL;
        if (!rx->in_sync) {
            if (p25_popcount64(rx->sreg ^ P25_P2_SYNC) <= 4) {
                rx->in_sync = true;
                rx->slot_id = -1;
                rx->miss = 0;
                rx->pos = 20;
                rx->rssi_sum = 0;
                rx->rssi_n = 0;
                for (int k = 0; k < 20; k++) rx->buf[k] = (uint8_t)((P25_P2_SYNC >> (38 - 2 * k)) & 3);
            }
            continue;
        }
        rx->buf[rx->pos++] = x;
        rx->rssi_sum += rssi;
        rx->rssi_n++;
        if (rx->pos == 20) {
            /* ISCH of this slot complete: identify the slot */
            uint64_t cw = 0;
            for (int k = 0; k < 20; k++) cw = (cw << 2) | rx->buf[k];
            int v = p25_p2_isch_decode(cw, NULL);
            int prev = rx->slot_id;
            int next = prev < 0 ? -1 : (prev + 1) % 12;
            if (v >= 0 && isch_slot_ok(v)) {
                unsigned id = (((unsigned)v >> 3) & 3) * 4 + (((unsigned)v >> 5) & 3);
                if (rx->slot_id < 0 || (int)id != next) {
                    p25_p2_event_t ev;
                    memset(&ev, 0, sizeof ev);
                    ev.type = P25_P2_EV_SYNC;
                    ev.slot = (uint8_t)id;
                    rx->slot_id = (int8_t)id;
                    p2_emit(rx, &ev);
                } else {
                    rx->slot_id = (int8_t)id;
                }
                rx->miss = 0;
            } else if (v == -2) {
                rx->slot_id = (int8_t)next;
                rx->miss = 0;
            } else {
                rx->slot_id = (int8_t)next;
                if (++rx->miss >= 6) {
                    p25_p2_event_t ev;
                    memset(&ev, 0, sizeof ev);
                    ev.type = P25_P2_EV_SYNC_LOST;
                    p2_emit(rx, &ev);
                    rx->in_sync = false;
                    rx->sreg = 0;
                    continue;
                }
            }
        }
        if (rx->pos == P25_P2_SLOT) {
            if (rx->slot_id >= 0) p25_p2_rx_slot(rx, rx->buf, (unsigned)rx->slot_id);
            rx->pos = 0;
            rx->rssi_sum = 0;
            rx->rssi_n = 0;
        }
    }
}
