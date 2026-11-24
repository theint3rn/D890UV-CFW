/* NXDN Type-C trunking, subscriber unit side (TS 1-A 2.2, 5.6, 6.4; TS 1-C procedures, simplified).
 * Hunt control channels, camp on a valid outbound CAC (SITE_INFO gives location / control channels),
 * register (REG_REQ, GRP_REG_REQ), follow VCALL_ASSGN for wanted groups or our unit onto the traffic
 * channel, return on TX_REL / DISC / hang timeout / sync loss. Inbound messages go out through the
 * tx callback (the platform puts them in a Long/Short CAC frame aligned to the inbound frame timing).
 */
#include "nxdn.h"
#include <string.h>

#define NXT_CC_LOSS_MS  2500u
#define NXT_REQ_WAIT_MS 1000u
#define NXT_FRAME_MS    80u

static void nemit(nxt_t *t, nxt_event_t *ev) { if (t->cb.event) t->cb.event(t->cb.user, ev); }
static void nev(nxt_event_t *ev, nxt_ev_type_t ty) { memset(ev, 0, sizeof *ev); ev->type = ty; }

void nxt_init(nxt_t *t, const nxt_cfg_t *cfg, const nxt_cb_t *cb)
{
    memset(t, 0, sizeof *t);
    t->cfg = *cfg;
    if (!t->cfg.dwell_ms) t->cfg.dwell_ms = 1000;
    if (!t->cfg.hang_ms) t->cfg.hang_ms = 3000;
    if (cb) t->cb = *cb;
}

static void hunt_tune(nxt_t *t)
{
    if (!t->cfg.n_hunt) return;
    t->cc = t->cfg.hunt[t->hunt_idx % t->cfg.n_hunt];
    t->state = NXT_HUNT;
    t->t0 = t->now;
    if (t->cb.tune) t->cb.tune(t->cb.user, t->cc, true);
    nxt_event_t ev;
    nev(&ev, NXT_EV_HUNT_TUNE);
    ev.channel = t->cc;
    nemit(t, &ev);
}

void nxt_start(nxt_t *t)
{
    t->hunt_idx = 0;
    t->registered = false;
    hunt_tune(t);
}

static bool group_wanted(const nxt_t *t, uint16_t g)
{
    for (unsigned i = 0; i < t->cfg.n_groups; i++) if (t->cfg.groups[i] == g) return true;
    return false;
}

static bool queue_req(nxt_t *t, const uint8_t *msg, unsigned len, uint8_t type)
{
    if (t->state != NXT_CAMPED || t->req_pending || len > sizeof t->req_msg) return false;
    memcpy(t->req_msg, msg, len);
    t->req_len = (uint8_t)len;
    t->req_type = type;
    t->req_pending = true;
    t->req_tries = 0;
    t->req_due = t->now + 1u + (uint32_t)((t->cfg.unit_id ^ t->now) % 4u) * NXT_FRAME_MS;
    return true;
}

bool nxt_register(nxt_t *t)
{
    uint8_t m[12];
    unsigned n = nxdn_msg_build_reg_req(m, t->site.lid.category, t->site.lid.system, t->cfg.unit_id,
                                        t->cfg.n_groups ? t->cfg.groups[0] : 0);
    return queue_req(t, m, n, NXDN_MSG_REG);
}

bool nxt_request_group_call(nxt_t *t, uint16_t group)
{
    uint8_t m[10];
    unsigned n = nxdn_msg_build_vcall_req(m, NXDN_CALL_CONFERENCE, t->cfg.unit_id, group);
    return queue_req(t, m, n, NXDN_MSG_VCALL_REQ);
}

static void camp(nxt_t *t)
{
    t->state = NXT_CAMPED;
    t->t0 = t->now;
    t->last_cc = t->now;
    nxt_event_t ev;
    nev(&ev, NXT_EV_CAMPED);
    ev.channel = t->cc;
    ev.site = t->site;
    nemit(t, &ev);
}

static void go_traffic(nxt_t *t, const nxdn_call_t *c)
{
    t->call = *c;
    t->state = NXT_TRAFFIC;
    t->t0 = t->last_tr = t->now;
    t->voice_seen = false;
    t->req_pending = false;
    nxt_event_t ev;
    nev(&ev, NXT_EV_GRANT);
    ev.channel = c->channel;
    ev.call = *c;
    nemit(t, &ev);
    if (t->cb.tune) t->cb.tune(t->cb.user, c->channel, false);
}

static void back_to_cc(nxt_t *t)
{
    t->state = NXT_CAMPED;
    t->t0 = t->last_cc = t->now;
    if (t->cb.tune) t->cb.tune(t->cb.user, t->cc, true);
    nxt_event_t ev;
    nev(&ev, NXT_EV_RETURN);
    ev.channel = t->cc;
    nemit(t, &ev);
}

