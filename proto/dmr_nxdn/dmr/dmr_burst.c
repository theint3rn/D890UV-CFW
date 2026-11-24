/* DMR burst layout: sync, slot type, EMB, voice socket, CACH (TS 102 361-1 clause 6, 9.1, annex E). */
#include "dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

/* Table 9.2 */
const uint64_t DMR_SYNC_PATTERN[DMR_SYNC_COUNT] = {
    [DMR_SYNC_NONE]      = 0,
    [DMR_SYNC_BS_VOICE]  = 0x755FD7DF75F7ull,
    [DMR_SYNC_BS_DATA]   = 0xDFF57D75DF5Dull,
    [DMR_SYNC_MS_VOICE]  = 0x7F7D5DD57DFDull,
    [DMR_SYNC_MS_DATA]   = 0xD5D7F77FD757ull,
    [DMR_SYNC_MS_RC]     = 0x77D55F7DFD77ull,
    [DMR_SYNC_TS1_VOICE] = 0x5D577F7757FFull,
    [DMR_SYNC_TS1_DATA]  = 0xF7FDD5DDFD55ull,
    [DMR_SYNC_TS2_VOICE] = 0x7DFFD5F55D5Full,
    [DMR_SYNC_TS2_DATA]  = 0xD7557F5FF7F5ull,
    [DMR_SYNC_RESERVED]  = 0xDD7FF5D757DDull,
};

bool dmr_sync_is_voice(dmr_sync_t s)
{
    return s == DMR_SYNC_BS_VOICE || s == DMR_SYNC_MS_VOICE || s == DMR_SYNC_TS1_VOICE || s == DMR_SYNC_TS2_VOICE;
}

bool dmr_sync_is_data(dmr_sync_t s)
{
    return s == DMR_SYNC_BS_DATA || s == DMR_SYNC_MS_DATA || s == DMR_SYNC_TS1_DATA || s == DMR_SYNC_TS2_DATA;
}

dmr_sync_t dmr_sync_data_for(dmr_sync_t v)
{
    switch (v) {
    case DMR_SYNC_BS_VOICE: return DMR_SYNC_BS_DATA;
    case DMR_SYNC_MS_VOICE: return DMR_SYNC_MS_DATA;
    case DMR_SYNC_TS1_VOICE: return DMR_SYNC_TS1_DATA;
    case DMR_SYNC_TS2_VOICE: return DMR_SYNC_TS2_DATA;
    default: return v;
    }
}

static int popcount64(uint64_t v)
{
    return popcount32((uint32_t)v) + popcount32((uint32_t)(v >> 32));
}

dmr_sync_t dmr_sync_match(uint64_t w48, int max_err, int *err)
{
    dmr_sync_t best = DMR_SYNC_NONE;
    int bd = 99;
    w48 &= 0xFFFFFFFFFFFFull;
    for (int s = 1; s < DMR_SYNC_COUNT; s++) {
        int d = popcount64(w48 ^ DMR_SYNC_PATTERN[s]);
        if (d < bd) { bd = d; best = (dmr_sync_t)s; }
    }
    if (err) *err = bd;
    return bd <= max_err ? best : DMR_SYNC_NONE;
}

/* ---------------- burst field access ---------------- */
static void burst_bits(const uint8_t burst[DMR_BURST_DIBITS], uint8_t bits[264])
{
    dibits_to_bits(bits, burst, DMR_BURST_DIBITS);
}

uint64_t dmr_burst_sync(const uint8_t burst[DMR_BURST_DIBITS])
{
    uint64_t w = 0;
    for (int i = 54; i < 78; i++) w = (w << 2) | (burst[i] & 3u);
    return w;
}

void dmr_burst_info_bits(const uint8_t burst[DMR_BURST_DIBITS], uint8_t bits196[196])
{
    uint8_t b[264];
    burst_bits(burst, b);
    memcpy(bits196, b, 98);
    memcpy(bits196 + 98, b + 166, 98);
}

int dmr_burst_slot_type(const uint8_t burst[DMR_BURST_DIBITS], uint8_t *cc, uint8_t *dt)
{
    uint8_t b[264];
    burst_bits(burst, b);
    uint32_t cw = (bits_arr_val(b + 98, 10) << 10) | bits_arr_val(b + 156, 10);
    int e, d = fec_golay2087_dec(cw, &e);
    if (d < 0) return -1;
    *cc = (uint8_t)(d >> 4);
    *dt = (uint8_t)(d & 0xF);
    return 0;
}

void dmr_burst_voice(const uint8_t burst[DMR_BURST_DIBITS], uint8_t frames[3][9])
{
    uint8_t b[264], vs[216];
    burst_bits(burst, b);
    memcpy(vs, b, 108);
    memcpy(vs + 108, b + 156, 108);
    for (int f = 0; f < 3; f++) bits_pack(frames[f], vs + 72 * f, 72);
}

