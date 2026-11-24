/* DMR receiver: burst timing from the 48-bit sync, CACH, per-slot voice superframes,
 * embedded LC, CSBK/MBC, data reassembly (TS 102 361-1 clauses 5-8, TS 102 361-3).
 */
#include "dmr.h"
#include "../common/bits.h"
#include "../common/fec.h"
#include <string.h>

#define SYNC_LOSS_BURSTS 13     /* one voice superframe per slot = 12 bursts between syncs */

void dmr_rx_init(dmr_rx_t *rx, const dmr_rx_cfg_t *cfg, const dmr_cb_t *cb)
{
    memset(rx, 0, sizeof *rx);
    if (cfg) rx->cfg = *cfg;
    else { rx->cfg.sync_max_err = 4; rx->cfg.cc_filter = -1; rx->cfg.bs_cach = true; }
    if (cb) rx->cb = *cb;
    for (int s = 0; s < 2; s++) rx->slot[s].sf_index = 0xFF;
}

static void emit(dmr_rx_t *rx, dmr_event_t *ev)
{
    if (rx->cb.event) rx->cb.event(rx->cb.user, ev);
}

static void ev_init(dmr_event_t *ev, dmr_ev_type_t t, uint8_t slot, uint8_t cc, dmr_sync_t sync)
{
    memset(ev, 0, sizeof *ev);
    ev->type = t;
    ev->slot = slot;
    ev->cc = cc;
    ev->sync = sync;
    ev->crc_ok = -1;
}

static void voice_end(dmr_rx_t *rx, uint8_t slot, const dmr_lc_t *lc)
{
    dmr_slot_state_t *st = &rx->slot[slot];
    if (!st->active) return;
    bool foreign = rx->cfg.cc_filter >= 0 && !st->lc_valid && st->cc_state <= 0;   /* never passed the CC filter */
    st->cc_state = 0;
    st->a_held = false;
    if (foreign) {
        st->active = false;
        st->sf_index = 0xFF;
        st->emb_frags = 0;
        st->enc = false;
        dmr_priv_wipe(&st->priv);
        return;
    }
    dmr_event_t ev;
    ev_init(&ev, DMR_EV_VOICE_END, slot, rx->last_cc, DMR_SYNC_NONE);
    ev.lc = lc ? *lc : st->lc;
    ev.crypt = st->enc ? (st->priv.keyed ? DMR_CRYPT_DECRYPTED : DMR_CRYPT_ENCRYPTED) : DMR_CRYPT_CLEAR;
    st->active = false;
    st->lc_valid = false;
    st->sf_index = 0xFF;
    st->emb_frags = 0;
    st->enc = false;
    dmr_priv_wipe(&st->priv);
    emit(rx, &ev);
}

/* cfw privacy parameters from a PI header or a privacy LC; sf = superframe they belong to */
static void privacy_info(dmr_rx_t *rx, uint8_t slot, uint8_t cc, const dmr_priv_info_t *pi, bool from_pi)
{
    dmr_slot_state_t *st = &rx->slot[slot];
    dmr_priv_info_t pf = *pi;
    /* the privacy LC has only the low 4 bits of the key id: keep the full one a PI header gave for this call */
    if (!from_pi && st->enc && st->pinfo.iv == pi->iv && st->pinfo.alg == pi->alg &&
        (st->pinfo.key_id & 0xFu) == (pi->key_id & 0xFu)) pf.key_id = st->pinfo.key_id;
    pi = &pf;
    bool fresh = !st->enc || st->pinfo.iv != pi->iv || st->pinfo.key_id != pi->key_id || st->pinfo.alg != pi->alg;
    st->enc = true;
    st->pinfo = *pi;
    if (fresh || !st->priv.keyed) {
        const dmr_key_t *k = from_pi || pi->key_id > 15 ? dmr_keyring_find(rx->keys, pi->alg, pi->key_id)
                                                         : dmr_keyring_find_lc(rx->keys, pi->alg, pi->key_id);
        if (dmr_priv_setup(&st->priv, k) < 0) dmr_priv_wipe(&st->priv);
    }
    /* a PI header precedes the first superframe (burst A increments); an embedded LC is inside it */
    if (st->priv.keyed) dmr_priv_sync(&st->priv, pi->iv, (uint16_t)(from_pi ? pi->sf - 1u : pi->sf));
    if (fresh) {
        dmr_event_t ev;
        ev_init(&ev, DMR_EV_PRIVACY, slot, cc, DMR_SYNC_NONE);
        ev.priv = *pi;
        ev.crypt = st->priv.keyed ? DMR_CRYPT_DECRYPTED : DMR_CRYPT_ENCRYPTED;
        emit(rx, &ev);
    }
}

