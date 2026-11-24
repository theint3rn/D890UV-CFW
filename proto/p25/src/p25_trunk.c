/*
 * p25_trunk.c - trunking follower state machine (see p25_trunk.h).
 *
 *   IDLE --start--> HUNT --valid TSBK--> CC --grant(tg wanted)--> VOICE
 *                    ^  \__dwell timeout: next CC                   |
 *                    |                                               |
 *                    +---- CC quiet timeout <---- CC <-- TDU/TDULC/END_PTT/hang
 *   VOICE --grant/update for a higher-priority tg--> VOICE (preempt)
 */
#include "p25_trunk.h"
#include <string.h>

void p25_trunk_init(p25_trunk_t *t, const p25_trunk_cb_t *cb, void *ctx)
{
    memset(t, 0, sizeof *t);
    t->cb = cb;
    t->ctx = ctx;
    t->default_prio = 128;
    t->cc_timeout_ms = 1500;
    t->grant_wait_ms = 1000;
    t->hang_ms = 1500;
}

bool p25_trunk_add_cc(p25_trunk_t *t, uint32_t hz)
{
    if (t->n_cc >= P25_TRUNK_MAX_CC) return false;
    t->cc_list[t->n_cc++] = hz;
    return true;
}

bool p25_trunk_add_tg(p25_trunk_t *t, uint16_t tg, uint8_t prio, bool lockout)
{
    for (unsigned i = 0; i < t->n_tg; i++)
        if (t->tgs[i].tg == tg) { t->tgs[i].prio = prio; t->tgs[i].lockout = lockout; return true; }
    if (t->n_tg >= P25_TRUNK_MAX_TG) return false;
    t->tgs[t->n_tg].tg = tg;
    t->tgs[t->n_tg].prio = prio;
    t->tgs[t->n_tg].lockout = lockout;
    t->n_tg++;
    return true;
}

uint8_t p25_trunk_tg_prio(const p25_trunk_t *t, uint16_t tg)
{
    for (unsigned i = 0; i < t->n_tg; i++)
        if (t->tgs[i].tg == tg) return t->tgs[i].lockout ? 0 : t->tgs[i].prio;
    return t->scan_all ? t->default_prio : 0;
}

static void emit(p25_trunk_t *t, p25_trunk_ev_type_t type, uint16_t tg, uint32_t src)
{
    if (!t->cb || !t->cb->event) return;
    p25_trunk_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.tg = tg;
    ev.src = src;
    ev.rx_hz = t->state == P25_TS_VOICE ? t->call_rx_hz : t->cc_hz;
    ev.slot = t->call_slot;
    ev.mode = t->state == P25_TS_VOICE ? t->call_mode : P25_MODE_P1;
    ev.prio = t->call_prio;
    t->cb->event(t->ctx, &ev);
}

static void tune(p25_trunk_t *t, uint32_t rx, uint32_t tx, p25_mode_t mode, uint8_t slot)
{
    if (t->cb && t->cb->tune) t->cb->tune(t->ctx, rx, tx, mode, slot);
}

static void tune_cc(p25_trunk_t *t)
{
    uint32_t tx = 0;
    if (!p25_trunk_cc_tx_hz(t, &tx)) tx = t->cc_hz;
    tune(t, t->cc_hz, tx, P25_MODE_P1, 0);
}

static void enter_hunt(p25_trunk_t *t, bool next)
{
    if (!t->n_cc) { t->state = P25_TS_IDLE; return; }
    if (next) t->cc_idx = (uint8_t)((t->cc_idx + 1) % t->n_cc);
    t->state = P25_TS_HUNT;
    t->cc_hz = t->cc_list[t->cc_idx];
    t->t_state = 0;
    t->t_quiet = 0;
    tune(t, t->cc_hz, t->cc_hz, P25_MODE_P1, 0);
}

void p25_trunk_start(p25_trunk_t *t)
{
    t->cc_idx = 0;
    enter_hunt(t, false);
}

void p25_trunk_stop(p25_trunk_t *t)
{
    t->state = P25_TS_IDLE;
}

static void back_to_cc(p25_trunk_t *t)
{
    uint16_t tg = t->call_tg;
    uint32_t src = t->call_src;
    t->state = P25_TS_CC;
    t->t_state = 0;
    t->t_quiet = 0;
    tune_cc(t);
    emit(t, P25_TE_CALL_END, tg, src);
}

static void follow(p25_trunk_t *t, uint16_t tg, uint16_t ch, uint32_t src, uint8_t prio)
{
    uint32_t rx, tx;
    uint8_t slot;
    bool tdma;
    if (!p25_chan_to_freq(t->iden, ch, &rx, &tx, &slot, &tdma)) return;
    if (t->state == P25_TS_VOICE) emit(t, P25_TE_CALL_PREEMPT, t->call_tg, t->call_src);
    t->state = P25_TS_VOICE;
    t->t_state = 0;
    t->t_quiet = 0;
    t->voice_seen = false;
    t->call_tg = tg;
    t->call_src = src;
    t->call_prio = prio;
    t->call_rx_hz = rx;
    t->call_slot = slot;
    t->call_mode = tdma ? P25_MODE_P2 : P25_MODE_P1;
    tune(t, rx, tx, t->call_mode, slot);
    emit(t, P25_TE_CALL_START, tg, src);
}

