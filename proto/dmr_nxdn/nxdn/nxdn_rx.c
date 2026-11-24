/* NXDN receiver: FSW framing with flywheel, descrambling, LICH demux, SACCH superframes,
 * FACCH1 stealing, UDCH/FACCH2, CAC (TS 1-A 4.4, 5.2, 5.3, 6.3), Type-D SCCH (TS 1-E). */
#include "nxdn.h"
#include "../common/bits.h"
#include <string.h>

#define FSW_LOSS_FRAMES 3

void nxdn_rx_init(nxdn_rx_t *rx, const nxdn_rx_cfg_t *cfg, const nxdn_cb_t *cb)
{
    memset(rx, 0, sizeof *rx);
    if (cfg) rx->cfg = *cfg;
    else { rx->cfg.fsw_max_err = 2; rx->cfg.ran_filter = -1; }
    if (cb) rx->cb = *cb;
}

static void emit(nxdn_rx_t *rx, nxdn_event_t *ev)
{
    if (rx->cb.event) rx->cb.event(rx->cb.user, ev);
}

static void ev0(nxdn_event_t *ev, nxdn_ev_type_t t, const nxdn_lich_t *l)
{
    memset(ev, 0, sizeof *ev);
    ev->type = t;
    if (l) ev->lich = *l;
}

static void call_end(nxdn_rx_t *rx, const nxdn_lich_t *l)
{
    if (!rx->in_call) return;
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_VOICE_END, l);
    ev.call = rx->call;
    rx->in_call = false;
    rx->sacch_mask = 0;               /* last_facch kept: the TX_REL repeat in the second half is a duplicate */
    emit(rx, &ev);
}

static uint8_t crypt_setup(nxdn_rx_t *rx, const nxdn_lich_t *l, uint8_t cipher, uint8_t key_id, nxdn_ch_t ch)
{
    if (cipher == NXDN_CIPHER_NONE) return NXDN_CRYPT_CLEAR;
    const nxdn_key_t *k = nxdn_keyring_find(rx->keys, cipher, key_id);
    uint8_t st = k ? NXDN_CRYPT_DECRYPTED : NXDN_CRYPT_ENCRYPTED;
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_CRYPT, l);
    ev.ch = ch;
    ev.crypt = st;
    ev.call.cipher = cipher;
    ev.call.key_id = key_id;
    emit(rx, &ev);
    return st;
}

static void call_msg(nxdn_rx_t *rx, const nxdn_lich_t *l, const uint8_t *m)
{
    uint8_t t = m[0] & 0x3Fu;
    if (t == NXDN_MSG_VCALL && (!rx->in_call || (rx->call.type != NXDN_MSG_VCALL))) {
        nxdn_event_t ev;
        ev0(&ev, NXDN_EV_VOICE_START, l);
        nxdn_msg_parse_call(m, &ev.call);
        bool was = rx->in_call;
        rx->call = ev.call;
        rx->in_call = true;
        const nxdn_key_t *k = nxdn_keyring_find(rx->keys, ev.call.cipher, ev.call.key_id);
        nxdn_vcrypt_setup(&rx->vc, ev.call.cipher, ev.call.key_id, k, rx->cfg.efr);
        ev.crypt = crypt_setup(rx, l, ev.call.cipher, ev.call.key_id, NXDN_CH_FACCH1);
        if (!was) emit(rx, &ev);
    } else if (t == NXDN_MSG_VCALL_IV) {
        nxdn_vcrypt_iv(&rx->vc, nxdn_msg_iv(m));
    } else if (t == NXDN_MSG_TX_REL || t == NXDN_MSG_TX_REL_EX || t == NXDN_MSG_DISC) {
        call_end(rx, l);
    }
}

static void crc_err(nxdn_rx_t *rx, const nxdn_lich_t *l, nxdn_ch_t ch)
{
    if (!rx->confirmed && !rx->in_call) return;     /* most likely a false FSW in noise */
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_CRC_ERROR, l);
    ev.ch = ch;
    emit(rx, &ev);
}

static bool ran_ok(const nxdn_rx_t *rx, uint8_t sr)
{
    uint8_t ran = sr & 0x3Fu;
    return rx->cfg.ran_filter < 0 || ran == 0 || ran == (uint8_t)rx->cfg.ran_filter;
}