static void deliver_voice(dmr_rx_t *rx, uint8_t slot, const uint8_t burst[DMR_BURST_DIBITS], uint8_t idx)
{
    dmr_slot_state_t *st = &rx->slot[slot];
    uint8_t fr[3][9], crypt = DMR_CRYPT_CLEAR;
    dmr_burst_voice(burst, fr);
    if (st->enc) {
        crypt = DMR_CRYPT_ENCRYPTED;
        if (st->priv.keyed && st->priv.synced) {
            if (idx == 0) st->priv.sf++;
            for (uint8_t f = 0; f < 3; f++) dmr_priv_voice(&st->priv, idx, f, fr[f]);
            crypt = DMR_CRYPT_DECRYPTED;
        }
    }
    if (rx->cb.voice)
        for (int f = 0; f < 3; f++) rx->cb.voice(rx->cb.user, slot, fr[f], idx, crypt);
}

/* a voice burst that is not delivered still advances the privacy superframe counter */
static void skip_voice(dmr_slot_state_t *st, uint8_t idx)
{
    if (idx == 0 && st->enc && st->priv.keyed && st->priv.synced) st->priv.sf++;
}

/* colour code of a burst B..F from its EMB: 1 = cc_filter, -1 = other, 0 = EMB not decodable */
static int emb_cc(const dmr_rx_t *rx, const uint8_t burst[DMR_BURST_DIBITS])
{
    uint8_t cc, pi, lcss, e32[32];
    if (dmr_burst_emb(burst, &cc, &pi, &lcss, e32) < 0) return 0;
    return cc == (uint8_t)rx->cfg.cc_filter ? 1 : -1;
}

/* ---------------- data blocks ---------------- */
static int block_bytes(uint8_t dt) { return dt == DMR_DT_RATE_34 ? 18 : dt == DMR_DT_RATE_1 ? 24 : 12; }

static int decode_block(uint8_t dt, const uint8_t bits196[196], uint8_t out[24])
{
    if (dt == DMR_DT_RATE_34) {
        uint8_t d[98];
        bits_to_dibits(d, bits196, 98);
        return trellis34_decode(d, out) < 0 ? -1 : 0;
    }
    if (dt == DMR_DT_RATE_1) {     /* table B.10B: I(191..96), 4 pad bits, I(95..0) */
        uint8_t b[192];
        memcpy(b, bits196, 96);
        memcpy(b + 96, bits196 + 100, 96);
        bits_pack(out, b, 192);
        return 0;
    }
    return bptc196_decode(bits196, out) < 0 ? -1 : 0;
}

static void data_finish(dmr_rx_t *rx, uint8_t slot, uint8_t cc)
{
    dmr_slot_state_t *st = &rx->slot[slot];
    dmr_event_t ev;
    ev_init(&ev, DMR_EV_DATA_PACKET, slot, cc, DMR_SYNC_NONE);
    ev.dh = st->dh;
    ev.data = st->data;
    ev.len = st->data_len;
    if (st->data_len >= 4 && (st->dh.dpf == DMR_DPF_UNCONFIRMED || st->dh.dpf == DMR_DPF_CONFIRMED)) {
        uint32_t c = crc32_dmr(st->data, st->data_len - 4u);
        const uint8_t *p = st->data + st->data_len - 4;
        /* CRC octets: least significant first (figure B.8C; confirmed with real captures) */
        uint32_t rxc = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        ev.crc_ok = c == rxc;
    }
    if (st->data_enc) {             /* cfw privacy: user data encrypted, CRC over the ciphertext */
        size_t n = st->data_len >= 4 ? st->data_len - 4u : st->data_len;
        if (st->priv.keyed) {
            dmr_priv_sync(&st->priv, st->pinfo.iv, 0);
            dmr_priv_data(&st->priv, 0, st->data, n);
            ev.crypt = DMR_CRYPT_DECRYPTED;
        } else {
            ev.crypt = DMR_CRYPT_ENCRYPTED;
        }
        ev.priv = st->pinfo;
        st->data_enc = false;
        if (!st->active) { st->enc = false; dmr_priv_wipe(&st->priv); }
    }
    st->blocks_left = 0;
    emit(rx, &ev);
}