int dmr_burst_emb(const uint8_t burst[DMR_BURST_DIBITS], uint8_t *cc, uint8_t *pi, uint8_t *lcss, uint8_t emb32[32])
{
    uint8_t b[264];
    burst_bits(burst, b);
    uint16_t cw = (uint16_t)((bits_arr_val(b + 108, 8) << 8) | bits_arr_val(b + 148, 8));
    int e, d = fec_qr1676_dec(cw, &e);
    if (emb32) memcpy(emb32, b + 116, 32);
    if (d < 0) return -1;
    *cc = (uint8_t)(d >> 3);
    *pi = (uint8_t)((d >> 2) & 1);
    *lcss = (uint8_t)(d & 3);
    return 0;
}

static void put_sync(uint8_t b[264], dmr_sync_t sync)
{
    uint64_t s = DMR_SYNC_PATTERN[sync];
    for (int i = 0; i < 48; i++) b[108 + i] = (uint8_t)((s >> (47 - i)) & 1u);
}

void dmr_burst_build_data(uint8_t burst[DMR_BURST_DIBITS], dmr_sync_t sync, uint8_t cc, uint8_t dt,
                          const uint8_t bits196[196])
{
    uint8_t b[264];
    memcpy(b, bits196, 98);
    memcpy(b + 166, bits196 + 98, 98);
    uint32_t st = fec_golay2087_enc((uint8_t)(((cc & 0xFu) << 4) | (dt & 0xFu)));
    bits_arr_set(b + 98, 10, st >> 10);
    bits_arr_set(b + 156, 10, st & 0x3FFu);
    put_sync(b, sync);
    bits_to_dibits(burst, b, DMR_BURST_DIBITS);
}

static void put_voice(uint8_t b[264], const uint8_t frames[27])
{
    uint8_t vs[216];
    for (int f = 0; f < 3; f++) bits_unpack(vs + 72 * f, frames + 9 * f, 72);
    memcpy(b, vs, 108);
    memcpy(b + 156, vs + 108, 108);
}

void dmr_burst_build_voice_sync(uint8_t burst[DMR_BURST_DIBITS], dmr_sync_t sync, const uint8_t frames[27])
{
    uint8_t b[264];
    put_voice(b, frames);
    put_sync(b, sync);
    bits_to_dibits(burst, b, DMR_BURST_DIBITS);
}

void dmr_burst_build_voice_emb(uint8_t burst[DMR_BURST_DIBITS], uint8_t cc, uint8_t pi, uint8_t lcss,
                               const uint8_t emb32[32], const uint8_t frames[27])
{
    uint8_t b[264];
    put_voice(b, frames);
    uint16_t cw = fec_qr1676_enc((uint8_t)(((cc & 0xFu) << 3) | ((pi & 1u) << 2) | (lcss & 3u)));
    bits_arr_set(b + 108, 8, cw >> 8);
    memcpy(b + 116, emb32, 32);
    bits_arr_set(b + 148, 8, cw & 0xFFu);
    bits_to_dibits(burst, b, DMR_BURST_DIBITS);
}

/* ---------------- CACH (figure B.9) ----------------
 * transmit index (0 = TX(23)): AT 0, TC 4, LS1 8, LS0 12, H2 14, H1 18, H0 22; payload elsewhere.
 */
static const uint8_t CACH_TACT_POS[7] = { 0, 4, 8, 12, 14, 18, 22 };
static const uint8_t CACH_P_POS[17] = { 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23 };

int dmr_cach_decode(const uint8_t dibits[DMR_CACH_DIBITS], dmr_cach_t *c)
{
    uint8_t b[24], t[7];
    dibits_to_bits(b, dibits, DMR_CACH_DIBITS);
    for (int i = 0; i < 7; i++) t[i] = b[CACH_TACT_POS[i]];
    for (int i = 0; i < 17; i++) c->payload[i] = b[CACH_P_POS[i]];
    if (fec_hamming_7_4_dec(t) < 0) return -1;
    c->at = t[0];
    c->tc = t[1];
    c->lcss = (uint8_t)((t[2] << 1) | t[3]);
    return 0;
}

void dmr_cach_encode(const dmr_cach_t *c, uint8_t dibits[DMR_CACH_DIBITS])
{
    uint8_t b[24], t[7];
    t[0] = c->at & 1u;
    t[1] = c->tc & 1u;
    t[2] = (c->lcss >> 1) & 1u;
    t[3] = c->lcss & 1u;
    fec_hamming_7_4_enc(t);
    for (int i = 0; i < 7; i++) b[CACH_TACT_POS[i]] = t[i];
    for (int i = 0; i < 17; i++) b[CACH_P_POS[i]] = c->payload[i] & 1u;
    bits_to_dibits(dibits, b, DMR_CACH_DIBITS);
}
