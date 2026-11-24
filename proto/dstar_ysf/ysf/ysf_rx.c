/*
 * ysf_rx.c - YSF receiver: 40-bit sync search, flywheel on the 960-bit frame
 * period, FICH decode, payload split per FI/DT, callsign/data assembly.
 *
 * Out of sync after max_miss (spec N2 = 4) consecutive bad syncs. A FICH
 * that fails its CRC is replaced by the previous one with FN advanced.
 */
#include <string.h>

#include "../common/dv_common.h"
#include "ysf.h"

#define SYNC_MASK ((1ull << 40) - 1u)

static void emit(ysf_rx_t *rx, ysf_event_t *ev)
{
    if (rx->cb && rx->cb->event)
        rx->cb->event(rx->user, ev);
}

void ysf_rx_init(ysf_rx_t *rx, const ysf_rx_cb_t *cb, void *user)
{
    memset(rx, 0, sizeof(*rx));
    rx->cb = cb;
    rx->user = user;
    rx->max_sync_err = 3;
    rx->max_flywheel_err = 8;
    rx->max_miss = 4;
    ysf_rx_reset(rx);
}

void ysf_rx_reset(ysf_rx_t *rx)
{
    rx->state = YSF_RX_SEARCH;
    rx->n = 0;
    rx->miss = 0;
    rx->have_last = false;
    rx->data_len = 0;
    memset(&rx->csd, 0, sizeof(rx->csd));
}

static void ev_base(ysf_event_t *ev, ysf_event_type_t t, const ysf_fich_t *f, bool ok, int errs)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = t;
    ev->fich = *f;
    ev->fich_ok = ok;
    ev->fich_errs = errs;
}

static void emit_dch(ysf_rx_t *rx, const ysf_fich_t *f, const uint8_t *d, unsigned len, unsigned unit, bool ok)
{
    ysf_event_t ev;
    ev_base(&ev, YSF_EV_DCH, f, true, 0);
    ev.dch.data = d;
    ev.dch.len = len;
    ev.dch.unit = unit;
    ev.dch.crc_ok = ok;
    ev.csd = rx->csd;
    emit(rx, &ev);
}

static void emit_csd(ysf_rx_t *rx, const ysf_fich_t *f)
{
    ysf_event_t ev;
    ev_base(&ev, YSF_EV_CSD, f, true, 0);
    ev.csd = rx->csd;
    emit(rx, &ev);
}

static void emit_data(ysf_rx_t *rx, const ysf_fich_t *f)
{
    ysf_event_t ev;
    if (!rx->data_len)
        return;
    ev_base(&ev, YSF_EV_DATA, f, true, 0);
    ev.dch.data = rx->data;
    ev.dch.len = rx->data_len;
    ev.dch.crc_ok = true;
    ev.csd = rx->csd;
    emit(rx, &ev);
    rx->data_len = 0;
}

static void put_data(ysf_rx_t *rx, unsigned off, const uint8_t *d, unsigned n)
{
    if (off >= YSF_DATA_MAX)
        return;
    if (off + n > YSF_DATA_MAX)
        n = YSF_DATA_MAX - off;
    memcpy(rx->data + off, d, n);
    if (off + n > rx->data_len)
        rx->data_len = off + n;
}

/* 20-byte unit with index u in the CSD1, CSD2, CSD3, DT1.. sequence */
static void unit20(ysf_rx_t *rx, const ysf_fich_t *f, unsigned u, const uint8_t d[20])
{
    switch (u) {
    case 0:
        memcpy(rx->csd.dest, d, 10);
        memcpy(rx->csd.src, d + 10, 10);
        rx->csd.valid |= 0x03;
        emit_csd(rx, f);
        break;
    case 1:
        memcpy(rx->csd.down, d, 10);
        memcpy(rx->csd.up, d + 10, 10);
        rx->csd.valid |= 0x0C;
        emit_csd(rx, f);
        break;
    case 2:
        memcpy(rx->csd.rem, d, 20);
        rx->csd.valid |= 0x30;
        emit_csd(rx, f);
        break;
    default:
        put_data(rx, (u - 3u) * 20u, d, 20);
        break;
    }
}