static void data_block(dmr_rx_t *rx, uint8_t slot, uint8_t cc, uint8_t dt, const uint8_t bits196[196])
{
    dmr_slot_state_t *st = &rx->slot[slot];
    if (!st->blocks_left) return;
    uint8_t blk[24];
    int n = block_bytes(dt);
    int ok = decode_block(dt, bits196, blk) == 0;
    const uint8_t *user = blk;
    if (st->dh.dpf == DMR_DPF_CONFIRMED) {
        /* DBSN(7) + CRC-9 in the first two octets (9.2.2) */
        uint8_t dbsn = blk[0] >> 1;
        uint16_t crc = (uint16_t)(((blk[0] & 1u) << 8) | blk[1]);
        uint16_t mask = dt == DMR_DT_RATE_34 ? DMR_MASK_R34_CRC9 : dt == DMR_DT_RATE_1 ? DMR_MASK_R1_CRC9 : DMR_MASK_R12_CRC9;
        if ((crc ^ mask) != crc9_dmr(blk + 2, (size_t)n - 2u, dbsn)) ok = 0;
        user = blk + 2;
        n -= 2;
    }
    if (st->data_len + (unsigned)n <= sizeof st->data) {
        if (!ok) memset((uint8_t *)user, 0, (size_t)n);
        memcpy(st->data + st->data_len, user, (size_t)n);
        st->data_len = (uint16_t)(st->data_len + n);
    }
    if (--st->blocks_left == 0) data_finish(rx, slot, cc);
}

/* ---------------- burst dispatch ---------------- */
static void embedded(dmr_rx_t *rx, uint8_t slot, const uint8_t burst[DMR_BURST_DIBITS])
{
    dmr_slot_state_t *st = &rx->slot[slot];
    uint8_t cc, pi, lcss, e32[32];
    if (dmr_burst_emb(burst, &cc, &pi, &lcss, e32) < 0) { st->emb_frags = 0; return; }
    if (rx->cfg.cc_filter >= 0 && cc != rx->cfg.cc_filter) return;
    rx->last_cc = cc;
    switch (lcss) {
    case DMR_LCSS_FIRST:
        memcpy(st->emb_bits, e32, 32);
        st->emb_frags = 1;
        break;
    case DMR_LCSS_CONT:
        if (st->emb_frags >= 1 && st->emb_frags <= 2) memcpy(st->emb_bits + 32 * st->emb_frags++, e32, 32);
        else st->emb_frags = 0;
        break;
    case DMR_LCSS_LAST:
        if (st->emb_frags == 3) {
            uint8_t lc[9];
            memcpy(st->emb_bits + 96, e32, 32);
            if (emblc_decode(st->emb_bits, lc) == 0) {
                dmr_event_t ev;
                dmr_lc_t l;
                dmr_priv_info_t pi;
                if (dmr_priv_lc_parse(lc, &pi) == 0) {      /* late entry into an encrypted call */
                    privacy_info(rx, slot, cc, &pi, false);
                    st->emb_frags = 0;
                    break;
                }
                dmr_lc_parse(lc, &l);
                bool voice_lc = l.flco == DMR_FLCO_GROUP_VOICE || l.flco == DMR_FLCO_UNIT_VOICE;
                if (voice_lc && (l.svc_opts & 0x40u)) st->enc = true;
                if (!st->lc_valid && voice_lc) {
                    st->lc = l;
                    st->lc_valid = true;
                    ev_init(&ev, DMR_EV_VOICE_START, slot, cc, DMR_SYNC_NONE);
                } else {
                    ev_init(&ev, DMR_EV_VOICE_LC_EMB, slot, cc, DMR_SYNC_NONE);
                }
                ev.lc = l;
                emit(rx, &ev);
            }
        }
        st->emb_frags = 0;
        break;
    default:    /* single fragment: null message or RC (PI = 1) */
        if (pi) {
            uint16_t rc;
            if (sbbptc_decode(e32, 1, &rc) >= 0) {
                dmr_event_t ev;
                uint8_t cmd;
                ev_init(&ev, DMR_EV_RC, slot, cc, DMR_SYNC_NONE);
                ev.crc_ok = dmr_rc_check(rc, &cmd) == 0;
                ev.data = &cmd;
                ev.len = 1;
                emit(rx, &ev);
            }
        }
        break;
    }
}