static void cc_msg(nxt_t *t, const uint8_t *m, unsigned len)
{
    uint8_t type = m[0] & 0x3Fu;
    nxt_event_t ev;
    switch (type) {
    case NXDN_MSG_SITE_INFO:
        if (len >= 18) { nxdn_msg_parse_site_info(m, &t->site); t->site_valid = true; }
        break;
    case NXDN_MSG_VCALL_ASSGN:
    case NXDN_MSG_VCALL_ASSGN_DUP: {
        nxdn_call_t c;
        nxdn_msg_parse_call(m, &c);
        bool group = c.call_type == NXDN_CALL_CONFERENCE || c.call_type == NXDN_CALL_BROADCAST;
        bool mine = (group && group_wanted(t, c.dst)) || (!group && c.dst == t->cfg.unit_id) ||
                    c.src == t->cfg.unit_id;
        if (mine && t->state == NXT_CAMPED) go_traffic(t, &c);
        break;
    }
    case NXDN_MSG_REG: {
        nxdn_reg_t r;
        nxdn_msg_parse_reg(m, &r);
        if (r.unit != t->cfg.unit_id || !t->req_pending || t->req_type != NXDN_MSG_REG) break;
        t->req_pending = false;
        t->registered = (r.cause & 0x7Fu) == NXDN_CAUSE_REG_ACCEPTED;
        nev(&ev, t->registered ? NXT_EV_REGISTERED : NXT_EV_REG_FAILED);
        ev.cause = r.cause;
        nemit(t, &ev);
        if (t->registered && t->cfg.n_groups) {
            uint8_t g[8];
            unsigned n = nxdn_msg_build_grp_reg_req(g, t->cfg.unit_id, t->cfg.groups[0]);
            queue_req(t, g, n, NXDN_MSG_GRP_REG);
        }
        break;
    }
    case NXDN_MSG_GRP_REG:
        if (t->req_pending && t->req_type == NXDN_MSG_GRP_REG) t->req_pending = false;
        break;
    case NXDN_MSG_VCALL_RESP: {
        nxdn_call_t c;
        nxdn_msg_parse_call(m, &c);
        if (c.src == t->cfg.unit_id && t->req_pending && t->req_type == NXDN_MSG_VCALL_REQ && len >= 8) {
            uint8_t cause = m[7];
            if ((cause >> 4) >= 4) {           /* resource / service unavailable, procedure error */
                t->req_pending = false;
                nev(&ev, NXT_EV_REQ_FAILED);
                ev.cause = cause;
                nemit(t, &ev);
            } else {
                t->req_due = t->now + 10000u;   /* queued / accepted: assignment follows */
                t->req_tries = 0xFF;
            }
        }
        break;
    }
    default:
        break;
    }
}

void nxt_on_rx(nxt_t *t, const nxdn_event_t *ev)
{
    if (t->state == NXT_OFF) return;
    if (t->state == NXT_TRAFFIC) {
        if (ev->type == NXDN_EV_LICH || ev->type == NXDN_EV_SACCH) t->last_tr = t->now;
        if (ev->type == NXDN_EV_VOICE_START) t->voice_seen = true;
        if (ev->type == NXDN_EV_VOICE_END) { t->voice_seen = true; t->last_tr = t->now; }
        if (ev->type == NXDN_EV_FACCH1 && ev->len && (ev->data[0] & 0x3Fu) == NXDN_MSG_DISC) back_to_cc(t);
        return;
    }
    if (ev->type != NXDN_EV_CAC || !ev->lich.dir) return;
    if (t->cfg.ran >= 0 && (ev->sr & 0x3Fu) != (uint8_t)t->cfg.ran) return;
    t->last_cc = t->now;
    if (ev->sr & 1u) {        /* dual message format: two 72-bit messages */
        cc_msg(t, ev->data, 9);
        cc_msg(t, ev->data + 9, 9);
    } else {
        cc_msg(t, ev->data, ev->len);
    }
    if (t->state == NXT_HUNT) {
        camp(t);
        if (t->cfg.auto_register && !t->registered) nxt_register(t);
    }
}

void nxt_tick(nxt_t *t, uint32_t now)
{
    t->now = now;
    switch (t->state) {
    case NXT_HUNT:
        if (now - t->t0 >= t->cfg.dwell_ms) { t->hunt_idx++; hunt_tune(t); }
        break;
    case NXT_CAMPED:
        if (now - t->last_cc > NXT_CC_LOSS_MS) {
            nxt_event_t ev;
            nev(&ev, NXT_EV_LOST_CC);
            ev.channel = t->cc;
            nemit(t, &ev);
            t->req_pending = false;
            t->hunt_idx++;
            hunt_tune(t);
            break;
        }
        if (t->req_pending && now >= t->req_due) {
            if (t->req_tries >= 3) {
                nxt_event_t ev;
                nev(&ev, t->req_type == NXDN_MSG_REG ? NXT_EV_REG_FAILED : NXT_EV_REQ_FAILED);
                t->req_pending = false;
                nemit(t, &ev);
                break;
            }
            uint8_t info[17];
            memset(info, 0, sizeof info);
            info[0] = (uint8_t)(t->cfg.ran >= 0 ? t->cfg.ran : 0);     /* SR: RAN */
            bool shortc = t->req_len <= 12;
            memcpy(info + 1, t->req_msg, t->req_len);
            if (t->cb.tx) t->cb.tx(t->cb.user, info, shortc ? 13u : 17u, shortc);
            t->req_tries++;
            t->req_due = now + NXT_REQ_WAIT_MS;
        }
        break;
    case NXT_TRAFFIC:
        if (now - t->last_tr > (t->voice_seen ? t->cfg.hang_ms : 1500u)) back_to_cc(t);
        break;
    default:
        break;
    }
}