/* V/D mode 2: 10-byte units by FN */
static void unit10(ysf_rx_t *rx, const ysf_fich_t *f, unsigned fn, const uint8_t d[10])
{
    switch (fn) {
    case 0: memcpy(rx->csd.dest, d, 10); rx->csd.valid |= 0x01; break;
    case 1: memcpy(rx->csd.src, d, 10); rx->csd.valid |= 0x02; break;
    case 2: memcpy(rx->csd.down, d, 10); rx->csd.valid |= 0x04; break;
    case 3: memcpy(rx->csd.up, d, 10); rx->csd.valid |= 0x08; break;
    case 4: memcpy(rx->csd.rem[0], d, 10); rx->csd.valid |= 0x10; break;
    case 5: memcpy(rx->csd.rem[2], d, 10); rx->csd.valid |= 0x20; break;
    default: put_data(rx, (fn - 6u) * 10u, d, 10); return;
    }
    emit_csd(rx, f);
}

static void voice_out(ysf_rx_t *rx, ysf_voice_kind_t k, const uint8_t *fr, unsigned idx, const ysf_fich_t *f)
{
    if (rx->cb && rx->cb->voice)
        rx->cb->voice(rx->user, k, fr, idx, f);
}

void ysf_rx_frame(ysf_rx_t *rx, const uint8_t dibits[YSF_FRAME_DIBITS])
{
    uint8_t bits[YSF_FRAME_BITS];
    uint8_t u[360];
    uint8_t d[20];
    ysf_fich_t f;
    int errs = 0;
    ysf_event_t ev;

    ysf_dibits_to_bits(dibits, YSF_FRAME_DIBITS, bits);
    bool ok = ysf_fich_decode(bits + 40, &f, &errs);
    if (ok) {
        rx->last = f;
        rx->have_last = true;
    } else if (rx->have_last && rx->last.fi == YSF_FI_COMM) {
        f = rx->last;
        f.fn = (uint8_t)(f.ft ? (f.fn + 1u) % (f.ft + 1u) : 0u);
        rx->last = f;
    } else {
        ev_base(&ev, YSF_EV_FICH, &f, false, errs);
        emit(rx, &ev);
        return;
    }
    ev_base(&ev, YSF_EV_FICH, &f, ok, errs);
    emit(rx, &ev);

    const uint8_t *p = bits + 240;

    if (f.fi == YSF_FI_HEADER || f.fi == YSF_FI_TERM) {
        bool ok1, ok2;
        uint8_t d2[20];
        for (int i = 0; i < 5; i++)
            memcpy(u + 72 * i, p + 144 * i, 72);
        ok1 = ysf_dch_decode(u, 20, d, NULL);
        for (int i = 0; i < 5; i++)
            memcpy(u + 72 * i, p + 144 * i + 72, 72);
        ok2 = ysf_dch_decode(u, 20, d2, NULL);
        emit_dch(rx, &f, d, 20, 0, ok1);
        emit_dch(rx, &f, d2, 20, 1, ok2);
        if (ok1) {
            memcpy(rx->csd.dest, d, 10);
            memcpy(rx->csd.src, d + 10, 10);
            rx->csd.valid |= 0x03;
        }
        if (ok2) {
            memcpy(rx->csd.down, d2, 10);
            memcpy(rx->csd.up, d2 + 10, 10);
            rx->csd.valid |= 0x0C;
        }
        ev_base(&ev, f.fi == YSF_FI_HEADER ? YSF_EV_HEADER : YSF_EV_TERMINATOR, &f, ok, errs);
        ev.csd = rx->csd;
        ev.dch.crc_ok = ok1 && ok2;
        emit(rx, &ev);
        if (f.fi == YSF_FI_HEADER)
            rx->data_len = 0;
        return;
    }
    if (f.fi != YSF_FI_COMM)
        return; /* test channel: nothing defined */

    switch (f.dt) {
    case YSF_DT_VD1: {
        for (int i = 0; i < 5; i++)
            memcpy(u + 72 * i, p + 144 * i, 72);
        bool dok = ysf_dch_decode(u, 20, d, NULL);
        emit_dch(rx, &f, d, 20, 0, dok);
        if (f.fn == 0)
            rx->data_len = 0;
        if (dok)
            unit20(rx, &f, f.fn, d);
        for (unsigned i = 0; i < 5; i++) {
            uint8_t v[9];
            dv_pack_msb(p + 144 * i + 72, 72, v);
            voice_out(rx, YSF_VOICE_VD1, v, i, &f);
        }
        if (f.fn == f.ft && f.ft >= 3)
            emit_data(rx, &f);
        break;
    }
    case YSF_DT_VD2: {
        uint8_t v104[104];
        for (int i = 0; i < 5; i++)
            memcpy(u + 40 * i, p + 144 * i, 40);
        bool dok = ysf_dch_decode(u, 10, d, NULL);
        emit_dch(rx, &f, d, 10, 0, dok);
        if (f.fn == 0)
            rx->data_len = 0;
        if (dok)
            unit10(rx, &f, f.fn, d);
        for (unsigned i = 0; i < 5; i++) {
            uint8_t v[7];
            memcpy(v104, p + 144 * i + 40, 72);
            memcpy(v104 + 72, p + 144 * i + 112, 32);
            ysf_vd2_voice_decode(v104, v);
            voice_out(rx, YSF_VOICE_VD2, v, i, &f);
        }
        if (f.fn == f.ft && f.ft >= 6)
            emit_data(rx, &f);
        break;
    }
    case YSF_DT_DATA_FR: {
        if (f.fn == 0)
            rx->data_len = 0;
        for (unsigned j = 0; j < 2; j++) {
            for (int i = 0; i < 5; i++)
                memcpy(u + 72 * i, p + 144 * i + 72 * j, 72);
            bool dok = ysf_dch_decode(u, 20, d, NULL);
            emit_dch(rx, &f, d, 20, j, dok);
            if (dok)
                unit20(rx, &f, 2u * f.fn + j, d);
        }
        if (f.fn == f.ft)
            emit_data(rx, &f);
        break;
    }
    case YSF_DT_VOICE_FR: {
        if (f.ft == 1 && f.fn == 0) {
            /* CC0 sub header: DCH(0..4) = CSD3, 72 reserved, VCH(3), VCH(4) */
            bool dok = ysf_dch_decode(p, 20, d, NULL);
            emit_dch(rx, &f, d, 20, 0, dok);
            if (dok)
                unit20(rx, &f, 2, d);
            for (unsigned i = 3; i < 5; i++) {
                uint8_t v[18];
                dv_pack_msb(p + 432 + 144 * (i - 3), 144, v);
                voice_out(rx, YSF_VOICE_FR, v, i, &f);
            }
            break;
        }
        for (unsigned i = 0; i < 5; i++) {
            uint8_t v[18];
            dv_pack_msb(p + 144 * i, 144, v);
            voice_out(rx, YSF_VOICE_FR, v, i, &f);
        }
        break;
    }
    }
}

