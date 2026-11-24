/* NXDN frame builders (TS 1-A 4.4, 4.5, 4.6, 6.3.3). */
#include "nxdn.h"
#include "../common/bits.h"
#include <string.h>

static void put_fsw_lich(uint8_t bits[384], uint8_t lich7)
{
    uint8_t ld[8];
    bits_arr_set(bits, 20, NXDN_FSW);
    nxdn_lich_encode(lich7, ld);
    dibits_to_bits(bits + 20, ld, 8);
}

static void finish(const uint8_t bits[384], unsigned scr_last, uint8_t out[NXDN_FRAME_DIBITS])
{
    bits_to_dibits(out, bits, NXDN_FRAME_DIBITS);
    nxdn_scramble(out, 10, scr_last);
}

void nxdn_sacch_quarter(const uint8_t msg9[9], unsigned q, uint8_t ran, bool superframe, uint8_t info4[4])
{
    uint8_t structure = superframe ? (uint8_t)(3u - (q & 3u)) : 0u;
    memset(info4, 0, 4);
    info4[0] = (uint8_t)((structure << 6) | (ran & 0x3Fu));
    bits_set_field(info4, 8, 18, bits_field(msg9, (q & 3u) * 18u, 18));
}

static void voice_frame(nxdn_ch_t sch, uint8_t lich7, const uint8_t sacch_info[4], const uint8_t *facch1_first,
                        const uint8_t *facch1_second, const uint8_t *vch, bool efr, uint8_t out[NXDN_FRAME_DIBITS])
{
    uint8_t bits[384];
    memset(bits, 0, sizeof bits);
    put_fsw_lich(bits, lich7);
    nxdn_ch_encode(sch, sacch_info, bits + 36);
    const uint8_t *facch[2] = { facch1_first, facch1_second };
    for (int h = 0; h < 2; h++) {
        uint8_t *hb = bits + 96 + 144 * h;
        if (facch[h]) nxdn_ch_encode(NXDN_CH_FACCH1, facch[h], hb);
        else if (vch && efr) bits_unpack(hb, vch + 18 * h, 144);
        else if (vch) { bits_unpack(hb, vch + 18 * h, 72); bits_unpack(hb + 72, vch + 18 * h + 9, 72); }
    }
    finish(bits, NXDN_FRAME_DIBITS, out);
}

void nxdn_tx_voice_frame(uint8_t lich7, const uint8_t sacch_info[4], const uint8_t *facch1_first,
                         const uint8_t *facch1_second, const uint8_t *vch, bool efr,
                         uint8_t out[NXDN_FRAME_DIBITS])
{
    voice_frame(NXDN_CH_SACCH, lich7, sacch_info, facch1_first, facch1_second, vch, efr, out);
}

void nxdn_tx_typed_voice_frame(uint8_t lich7, const uint8_t scch_info[4], const uint8_t *facch1_first,
                               const uint8_t *facch1_second, const uint8_t *vch, uint8_t out[NXDN_FRAME_DIBITS])
{
    voice_frame(NXDN_CH_SCCH, lich7, scch_info, facch1_first, facch1_second, vch, false, out);
}

void nxdn_tx_udch_frame(uint8_t lich7, const uint8_t info[23], uint8_t out[NXDN_FRAME_DIBITS])
{
    uint8_t bits[384];
    memset(bits, 0, sizeof bits);
    put_fsw_lich(bits, lich7);
    nxdn_ch_encode(NXDN_CH_UDCH, info, bits + 36);
    finish(bits, NXDN_FRAME_DIBITS, out);
}

void nxdn_tx_cac_frame(uint8_t lich7, const uint8_t info[19], uint8_t out[NXDN_FRAME_DIBITS])
{
    uint8_t bits[384];
    memset(bits, 0, sizeof bits);
    put_fsw_lich(bits, lich7);
    nxdn_ch_encode(NXDN_CH_CAC, info, bits + 36);
    /* E (collision control, 336..359) left 0; post field 360..383 = +3 +3 +3 then the 9 tail symbols */
    bits_arr_set(bits + 360, 24, NXDN_PREAMBLE);
    finish(bits, 180, out);
}

void nxdn_tx_lcac_frame(uint8_t lich7, const uint8_t *info, bool short_cac, uint8_t out[NXDN_FRAME_DIBITS])
{
    uint8_t bits[384];
    memset(bits, 0, sizeof bits);
    put_fsw_lich(bits, lich7);
    nxdn_ch_encode(short_cac ? NXDN_CH_SCAC : NXDN_CH_LCAC, info, bits + 36);
    finish(bits, 144, out);          /* guard time 288..383 is not transmitted / not scrambled */
}

unsigned nxdn_tx_preamble(unsigned n_extra4, uint8_t *out)
{
    static const uint8_t extra[4] = { 1, 1, 3, 3 };          /* +3 +3 -3 -3 */
    unsigned n = 0;
    for (unsigned i = 0; i < n_extra4; i++) for (int k = 0; k < 4; k++) out[n++] = extra[k];
    uint8_t b[24];
    bits_arr_set(b, 24, NXDN_PREAMBLE);
    bits_to_dibits(out + n, b, 12);
    return n + 12;
}

uint8_t nxdn_lich_rdch_voice(bool superframe, uint8_t steal)
{
    nxdn_lich_t l = { NXDN_RF_RDCH, superframe ? NXDN_USC_SACCH_SS : NXDN_USC_SACCH_NS,
                      superframe ? steal : NXDN_STEAL_BOTH, 0 };
    return nxdn_lich_pack(&l);
}

uint8_t nxdn_lich_rdch_data(bool facch2)
{
    nxdn_lich_t l = { NXDN_RF_RDCH, NXDN_USC_UDCH, facch2 ? NXDN_STEAL_BOTH : NXDN_STEAL_NONE, 0 };
    return nxdn_lich_pack(&l);
}