static uint8_t sacch(nxdn_rx_t *rx, const nxdn_lich_t *l, const uint8_t *bits, bool superframe)
{
    uint8_t info[4];
    if (nxdn_ch_decode(NXDN_CH_SACCH, bits + 36, info, NULL) < 0) { crc_err(rx, l, NXDN_CH_SACCH); return NXDN_VPOS_UNKNOWN; }
    uint8_t sr = info[0];
    if (!ran_ok(rx, sr)) { rx->ran_bad = true; return NXDN_VPOS_UNKNOWN; }
    rx->ran_bad = false;
    rx->ctl_ok = true;
    uint8_t part[3] = { 0, 0, 0 };
    bits_set_field(part, 0, 18, bits_field(info, 8, 18));
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_SACCH, l);
    ev.ch = NXDN_CH_SACCH;
    ev.sr = sr;
    ev.data = part;
    ev.len = 3;
    emit(rx, &ev);
    if (!superframe) {
        ev.type = NXDN_EV_SACCH_MSG;
        emit(rx, &ev);
        return NXDN_VPOS_UNKNOWN;
    }
    unsigned q = 3u - (sr >> 6);              /* structure 11 = 1/4 (head) .. 00 = 4/4 */
    if (q == 0) { memset(rx->sacch, 0, sizeof rx->sacch); rx->sacch_mask = 0; }
    bits_set_field(rx->sacch, q * 18u, 18, bits_field(info, 8, 18));
    rx->sacch_mask |= (uint8_t)(1u << q);
    if (q == 3) {
        if (rx->sacch_mask == 0xF) {
            ev.type = NXDN_EV_SACCH_MSG;
            ev.data = rx->sacch;
            ev.len = 9;
            emit(rx, &ev);
            call_msg(rx, l, rx->sacch);
        }
        rx->sacch_mask = 0;
    }
    return (uint8_t)q;
}

static void facch1(nxdn_rx_t *rx, const nxdn_lich_t *l, const uint8_t *bits)
{
    uint8_t m[10];
    if (nxdn_ch_decode(NXDN_CH_FACCH1, bits, m, NULL) < 0) { crc_err(rx, l, NXDN_CH_FACCH1); return; }
    if (rx->last_facch_valid && !memcmp(m, rx->last_facch, 10)) return;   /* same message in both halves */
    memcpy(rx->last_facch, m, 10);
    rx->last_facch_valid = true;
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_FACCH1, l);
    ev.ch = NXDN_CH_FACCH1;
    ev.data = m;
    ev.len = 10;
    emit(rx, &ev);
    call_msg(rx, l, m);
}

static void voice_half(nxdn_rx_t *rx, const uint8_t *bits, int h, uint8_t q)
{
    uint8_t f[18];
    if (!rx->cb.voice) return;
    bool enc = rx->in_call && rx->call.type != 0xFF && rx->call.cipher != NXDN_CIPHER_NONE;
    if (rx->cfg.efr) {
        uint8_t pos = q == NXDN_VPOS_UNKNOWN ? q : (uint8_t)((q << 2) | (unsigned)h), crypt = NXDN_CRYPT_CLEAR;
        bits_pack(f, bits, 144);
        if (enc) crypt = nxdn_vcrypt_apply144(&rx->vc, pos, f) ? NXDN_CRYPT_DECRYPTED : NXDN_CRYPT_ENCRYPTED;
        rx->cb.voice(rx->cb.user, f, 144, pos, crypt);
    } else {
        for (int k = 0; k < 2; k++) {
            uint8_t pos = q == NXDN_VPOS_UNKNOWN ? q : (uint8_t)((q << 2) | (unsigned)(2 * h + k));
            uint8_t crypt = NXDN_CRYPT_CLEAR;
            bits_pack(f, bits + 72 * k, 72);
            if (enc) crypt = nxdn_vcrypt_apply72(&rx->vc, pos, f) ? NXDN_CRYPT_DECRYPTED : NXDN_CRYPT_ENCRYPTED;
            rx->cb.voice(rx->cb.user, f, 72, pos, crypt);
        }
    }
}