static void consider(p25_trunk_t *t, uint16_t tg, uint16_t ch, uint32_t src)
{
    uint8_t prio = p25_trunk_tg_prio(t, tg);
    if (!prio) return;
    if (t->state == P25_TS_CC) follow(t, tg, ch, src, prio);
    else if (t->state == P25_TS_VOICE && tg != t->call_tg && prio < t->call_prio) follow(t, tg, ch, src, prio);
}

static void site_check(p25_trunk_t *t)
{
    if ((t->wacn_filter && t->wacn && t->wacn != t->wacn_filter) ||
        (t->sysid_filter && t->sysid && t->sysid != t->sysid_filter)) {
        emit(t, P25_TE_CC_LOST, 0, 0);
        t->wacn = 0;
        t->sysid = 0;
        enter_hunt(t, true);
    }
}

void p25_trunk_osp(p25_trunk_t *t, const p25_tsbk_t *m)
{
    if (!m->known) return;
    switch (m->opcode) {
    case P25_OSP_GRP_V_CH_GRANT:
        consider(t, m->u.grant.grp, m->u.grant.ch, m->u.grant.src);
        break;
    case P25_OSP_GRP_V_CH_GRANT_UPDT:
    case P25_OSP_GRP_V_CH_GRANT_UPDT_EXP:
        for (unsigned i = 0; i < m->u.grant_updt.n; i++)
            consider(t, m->u.grant_updt.grp[i], m->u.grant_updt.ch[i], 0);
        break;
    case P25_OSP_IDEN_UP:
    case P25_OSP_IDEN_UP_VU:
    case P25_OSP_IDEN_UP_TDMA:
        t->iden[m->u.iden.iden & 15] = m->u.iden.e;
        break;
    case P25_OSP_NET_STS_BCST:
        if (t->wacn != m->u.net.wacn || t->sysid != m->u.net.sysid) {
            t->wacn = m->u.net.wacn;
            t->sysid = m->u.net.sysid;
            emit(t, P25_TE_SITE, 0, 0);
        }
        site_check(t);
        break;
    case P25_OSP_RFSS_STS_BCST:
        t->cc_ch = m->u.rfss.ch;
        if (t->rfss != m->u.rfss.rfss || t->site != m->u.rfss.site || t->sysid != m->u.rfss.sysid) {
            t->rfss = m->u.rfss.rfss;
            t->site = m->u.rfss.site;
            t->sysid = m->u.rfss.sysid;
            emit(t, P25_TE_SITE, 0, 0);
        }
        site_check(t);
        break;
    case P25_OSP_ADJ_STS_BCST: {
        unsigned i;
        for (i = 0; i < t->n_adj; i++)
            if (t->adj[i].sysid == m->u.adj.sysid && t->adj[i].rfss == m->u.adj.rfss && t->adj[i].site == m->u.adj.site)
                break;
        if (i == t->n_adj) {
            if (t->n_adj < P25_TRUNK_MAX_ADJ) t->n_adj++;
            else i = P25_TRUNK_MAX_ADJ - 1;
        }
        bool changed = t->adj[i].ch != m->u.adj.ch || t->adj[i].sysid != m->u.adj.sysid;
        t->adj[i].sysid = m->u.adj.sysid;
        t->adj[i].rfss = m->u.adj.rfss;
        t->adj[i].site = m->u.adj.site;
        t->adj[i].ch = m->u.adj.ch;
        t->adj[i].cfva = m->u.adj.cfva;
        if (changed && t->cb && t->cb->event) {
            p25_trunk_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = P25_TE_ADJ;
            ev.prio = (uint8_t)i;
            p25_chan_to_freq(t->iden, m->u.adj.ch, &ev.rx_hz, &ev.tx_hz, NULL, NULL);
            t->cb->event(t->ctx, &ev);
        }
        break;
    }
    case P25_OSP_GRP_AFF_RSP:
        emit(t, P25_TE_AFF_RSP, m->u.aff_rsp.ga, m->u.aff_rsp.gav);
        break;
    case P25_OSP_U_REG_RSP:
        emit(t, P25_TE_REG_RSP, 0, m->u.reg_rsp.rv);
        break;
    case P25_OSP_DENY_RSP:
        emit(t, P25_TE_DENY, 0, m->u.deny.reason);
        break;
    default:
        break;
    }
}

static void cc_alive(p25_trunk_t *t, uint16_t nac)
{
    if (t->state == P25_TS_HUNT) {
        t->state = P25_TS_CC;
        t->t_state = 0;
        t->nac = nac;
        emit(t, P25_TE_CC_FOUND, 0, 0);
    }
    if (t->state == P25_TS_CC) {
        t->t_quiet = 0;
        if (t->nac != nac) { t->nac = nac; emit(t, P25_TE_SITE, 0, 0); }
    }
}