static void data_burst(dmr_rx_t *rx, uint8_t slot, dmr_sync_t sync, const uint8_t burst[DMR_BURST_DIBITS])
{
    dmr_slot_state_t *st = &rx->slot[slot];
    uint8_t cc, dt, bits[196];
    if (dmr_burst_slot_type(burst, &cc, &dt) < 0) return;
    if (rx->cfg.cc_filter >= 0 && cc != rx->cfg.cc_filter) return;
    rx->last_cc = cc;
    dmr_burst_info_bits(burst, bits);
    dmr_event_t ev;
    switch (dt) {
    case DMR_DT_VOICE_LC_HEADER: {
        dmr_lc_t lc;
        if (dmr_fulllc_decode(bits, DMR_MASK_VOICE_HDR, &lc) < 0) return;
        if (st->active && st->lc_valid && memcmp(lc.raw, st->lc.raw, 9) == 0) return;   /* repeated header */
        st->active = true;
        st->lc_valid = true;
        st->lc = lc;
        if (lc.svc_opts & 0x40u) st->enc = true;     /* service options privacy bit (TS 102 361-2 7.2.1) */
        st->data_enc = false;                        /* the PI header belonged to this call */
        st->sf_index = 0xFF;
        st->emb_frags = 0;
        st->missed = 0;
        st->cc_state = 1;                            /* header passed the CC filter */
        st->a_held = false;
        ev_init(&ev, DMR_EV_VOICE_START, slot, cc, sync);
        ev.lc = lc;
        emit(rx, &ev);
        break;
    }
    case DMR_DT_TERMINATOR_LC: {
        dmr_lc_t lc;
        if (dmr_fulllc_decode(bits, DMR_MASK_TERMINATOR, &lc) < 0) { voice_end(rx, slot, NULL); return; }
        voice_end(rx, slot, &lc);
        break;
    }
    case DMR_DT_PI_HEADER: {
        uint8_t info[12];
        if (bptc196_decode(bits, info) < 0) return;
        ev_init(&ev, DMR_EV_PI_HEADER, slot, cc, sync);
        uint16_t crc = (uint16_t)(((info[10] << 8) | info[11]) ^ DMR_MASK_PI_HEADER);
        ev.crc_ok = crc == crc_ccitt_dmr(info, 10);
        ev.data = info;
        ev.len = 10;
        emit(rx, &ev);
        dmr_priv_info_t pi;
        if (ev.crc_ok && dmr_priv_pi_parse(info, &pi) == 0) {
            privacy_info(rx, slot, cc, &pi, true);
            st->data_enc = true;            /* applies to a following data packet too */
        }
        break;
    }
    case DMR_DT_CSBK:
        ev_init(&ev, DMR_EV_CSBK, slot, cc, sync);
        if (dmr_csbk_decode(bits, DMR_MASK_CSBK, &ev.csbk) < 0) return;
        emit(rx, &ev);
        break;
    case DMR_DT_MBC_HEADER:
        if (dmr_csbk_decode(bits, DMR_MASK_MBC_HEADER, &st->mbc) < 0) { st->mbc_left = 0; return; }
        st->mbc_left = 3;
        st->data_len = 0;
        st->blocks_left = 0;
        break;
    case DMR_DT_MBC_CONT: {
        uint8_t info[12];
        if (!st->mbc_left || bptc196_decode(bits, info) < 0) { st->mbc_left = 0; return; }
        if (st->data_len + 12u <= sizeof st->data) { memcpy(st->data + st->data_len, info, 12); st->data_len += 12; }
        st->mbc_left--;
        if ((info[0] & 0x80u) || !st->mbc_left) {      /* last block */
            ev_init(&ev, DMR_EV_MBC, slot, cc, sync);
            ev.csbk = st->mbc;
            ev.data = st->data;
            ev.len = st->data_len;
            /* last block CRC-CCITT over header data + continuation payload (no mask) */
            uint8_t tmp[10 + 3 * 12];
            unsigned n = 0;
            memcpy(tmp, st->mbc.raw, 10); n = 10;
            for (unsigned b = 0; b < st->data_len / 12u; b++) {
                unsigned take = (b + 1u == st->data_len / 12u) ? 10u : 12u;
                memcpy(tmp + n, st->data + 12 * b, take); n += take;
            }
            const uint8_t *lb = st->data + st->data_len - 12;
            ev.crc_ok = (uint16_t)((lb[10] << 8) | lb[11]) == crc_ccitt_dmr(tmp, n);
            st->mbc_left = 0;
            emit(rx, &ev);
        }
        break;
    }
    case DMR_DT_DATA_HEADER:
        ev_init(&ev, DMR_EV_DATA_HEADER, slot, cc, sync);
        if (dmr_data_header_decode(bits, &ev.dh) < 0) return;
        st->dh = ev.dh;
        st->data_len = 0;
        st->blocks_left = (ev.dh.dpf == DMR_DPF_UNCONFIRMED || ev.dh.dpf == DMR_DPF_CONFIRMED ||
                           ev.dh.dpf == DMR_DPF_RESPONSE) ? ev.dh.bf : 0;
        emit(rx, &ev);
        break;
    case DMR_DT_RATE_12:
    case DMR_DT_RATE_34:
    case DMR_DT_RATE_1:
        data_block(rx, slot, cc, dt, bits);
        break;
    case DMR_DT_USBD: {
        uint8_t info[12];
        if (bptc196_decode(bits, info) < 0) return;
        ev_init(&ev, DMR_EV_DATA_PACKET, slot, cc, sync);
        uint16_t crc = (uint16_t)(((info[10] << 8) | info[11]) ^ DMR_MASK_USBD);
        ev.crc_ok = crc == crc_ccitt_dmr(info, 10);
        ev.data = info;
        ev.len = 10;
        emit(rx, &ev);
        break;
    }
    case DMR_DT_IDLE:
        ev_init(&ev, DMR_EV_IDLE, slot, cc, sync);
        emit(rx, &ev);
        break;
    default:
        break;
    }
}