static void traffic(nxdn_rx_t *rx, const nxdn_lich_t *l, const uint8_t *bits, bool type_d)
{
    nxdn_event_t ev;
    bool voice_frame, superframe;
    uint8_t steal = l->option, q = NXDN_VPOS_UNKNOWN;
    if (type_d) {
        uint8_t info[4];
        if (nxdn_ch_decode(NXDN_CH_SCCH, bits + 36, info, NULL) == 0) {
            rx->ctl_ok = true;
            ev0(&ev, NXDN_EV_SCCH, l);
            ev.ch = NXDN_CH_SCCH;
            ev.data = info;
            ev.len = 4;
            ev.sr = info[0] >> 6;
            emit(rx, &ev);
        } else {
            crc_err(rx, l, NXDN_CH_SCCH);
        }
        if (l->fct == 1) return;                       /* UDCH2 / FACCH3: not decoded here */
        voice_frame = l->fct == 2;
        superframe = true;
        if (l->fct == 3) return;                       /* idle */
        if (l->fct == 0 && steal == 1) {               /* FACCH1 first half, G/N/post second */
            facch1(rx, l, bits + 96);
            return;
        }
    } else {
        if (l->fct == NXDN_USC_UDCH) {
            uint8_t info[23];
            nxdn_ch_t ch = NXDN_CH_UDCH;
            if (nxdn_ch_decode(ch, bits + 36, info, NULL) < 0) { crc_err(rx, l, ch); return; }
            ev0(&ev, steal == NXDN_STEAL_NONE ? NXDN_EV_UDCH : NXDN_EV_FACCH2, l);
            ev.ch = ch;
            if (steal == NXDN_STEAL_NONE) {        /* data call security (TS 1-D 5.4.5) */
                uint8_t *m = info + 1, t = m[0] & 0x3Fu;
                if (t == NXDN_MSG_DCALL_HDR) {
                    uint8_t cipher = m[7] >> 6, key_id = m[7] & 0x3Fu;
                    rx->dcrypt = crypt_setup(rx, l, cipher, key_id, ch);
                    if (rx->dcrypt == NXDN_CRYPT_DECRYPTED) {
                        uint64_t iv = 0;
                        for (int i = 0; i < 8; i++) iv = (iv << 8) | m[11 + i];
                        if (nxdn_ks_init(&rx->dks, nxdn_keyring_find(rx->keys, cipher, key_id)) < 0) rx->dcrypt = NXDN_CRYPT_ENCRYPTED;
                        else nxdn_dcrypt_start(&rx->dks, iv);
                    }
                } else if (t == NXDN_MSG_DCALL_DATA && rx->dcrypt != NXDN_CRYPT_CLEAR) {
                    if (rx->dcrypt == NXDN_CRYPT_DECRYPTED) nxdn_dcrypt_block(&rx->dks, m + 2, 20);
                    ev.crypt = rx->dcrypt;
                }
            }
            ev.sr = info[0];
            ev.data = info + 1;
            ev.len = 22;
            emit(rx, &ev);
            if (steal != NXDN_STEAL_NONE) call_msg(rx, l, info + 1);
            return;
        }
        superframe = l->fct != NXDN_USC_SACCH_NS;
        q = sacch(rx, l, bits, superframe);
        if (l->fct == NXDN_USC_SACCH_SS_IDLE) return;
        voice_frame = true;
    }
    if (!voice_frame) steal = NXDN_STEAL_BOTH;
    if (q != NXDN_VPOS_UNKNOWN) nxdn_vcrypt_frame(&rx->vc, q);
    for (int h = 0; h < 2; h++) {
        bool stolen = steal == NXDN_STEAL_BOTH || (h == 0 && steal == NXDN_STEAL_FIRST) ||
                      (h == 1 && steal == NXDN_STEAL_SECOND);
        const uint8_t *hb = bits + 96 + 144 * h;
        if (stolen) facch1(rx, l, hb);
        else {
            if (rx->ran_bad) continue;                    /* other RAN (6.3.2): not for us */
            if (!rx->ctl_ok && (!rx->in_call || !rx->fsw_ok || !rx->confirmed)) continue;  /* late entry / flywheel frame: need a good SACCH/SCCH */
            if (!rx->in_call) {           /* voice without a decoded VCALL yet: late entry */
                ev0(&ev, NXDN_EV_VOICE_START, l);
                memset(&rx->call, 0, sizeof rx->call);
                rx->call.type = 0xFF;          /* IDs / cipher follow in SACCH */
                nxdn_vcrypt_setup(&rx->vc, NXDN_CIPHER_NONE, 0, NULL, rx->cfg.efr);
                rx->in_call = true;
                emit(rx, &ev);
            }
            voice_half(rx, hb, h, q);
        }
    }
}