void p25_trunk_voice_activity(p25_trunk_t *t)
{
    if (t->state == P25_TS_VOICE) {
        t->voice_seen = true;
        t->t_quiet = 0;
    }
}

void p25_trunk_p1_event(p25_trunk_t *t, const p25_p1_event_t *ev)
{
    if (t->state == P25_TS_HUNT || t->state == P25_TS_CC) {
        if (ev->type == P25_P1_EV_TSBK && ev->u.tsbk.crc_ok) {
            p25_tsbk_t m;
            cc_alive(t, ev->nac);
            p25_tsbk_parse(ev->u.tsbk.tsbk, &m);
            p25_trunk_osp(t, &m);
        }
        return;
    }
    if (t->state != P25_TS_VOICE || t->call_mode != P25_MODE_P1) return;
    switch (ev->type) {
    case P25_P1_EV_HDU:
    case P25_P1_EV_LDU2:
        p25_trunk_voice_activity(t);
        break;
    case P25_P1_EV_LDU1:
        p25_trunk_voice_activity(t);
        if (ev->u.ldu1.lc_ok) {
            const uint8_t *b = ev->u.ldu1.lc.b;
            p25_lc_info_t li;
            p25_lc_parse(&ev->u.ldu1.lc, &li);
            if (li.known && li.is_group && li.tgid == t->call_tg) t->call_src = li.src;
            if ((b[0] & 0x3F) == P25_LCO_GRP_V_UPDT) {     /* channel A/B + group A/B (implicit MFID) */
                consider(t, (uint16_t)((b[3] << 8) | b[4]), (uint16_t)((b[1] << 8) | b[2]), 0);
                if (t->state == P25_TS_VOICE)
                    consider(t, (uint16_t)((b[7] << 8) | b[8]), (uint16_t)((b[5] << 8) | b[6]), 0);
            }
        }
        break;
    case P25_P1_EV_TDU:
    case P25_P1_EV_TDULC:
        back_to_cc(t);
        break;
    default:
        break;
    }
}

static void mac_msg(void *ctx, uint8_t mco, const uint8_t *msg, unsigned len, const p25_tsbk_t *osp)
{
    p25_trunk_t *t = ctx;
    (void)mco; (void)msg; (void)len;
    if (!osp || !osp->known) return;
    /* on a voice channel only grants matter (preemption); site info is kept */
    if (osp->opcode == P25_OSP_GRP_V_CH_GRANT || osp->opcode == P25_OSP_GRP_V_CH_GRANT_UPDT ||
        osp->opcode == P25_OSP_GRP_V_CH_GRANT_UPDT_EXP || osp->opcode == P25_OSP_IDEN_UP_TDMA ||
        osp->opcode == P25_OSP_IDEN_UP_VU || osp->opcode == P25_OSP_IDEN_UP)
        p25_trunk_osp(t, osp);
}

void p25_trunk_p2_event(p25_trunk_t *t, const p25_p2_event_t *ev)
{
    if (t->state != P25_TS_VOICE || t->call_mode != P25_MODE_P2) return;
    if (ev->type == P25_P2_EV_ESS && ev->channel == t->call_slot) p25_trunk_voice_activity(t);
    if (ev->type != P25_P2_EV_MAC) return;
    if (ev->channel == t->call_slot) {
        switch (ev->mac.opcode) {
        case P25_MAC_PTT:
            p25_trunk_voice_activity(t);
            if (ev->mac.grp == t->call_tg) t->call_src = ev->mac.src;
            return;
        case P25_MAC_END_PTT:
            back_to_cc(t);
            return;
        case P25_MAC_ACTIVE:
            p25_trunk_voice_activity(t);
            break;
        default:
            break;
        }
    }
    p25_mac_messages(&ev->mac, mac_msg, t);
}

void p25_trunk_tick(p25_trunk_t *t, uint32_t ms)
{
    t->t_state += ms;
    t->t_quiet += ms;
    switch (t->state) {
    case P25_TS_HUNT:
        if (t->t_state >= t->cc_timeout_ms) enter_hunt(t, true);
        break;
    case P25_TS_CC:
        if (t->t_quiet >= t->cc_timeout_ms) {
            emit(t, P25_TE_CC_LOST, 0, 0);
            enter_hunt(t, false);
        }
        break;
    case P25_TS_VOICE:
        if (t->t_quiet >= (t->voice_seen ? t->hang_ms : t->grant_wait_ms)) back_to_cc(t);
        break;
    default:
        break;
    }
}

bool p25_trunk_cc_tx_hz(const p25_trunk_t *t, uint32_t *tx_hz)
{
    if (!t->cc_ch) return false;
    return p25_chan_to_freq(t->iden, t->cc_ch, NULL, tx_hz, NULL, NULL);
}