static void cach(dmr_rx_t *rx, const uint8_t cd[DMR_CACH_DIBITS], int *slot)
{
    dmr_cach_t c;
    if (dmr_cach_decode(cd, &c) < 0) { rx->slc_frags = 0; return; }
    *slot = c.tc;
    switch (c.lcss) {
    case DMR_LCSS_FIRST: memcpy(rx->slc_bits, c.payload, 17); rx->slc_frags = 1; break;
    case DMR_LCSS_CONT:
        if (rx->slc_frags >= 1 && rx->slc_frags <= 2) memcpy(rx->slc_bits + 17 * rx->slc_frags++, c.payload, 17);
        else rx->slc_frags = 0;
        break;
    case DMR_LCSS_LAST:
        if (rx->slc_frags == 3) {
            uint8_t slc[4];
            memcpy(rx->slc_bits + 51, c.payload, 17);
            if (cach_slc_decode(rx->slc_bits, slc) == 0) {
                dmr_event_t ev;
                ev_init(&ev, DMR_EV_SHORT_LC, (uint8_t)c.tc, rx->last_cc, DMR_SYNC_NONE);
                ev.slc.slco = slc[0] & 0xFu;
                ev.slc.data24 = ((uint32_t)slc[1] << 16) | ((uint32_t)slc[2] << 8) | slc[3];
                emit(rx, &ev);
            }
        }
        rx->slc_frags = 0;
        break;
    default: rx->slc_frags = 0; break;
    }
}