void nxdn_rx_frame(nxdn_rx_t *rx, const uint8_t frame[NXDN_FRAME_DIBITS])
{
    uint8_t d[NXDN_FRAME_DIBITS], bits[384], v7;
    memcpy(d, frame, sizeof d);
    nxdn_scramble(d, 10, NXDN_FRAME_DIBITS);
    if (nxdn_lich_decode(d + 10, &v7) < 0) return;
    nxdn_lich_t l;
    nxdn_lich_unpack(v7, &l);
    if (rx->fsw_ok) rx->last_rf = l.rf;
    else if (l.rf != rx->last_rf) return;           /* flywheel frame: the channel type cannot change */
    rx->ctl_ok = false;
    dibits_to_bits(bits, d, NXDN_FRAME_DIBITS);
    nxdn_event_t ev;
    ev0(&ev, NXDN_EV_LICH, &l);
    emit(rx, &ev);
    if (l.rf == NXDN_RF_RCCH) {
        uint8_t info[19];
        nxdn_ch_t ch = l.dir ? NXDN_CH_CAC : (l.fct == NXDN_CAC_SHORT ? NXDN_CH_SCAC : NXDN_CH_LCAC);
        if (nxdn_ch_decode(ch, bits + 36, info, NULL) < 0) { crc_err(rx, &l, ch); return; }
        ev0(&ev, NXDN_EV_CAC, &l);
        ev.ch = ch;
        ev.sr = info[0];
        ev.data = info + 1;
        ev.len = (uint16_t)(nxdn_ch_info_bits(ch) / 8u - 1u);
        emit(rx, &ev);
        return;
    }
    traffic(rx, &l, bits, rx->cfg.type_d && l.rf == NXDN_RF_RTCH_C);
}

static void process(nxdn_rx_t *rx)
{
    uint8_t f[NXDN_FRAME_DIBITS];
    for (unsigned i = 0; i < NXDN_FRAME_DIBITS; i++) f[i] = rx->ring[(rx->pos - NXDN_FRAME_DIBITS + i) & 0xFFu];
    nxdn_rx_frame(rx, f);
}

void nxdn_rx_dibit(nxdn_rx_t *rx, uint8_t dbt)
{
    dbt &= 3u;
    rx->ring[rx->pos & 0xFFu] = dbt;
    rx->pos++;
    rx->shift = ((rx->shift << 2) | dbt) & 0xFFFFFu;
    int err = popcount32(rx->shift ^ NXDN_FSW);
    if (rx->locked && rx->pos == rx->frame_end - (NXDN_FRAME_DIBITS - 10u)) {
        rx->fsw_ok = err <= rx->cfg.fsw_max_err + 2;
        if (rx->fsw_ok) { rx->missed = 0; rx->confirmed = true; }
        else if (++rx->missed > FSW_LOSS_FRAMES) {
            rx->locked = false;
            nxdn_event_t ev;
            ev0(&ev, NXDN_EV_SYNC_LOST, NULL);
            call_end(rx, NULL);
            emit(rx, &ev);
        }
    } else if (rx->pos >= 10 && err <= rx->cfg.fsw_max_err && (!rx->locked || rx->missed || !rx->confirmed)) {
        /* (re)acquire: also when the current timing came from a single, unconfirmed FSW (noise) */
        bool was = rx->locked;
        /* the replaced timing may be the right one (a near-FSW inside a real frame, 20 bits with <= 2 errors, is
         * not rare): its frame is still decoded at its end; LICH + CRCs reject it if it was noise */
        rx->alt_end = was && !rx->missed && !rx->confirmed && rx->frame_end > rx->pos ? rx->frame_end : 0;
        rx->locked = true;
        rx->missed = 0;
        rx->frame_end = rx->pos + NXDN_FRAME_DIBITS - 10u;
        rx->fsw_ok = true;
        rx->confirmed = false;
        if (!was) {
            nxdn_event_t ev;
            ev0(&ev, NXDN_EV_SYNC, NULL);
            emit(rx, &ev);
        }
    }
    if (rx->alt_end && rx->pos == rx->alt_end) {
        rx->alt_end = 0;
        bool f = rx->fsw_ok, c = rx->confirmed;
        rx->fsw_ok = true;
        rx->confirmed = false;
        process(rx);
        rx->fsw_ok = f;
        rx->confirmed = c;
    }
    if (rx->locked && rx->pos == rx->frame_end) {
        /* flywheel: a frame without its FSW is only used to keep a running call going */
        if (rx->fsw_ok || rx->in_call) process(rx);
        rx->frame_end += NXDN_FRAME_DIBITS;
    }
}

void nxdn_rx_dibits(nxdn_rx_t *rx, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) nxdn_rx_dibit(rx, d[i]);
}