static void frame_done(ysf_rx_t *rx)
{
    uint8_t fr[YSF_FRAME_DIBITS];
    memcpy(fr, rx->sync_buf, YSF_SYNC_DIBITS);
    memcpy(fr + YSF_SYNC_DIBITS, rx->buf, sizeof(rx->buf));
    ysf_rx_frame(rx, fr);
    if (rx->have_last && rx->last.fi == YSF_FI_TERM) {
        ysf_rx_reset(rx);
        return;
    }
    rx->state = YSF_RX_SYNC;
    rx->n = 0;
}

void ysf_rx_push_dibit(ysf_rx_t *rx, uint8_t d)
{
    d &= 3u;
    rx->sr = ((rx->sr << 2) | d) & SYNC_MASK;
    switch (rx->state) {
    case YSF_RX_SEARCH:
        if (dv_popcount64(rx->sr ^ YSF_SYNC) <= rx->max_sync_err) {
            for (int i = 0; i < 20; i++)
                rx->sync_buf[i] = (uint8_t)((rx->sr >> (38 - 2 * i)) & 3u);
            rx->state = YSF_RX_FRAME;
            rx->n = 0;
            rx->miss = 0;
        }
        break;
    case YSF_RX_FRAME:
        rx->buf[rx->n++] = d;
        if (rx->n == sizeof(rx->buf))
            frame_done(rx);
        break;
    case YSF_RX_SYNC:
        rx->sync_buf[rx->n++] = d;
        if (rx->n == YSF_SYNC_DIBITS) {
            if (dv_popcount64(rx->sr ^ YSF_SYNC) <= rx->max_flywheel_err) {
                rx->miss = 0;
            } else if (++rx->miss >= rx->max_miss) {
                ysf_event_t ev;
                ev_base(&ev, YSF_EV_LOST, &rx->last, false, 0);
                emit(rx, &ev);
                ysf_rx_reset(rx);
                break;
            }
            rx->state = YSF_RX_FRAME;
            rx->n = 0;
        }
        break;
    }
}

void ysf_rx_push_dibits(ysf_rx_t *rx, const uint8_t *dibits, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ysf_rx_push_dibit(rx, dibits[i]);
}