void dmr_rx_burst(dmr_rx_t *rx, const uint8_t cd[DMR_CACH_DIBITS], const uint8_t burst[DMR_BURST_DIBITS],
                  dmr_sync_t sync, int slot_hint)
{
    int slot = slot_hint;
    if (dmr_sync_is_bs(sync)) rx->bs_stream = true;
    else if (sync != DMR_SYNC_NONE) rx->bs_stream = false;
    /* only BS-sourced bursts have a CACH in front; direct / MS bursts have a guard time there */
    if (cd && rx->cfg.bs_cach && rx->bs_stream) cach(rx, cd, &slot);
    /* direct mode / MS-sourced: no slot numbering on air; the bursts with sync are the forward path */
    if ((sync == DMR_SYNC_MS_VOICE || sync == DMR_SYNC_MS_DATA) && !rx->bs_stream) slot = 0;
    if (sync == DMR_SYNC_MS_RC && !rx->bs_stream) slot = 1;
    if (sync == DMR_SYNC_TS1_VOICE || sync == DMR_SYNC_TS1_DATA) slot = 0;
    if (sync == DMR_SYNC_TS2_VOICE || sync == DMR_SYNC_TS2_DATA) slot = 1;
    if (slot < 0 || slot > 1) slot = 0;
    rx->cur_slot = (uint8_t)slot;
    dmr_slot_state_t *st = &rx->slot[slot];

    if (dmr_sync_is_voice(sync)) {
        if (!st->active) {          /* voice without header: late entry */
            st->active = true;
            st->lc_valid = false;
            st->emb_frags = 0;
            st->cc_state = 0;
            st->a_held = false;
        }
        st->sf_index = 0;
        st->missed = 0;
        if (st->a_held) skip_voice(st, 0);           /* burst B of the held A never came */
        st->a_held = false;
        /* burst A has no EMB: deliver it only if the CC is already known good, else hold it for B */
        if (rx->cfg.cc_filter < 0 || st->cc_state > 0) deliver_voice(rx, (uint8_t)slot, burst, 0);
        else { memcpy(st->a_burst, burst, DMR_BURST_DIBITS); st->a_held = true; }
        return;
    }
    if (dmr_sync_is_data(sync)) {
        data_burst(rx, (uint8_t)slot, sync, burst);
        if (st->active && ++st->missed > 6) voice_end(rx, (uint8_t)slot, NULL);
        return;
    }
    if (sync == DMR_SYNC_MS_RC) {          /* standalone RC burst (table E.12) in the backward slot */
        uint8_t b[264], emb_ok, cc = 0, pi = 0, lcss = 0, r32[32];
        dibits_to_bits(b, burst, DMR_BURST_DIBITS);
        const uint8_t *rb = b + 2 * DMR_RC_BURST_OFFSET;
        uint16_t qr = (uint16_t)((bits_arr_val(rb, 8) << 8) | bits_arr_val(rb + 88, 8));
        int e, d = fec_qr1676_dec(qr, &e);
        emb_ok = d >= 0;
        if (emb_ok) { cc = (uint8_t)(d >> 3); pi = (uint8_t)((d >> 2) & 1); lcss = (uint8_t)(d & 3); }
        (void)pi; (void)lcss;
        if (emb_ok && rx->cfg.cc_filter >= 0 && cc != rx->cfg.cc_filter) return;
        memcpy(r32, rb + 8, 16);
        memcpy(r32 + 16, rb + 72, 16);
        uint16_t info;
        if (sbbptc_decode(r32, 1, &info) < 0) return;
        uint8_t cmd;
        dmr_event_t ev;
        ev_init(&ev, DMR_EV_RC, (uint8_t)slot, cc, sync);
        ev.crc_ok = dmr_rc_check(info, &cmd) == 0;
        ev.data = &cmd;
        ev.len = 1;
        emit(rx, &ev);
        return;
    }
    if (sync != DMR_SYNC_NONE) return;     /* reserved */
    if (st->active && st->sf_index < 5) {
        st->sf_index++;
        st->missed = 0;
        if (rx->cfg.cc_filter < 0) {
            deliver_voice(rx, (uint8_t)slot, burst, st->sf_index);
        } else {
            int c = emb_cc(rx, burst);
            if (c) st->cc_state = (int8_t)c;     /* undecodable EMB: keep the last verdict */
            if (st->a_held) {
                if (st->sf_index == 1 && st->cc_state > 0) deliver_voice(rx, (uint8_t)slot, st->a_burst, 0);
                else skip_voice(st, 0);
                st->a_held = false;
            }
            if (st->cc_state > 0) deliver_voice(rx, (uint8_t)slot, burst, st->sf_index);
        }
        embedded(rx, (uint8_t)slot, burst);
    } else {
        if (st->active && ++st->missed > 6) voice_end(rx, (uint8_t)slot, NULL);
        else if (!st->active && rx->bs_stream) {
            /* outbound embedded RC (6.4.2) in a voice burst of the other logical channel */
            uint8_t cc, pi, lcss, e32[32];
            if (dmr_burst_emb(burst, &cc, &pi, &lcss, e32) == 0 && pi && lcss == DMR_LCSS_SINGLE)
                embedded(rx, (uint8_t)slot, burst);
        }
    }
}

static void process_burst(dmr_rx_t *rx)
{
    uint8_t cd[DMR_CACH_DIBITS], b[DMR_BURST_DIBITS];
    uint32_t end = rx->pos;     /* exclusive */
    for (int i = 0; i < DMR_BURST_DIBITS; i++) b[i] = rx->ring[(end - DMR_BURST_DIBITS + (uint32_t)i) & 0xFFu];
    for (int i = 0; i < DMR_CACH_DIBITS; i++) cd[i] = rx->ring[(end - DMR_SLOT_DIBITS + (uint32_t)i) & 0xFFu];
    dmr_sync_t s = rx->pending_sync;
    rx->pending_sync = DMR_SYNC_NONE;
    if (s == DMR_SYNC_NONE) {
        if (++rx->unsynced > SYNC_LOSS_BURSTS) {
            rx->locked = false;
            for (uint8_t sl = 0; sl < 2; sl++) voice_end(rx, sl, NULL);
            dmr_event_t ev;
            ev_init(&ev, DMR_EV_SYNC_LOST, 0, rx->last_cc, DMR_SYNC_NONE);
            emit(rx, &ev);
            return;
        }
    } else {
        rx->unsynced = 0;
    }
    int hint = rx->cur_slot ^ 1;
    dmr_rx_burst(rx, rx->cfg.bs_cach ? cd : NULL, b, s, hint);
}

void dmr_rx_dibit(dmr_rx_t *rx, uint8_t d)
{
    d &= 3u;
    rx->ring[rx->pos & 0xFFu] = d;
    rx->pos++;
    rx->shift = ((rx->shift << 2) | d) & 0xFFFFFFFFFFFFull;
    if (rx->pos >= 24) {
        int err;
        dmr_sync_t s = dmr_sync_match(rx->shift, rx->cfg.sync_max_err, &err);
        if (s != DMR_SYNC_NONE && s != DMR_SYNC_RESERVED) {
            /* this dibit is R12; the burst ends 54 dibits later */
            uint32_t end = rx->pos + 54u;
            if (!rx->locked || end != rx->next_end) {
                /* new or corrected timing: a burst predicted by the old timing is dropped */
                rx->next_end = end;
                rx->locked = true;
            }
            rx->pending_sync = s;
        }
    }
    if (rx->locked && rx->pos == rx->next_end) {
        process_burst(rx);
        rx->next_end += DMR_SLOT_DIBITS;
    }
}

void dmr_rx_dibits(dmr_rx_t *rx, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) dmr_rx_dibit(rx, d[i]);
}

bool dmr_rx_next_slot_start(const dmr_rx_t *rx, uint8_t slot, uint32_t *dibit_index)
{
    if (!rx->locked || !rx->bs_stream) return false;
    /* next_end = end of the next burst; it belongs to the slot after the one processed last */
    uint32_t start = rx->next_end - DMR_BURST_DIBITS;
    uint8_t next_slot = (uint8_t)(rx->cur_slot ^ 1u);
    if (next_slot != (slot & 1u)) start += DMR_SLOT_DIBITS;
    while ((int32_t)(start - rx->pos) < 0) start += 2u * DMR_SLOT_DIBITS;
    *dibit_index = start;
    return true;
}
