/* DMR Tier III MS procedures (ETSI TS 102 361-4 clauses 6 and 7). */
#include "dmr_t3.h"
#include "../common/bits.h"
#include <string.h>

#define TDMA_FRAME_MS 60u
#define CC_LOSS_MS    2000u
#define TRAFFIC_LOSS_MS 1500u

/* ---------------- PDU coding ---------------- */
t3_ack_class_t dmr_t3_ack_class(uint8_t reason)
{
    switch (reason >> 5) {
    case 3: return T3_ACK_ACK;          /* 011x xxxx: TS acknowledgement */
    case 2: return T3_ACK_MS;           /* 010x xxxx: MS acknowledgement (mirrored) */
    case 5: return T3_ACK_QUEUED;       /* 1010 xxxx */
    case 7: return T3_ACK_WAIT;         /* 1110 xxxx */
    default: return T3_ACK_NACK;        /* 000x / 001x */
    }
}

void dmr_t3_syscode_parse(uint16_t sc, t3_syscode_t *s)
{
    static const uint8_t netbits[4] = { 9, 7, 4, 2 };     /* tiny, small, large, huge */
    s->model = (uint8_t)(sc >> 14);
    s->par = (uint8_t)(sc & 3u);
    uint16_t ns = (uint16_t)((sc >> 2) & 0xFFFu);
    unsigned sb = 12u - netbits[s->model];
    s->net = (uint16_t)(ns >> sb);
    s->site = (uint16_t)(ns & ((1u << sb) - 1u));
}

#define F(c, pos, n) bits_field((c)->data, (pos), (n))

void dmr_t3_parse_aloha(const dmr_csbk_t *c, t3_aloha_t *a)
{
    a->tsccas = (uint8_t)F(c, 1, 1);
    a->site_sync = (uint8_t)F(c, 2, 1);
    a->version = (uint8_t)F(c, 3, 3);
    a->offset = (uint8_t)F(c, 6, 1);
    a->active_conn = (uint8_t)F(c, 7, 1);
    a->mask = (uint8_t)F(c, 8, 5);
    a->svc_fn = (uint8_t)F(c, 13, 2);
    a->nrand_wait = (uint8_t)F(c, 15, 4);
    a->reg = (uint8_t)F(c, 19, 1);
    a->backoff = (uint8_t)F(c, 20, 4);
    a->syscode = (uint16_t)F(c, 24, 16);
    a->ms_addr = F(c, 40, 24);
}

void dmr_t3_parse_grant(const dmr_csbk_t *c, t3_grant_t *g)
{
    g->lpcn = (uint16_t)F(c, 0, 12);
    g->lcn = (uint8_t)F(c, 12, 1);
    g->late_entry = (uint8_t)F(c, 13, 1);
    g->emergency = (uint8_t)F(c, 14, 1);
    g->offset = (uint8_t)F(c, 15, 1);
    g->target = F(c, 16, 24);
    g->source = F(c, 40, 24);
}

void dmr_t3_parse_bcast(const dmr_csbk_t *c, t3_bcast_t *b)
{
    b->type = (uint8_t)F(c, 0, 5);
    b->parms1 = (uint16_t)F(c, 5, 14);
    b->reg = (uint8_t)F(c, 19, 1);
    b->backoff = (uint8_t)F(c, 20, 4);
    b->syscode = (uint16_t)F(c, 24, 16);
    b->parms2 = F(c, 40, 24);
}

void dmr_t3_parse_ahoy(const dmr_csbk_t *c, t3_ahoy_t *a)
{
    a->som = (uint8_t)F(c, 0, 7);
    a->skf = (uint8_t)F(c, 7, 1);
    a->als = (uint8_t)F(c, 8, 1);
    a->gi = (uint8_t)F(c, 9, 1);
    a->ab = (uint8_t)F(c, 10, 2);
    a->svc_kind = (uint8_t)F(c, 12, 4);
    a->target = F(c, 16, 24);
    a->source = F(c, 40, 24);
}

void dmr_t3_parse_ack(const dmr_csbk_t *c, t3_ack_t *a)
{
    a->resp_info = (uint8_t)F(c, 0, 7);
    a->reason = (uint8_t)F(c, 7, 8);
    a->target = F(c, 16, 24);
    a->source = F(c, 40, 24);
}

#define S(d, pos, n, v) bits_set_field((d), (pos), (n), (uint32_t)(v))

void dmr_t3_build_aloha(dmr_csbk_t *c, const t3_aloha_t *a)
{
    uint8_t d[8] = { 0 };
    S(d, 1, 1, a->tsccas); S(d, 2, 1, a->site_sync); S(d, 3, 3, a->version); S(d, 6, 1, a->offset);
    S(d, 7, 1, a->active_conn); S(d, 8, 5, a->mask); S(d, 13, 2, a->svc_fn); S(d, 15, 4, a->nrand_wait);
    S(d, 19, 1, a->reg); S(d, 20, 4, a->backoff); S(d, 24, 16, a->syscode); S(d, 40, 24, a->ms_addr);
    dmr_csbk_set(c, T3_C_ALOHA, 0, d);
}

void dmr_t3_build_grant(dmr_csbk_t *c, uint8_t opcode, const t3_grant_t *g)
{
    uint8_t d[8] = { 0 };
    S(d, 0, 12, g->lpcn); S(d, 12, 1, g->lcn); S(d, 13, 1, g->late_entry); S(d, 14, 1, g->emergency);
    S(d, 15, 1, g->offset); S(d, 16, 24, g->target); S(d, 40, 24, g->source);
    dmr_csbk_set(c, opcode, 0, d);
}

void dmr_t3_build_bcast(dmr_csbk_t *c, const t3_bcast_t *b)
{
    uint8_t d[8] = { 0 };
    S(d, 0, 5, b->type); S(d, 5, 14, b->parms1); S(d, 19, 1, b->reg); S(d, 20, 4, b->backoff);
    S(d, 24, 16, b->syscode); S(d, 40, 24, b->parms2);
    dmr_csbk_set(c, T3_C_BCAST, 0, d);
}

void dmr_t3_build_ack(dmr_csbk_t *c, uint8_t opcode, const t3_ack_t *a)
{
    uint8_t d[8] = { 0 };
    S(d, 0, 7, a->resp_info); S(d, 7, 8, a->reason); S(d, 16, 24, a->target); S(d, 40, 24, a->source);
    dmr_csbk_set(c, opcode, 0, d);
}

void dmr_t3_build_rand(dmr_csbk_t *c, uint8_t svc_opts, uint8_t proxy, uint8_t svc_kind, uint32_t target,
                       uint32_t source)
{
    uint8_t d[8] = { 0 };
    S(d, 0, 7, svc_opts); S(d, 7, 1, proxy); S(d, 12, 4, svc_kind); S(d, 16, 24, target); S(d, 40, 24, source);
    dmr_csbk_set(c, T3_C_RAND, 0, d);
}

void dmr_t3_build_p_clear(dmr_csbk_t *c, uint16_t lpcn, uint8_t gi, uint32_t target, uint32_t source)
{
    uint8_t d[8] = { 0 };
    S(d, 0, 12, lpcn); S(d, 15, 1, gi); S(d, 16, 24, target); S(d, 40, 24, source);
    dmr_csbk_set(c, T3_P_CLEAR, 0, d);
}

/* ---------------- state machine ---------------- */
static void t3_emit(dmr_t3_t *t, t3_event_t *ev)
{
    if (t->cb.event) t->cb.event(t->cb.user, ev);
}

static void ev0(t3_event_t *ev, t3_ev_type_t ty)
{
    memset(ev, 0, sizeof *ev);
    ev->type = ty;
}

static uint32_t t3_rand(dmr_t3_t *t)
{
    uint32_t x = t->rng ? t->rng : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    t->rng = x;
    return x;
}

void dmr_t3_init(dmr_t3_t *t, const dmr_t3_cfg_t *cfg, const dmr_t3_cb_t *cb)
{
    memset(t, 0, sizeof *t);
    t->cfg = *cfg;
    if (!t->cfg.dwell_ms) t->cfg.dwell_ms = 600;
    if (!t->cfg.hang_ms) t->cfg.hang_ms = 3000;
    if (!t->cfg.nrand) t->cfg.nrand = 4;
    if (cb) t->cb = *cb;
    t->rng = cfg->my_id * 2654435761u + 1u;
    t->state = T3_ST_OFF;
}

bool dmr_t3_tg_wanted(const dmr_t3_t *t, uint32_t tg)
{
    for (unsigned i = 0; i < t->cfg.n_tg; i++) if (t->cfg.tg[i] == tg) return true;
    return false;
}

static void tune_hunt(dmr_t3_t *t)
{
    if (!t->cfg.n_hunt) return;
    t->cc_lpcn = t->cfg.hunt[t->hunt_idx % t->cfg.n_hunt];
    t->state = T3_ST_HUNT;
    t->state_t0 = t->now;
    t->aloha_seen = false;
    if (t->cb.tune) t->cb.tune(t->cb.user, t->cc_lpcn, t->cfg.tscc_slot, true);
    t3_event_t ev;
    ev0(&ev, T3_EV_HUNT_TUNE);
    ev.lpcn = t->cc_lpcn;
    ev.slot = t->cfg.tscc_slot;
    t3_emit(t, &ev);
}

void dmr_t3_start(dmr_t3_t *t)
{
    t->hunt_idx = 0;
    t->registered = false;
    tune_hunt(t);
}

static void return_to_cc(dmr_t3_t *t)
{
    t->state = T3_ST_CAMPED;
    t->state_t0 = t->now;
    t->last_cc_rx = t->now;
    if (t->cb.tune) t->cb.tune(t->cb.user, t->cc_lpcn, t->cfg.tscc_slot, true);
    t3_event_t ev;
    ev0(&ev, T3_EV_RETURN);
    ev.lpcn = t->cc_lpcn;
    ev.slot = t->cfg.tscc_slot;
    t3_emit(t, &ev);
}

static void go_traffic(dmr_t3_t *t, uint8_t opcode, const t3_grant_t *g)
{
    t->grant = *g;
    t->grant_opcode = opcode;
    t->state = T3_ST_TRAFFIC;
    t->state_t0 = t->now;
    t->last_traffic_rx = t->now;
    t->voice_seen = false;
    t->ra_active = false;
    t3_event_t ev;
    ev0(&ev, T3_EV_GRANT);
    ev.grant = *g;
    ev.opcode = opcode;
    ev.lpcn = g->lpcn;
    ev.slot = g->lcn;
    t3_emit(t, &ev);
    if (g->lpcn != T3_LPCN_ABSOLUTE && t->cb.tune) t->cb.tune(t->cb.user, g->lpcn, g->lcn, false);
}

/* random access (6.2.1): queue a C_RAND, sent after a permitting Aloha and a random backoff */
static bool ra_start(dmr_t3_t *t, uint8_t svc_opts, uint8_t kind, uint32_t target)
{
    if (t->state != T3_ST_CAMPED || t->ra_active) return false;
    dmr_t3_build_rand(&t->ra_csbk, svc_opts, 0, kind, target, t->cfg.my_id);
    t->ra_active = true;
    t->ra_sent = false;
    t->ra_tries = 0;
    t->ra_kind = kind;
    t->ra_target = target;
    t->ra_due = 0;
    return true;
}

static bool aloha_permits(const dmr_t3_t *t)
{
    if (!t->aloha_seen) return true;                 /* no Aloha yet: assume undivided population */
    const t3_aloha_t *a = &t->aloha;
    if (a->mask) {
        uint32_t m = a->mask >= 24 ? 0xFFFFFFu : ((1u << a->mask) - 1u);
        if ((a->ms_addr & m) != (t->cfg.my_id & m)) return false;
    }
    if (t->ra_kind == T3_SK_REGISTRATION) return true;   /* every Service_Function invites registration */
    if (a->svc_fn == 3) return false;                    /* registration only */
    return true;
}

static uint32_t backoff_frames(uint8_t b)
{
    static const uint8_t tab[16] = { 1, 1, 2, 3, 4, 5, 8, 11, 15, 20, 26, 33, 41, 50, 70, 100 };
    return tab[b & 15u];
}

static uint32_t nrand_wait_frames(uint8_t n)
{
    return n < 14 ? n : (n == 14 ? 15u : 24u);
}

static void ra_schedule(dmr_t3_t *t)
{
    uint32_t bo = backoff_frames(t->aloha_seen ? t->aloha.backoff : 1);
    t->ra_due = t->now + (t3_rand(t) % bo) * TDMA_FRAME_MS + 1u;
}

static void ra_done(dmr_t3_t *t)
{
    t->ra_active = false;
    t->ra_sent = false;
}

bool dmr_t3_register(dmr_t3_t *t)
{
    if (!ra_start(t, 0x01, T3_SK_REGISTRATION, T3_ADDR_REGI)) return false;   /* Reg_Dereg = 1 */
    t->reg_pending = true;
    ra_schedule(t);
    return true;
}

bool dmr_t3_request_voice(dmr_t3_t *t, uint32_t target, bool group)
{
    if (!ra_start(t, 0x00, group ? T3_SK_TG_VOICE : T3_SK_IND_VOICE, target)) return false;
    ra_schedule(t);
    return true;
}

static void camp(dmr_t3_t *t, uint16_t syscode)
{
    t->state = T3_ST_CAMPED;
    t->state_t0 = t->now;
    t3_event_t ev;
    ev0(&ev, T3_EV_CAMPED);
    ev.lpcn = t->cc_lpcn;
    ev.syscode = syscode;
    dmr_t3_syscode_parse(syscode, &ev.sys);
    t3_emit(t, &ev);
    if (t->cfg.auto_register && !t->registered) dmr_t3_register(t);
}

static void on_ack(dmr_t3_t *t, const dmr_csbk_t *c)
{
    t3_ack_t a;
    dmr_t3_parse_ack(c, &a);
    if (a.target != t->cfg.my_id || !t->ra_active) return;
    t3_ack_class_t cl = dmr_t3_ack_class(a.reason);
    t3_event_t ev;
    if (t->reg_pending && t->ra_kind == T3_SK_REGISTRATION) {
        if (cl == T3_ACK_WAIT || cl == T3_ACK_QUEUED) { t->ra_timeout = t->now + 5000u; return; }
        t->reg_pending = false;
        ev0(&ev, cl == T3_ACK_ACK ? T3_EV_REGISTERED : T3_EV_REG_REFUSED);
        ev.reason = a.reason;
        ev.ack = cl;
        t->registered = cl == T3_ACK_ACK;
        ra_done(t);
        t3_emit(t, &ev);
        return;
    }
    ev0(&ev, T3_EV_REQUEST_ACK);
    ev.reason = a.reason;
    ev.ack = cl;
    if (cl == T3_ACK_WAIT || cl == T3_ACK_QUEUED) t->ra_timeout = t->now + 10000u;   /* grant follows */
    else if (cl == T3_ACK_NACK) ra_done(t);
    t3_emit(t, &ev);
}

static void on_csbk_cc(dmr_t3_t *t, const dmr_csbk_t *c)
{
    t3_event_t ev;
    switch (c->opcode) {
    case T3_C_ALOHA: {
        t3_aloha_t a;
        dmr_t3_parse_aloha(c, &a);
        if (t->cfg.syscode_filter >= 0 && a.syscode != (uint16_t)t->cfg.syscode_filter) return;
        t->aloha = a;
        t->aloha_seen = true;
        if (t->state == T3_ST_HUNT) camp(t, a.syscode);
        if (a.reg && !t->registered && !t->ra_active && t->cfg.auto_register) dmr_t3_register(t);
        break;
    }
    case T3_C_BCAST: {
        t3_bcast_t b;
        dmr_t3_parse_bcast(c, &b);
        if (b.type == 0) {          /* Ann_WD_TSCC */
            uint16_t ch[2] = { (uint16_t)(b.parms2 >> 12), (uint16_t)(b.parms2 & 0xFFFu) };
            uint8_t aw[2] = { (uint8_t)((b.parms1 >> 1) & 1u), (uint8_t)(b.parms1 & 1u) };
            bool changed = false;
            for (int k = 0; k < 2; k++) {
                if (!ch[k]) continue;
                int found = -1;
                for (unsigned i = 0; i < t->cfg.n_hunt; i++) if (t->cfg.hunt[i] == ch[k]) found = (int)i;
                if (!aw[k] && found < 0 && t->cfg.n_hunt < T3_MAX_HUNT) { t->cfg.hunt[t->cfg.n_hunt++] = ch[k]; changed = true; }
                if (aw[k] && found >= 0) {
                    t->cfg.hunt[found] = t->cfg.hunt[--t->cfg.n_hunt];
                    changed = true;
                }
            }
            if (changed) { ev0(&ev, T3_EV_TSCC_LIST); ev.csbk = c; t3_emit(t, &ev); }
        } else if (b.type == 6) {
            ev0(&ev, T3_EV_ADJ_SITE);
            ev.lpcn = (uint16_t)(b.parms2 & 0xFFFu);
            ev.csbk = c;
            t3_emit(t, &ev);
        }
        if (t->state == T3_ST_HUNT && (t->cfg.syscode_filter < 0 || b.syscode == (uint16_t)t->cfg.syscode_filter))
            camp(t, b.syscode);
        break;
    }
    case T3_C_ACKD:
        on_ack(t, c);
        break;
    case T3_C_AHOY: {
        t3_ahoy_t a;
        dmr_t3_parse_ahoy(c, &a);
        if (a.target != t->cfg.my_id || a.gi) break;
        if (a.svc_kind == T3_SK_REGISTRATION && a.source <= T3_AUTH_CHALLENGE_MAX) {
            /* authentication challenge (table 6.17): answer C_ACKU(Authentication Response) */
            t3_event_t e;
            ev0(&e, T3_EV_AUTH);
            e.reason = t->cfg.auth ? 0 : 1;
            if (t->cfg.auth) {
                dmr_csbk_t r;
                uint32_t resp = dmr_t3_auth_response(t->cfg.auth_key, a.source, t->cfg.use_psn ? t->cfg.psn : NULL);
                t3_ack_t ack = { 0, T3_REASON_AUTH_RESP, resp, t->cfg.my_id };
                dmr_t3_build_ack(&r, T3_C_ACKU, &ack);
                if (t->cb.tx_csbk) t->cb.tx_csbk(t->cb.user, &r, t->cfg.tscc_slot);
                if (t->ra_active && t->ra_sent) t->ra_timeout = t->now + 5000u;   /* registration continues */
            }
            t3_emit(t, &e);
            break;
        }
        if (t->ra_active && a.source == t->ra_target) break;   /* AHOY as call setup progress */
        /* presence check / radio check: answer C_ACKU(MS_Accepted) */
        dmr_csbk_t r;
        t3_ack_t ack = { 0, T3_REASON_MS_ACCEPTED, a.source, t->cfg.my_id };
        dmr_t3_build_ack(&r, T3_C_ACKU, &ack);
        if (t->cb.tx_csbk) t->cb.tx_csbk(t->cb.user, &r, t->cfg.tscc_slot);
        break;
    }
    case T3_TV_GRANT:
    case T3_BTV_GRANT: {
        t3_grant_t g;
        dmr_t3_parse_grant(c, &g);
        bool mine = dmr_t3_tg_wanted(t, g.target) ||
                    (t->ra_active && t->ra_kind == T3_SK_TG_VOICE && g.target == t->ra_target);
        if (mine) go_traffic(t, c->opcode, &g);
        break;
    }
    case T3_PV_GRANT:
    case T3_PV_GRANT_DX: {
        t3_grant_t g;
        dmr_t3_parse_grant(c, &g);
        if (g.target == t->cfg.my_id || g.source == t->cfg.my_id) go_traffic(t, c->opcode, &g);
        break;
    }
    default:
        break;
    }
}

void dmr_t3_on_rx(dmr_t3_t *t, const dmr_event_t *ev)
{
    if (t->state == T3_ST_OFF) return;
    if (ev->type == DMR_EV_CSBK || ev->type == DMR_EV_MBC) {
        const dmr_csbk_t *c = &ev->csbk;
        if (c->fid != 0) {
            t3_event_t e;
            ev0(&e, T3_EV_MFID_CSBK);
            e.csbk = c;
            e.opcode = c->opcode;
            t3_emit(t, &e);
            return;
        }
        if (t->state == T3_ST_TRAFFIC) {
            t->last_traffic_rx = t->now;
            if (c->opcode == T3_P_CLEAR) { return_to_cc(t); return; }
            if (c->opcode == T3_TV_GRANT || c->opcode == T3_PV_GRANT || c->opcode == T3_BTV_GRANT) {
                /* P_GRANT swap on the payload channel */
                t3_grant_t g;
                dmr_t3_parse_grant(c, &g);
                if (g.target == t->grant.target && g.lpcn != t->grant.lpcn) go_traffic(t, c->opcode, &g);
            }
            return;
        }
        if (ev->slot != t->cfg.tscc_slot) return;
        t->last_cc_rx = t->now;
        on_csbk_cc(t, c);
        return;
    }
    if (ev->type == DMR_EV_SHORT_LC && ev->slc.slco == DMR_SLCO_C_SYS_PARMS && t->state != T3_ST_TRAFFIC) {
        t->last_cc_rx = t->now;
        if (t->state == T3_ST_HUNT) {
            /* MODEL(2) NET/SITE(12) -> syscode with PAR = 0 for the filter */
            uint16_t sc = (uint16_t)((ev->slc.data24 >> 10) << 2);
            if (t->cfg.syscode_filter < 0 || (sc & 0xFFFCu) == ((uint16_t)t->cfg.syscode_filter & 0xFFFCu)) camp(t, sc);
        }
        return;
    }
    if (t->state == T3_ST_TRAFFIC && ev->slot == t->grant.lcn) {
        switch (ev->type) {
        case DMR_EV_VOICE_START:
        case DMR_EV_VOICE_LC_EMB:
        case DMR_EV_DATA_HEADER:
        case DMR_EV_IDLE:
            t->voice_seen = t->voice_seen || ev->type == DMR_EV_VOICE_START;
            t->last_traffic_rx = t->now;
            break;
        case DMR_EV_VOICE_END:
            t->last_traffic_rx = t->now;
            break;
        default:
            break;
        }
    }
    if (ev->type == DMR_EV_SYNC_LOST && t->state != T3_ST_TRAFFIC) {
        /* handled by the CC loss timer */
    }
}

void dmr_t3_tick(dmr_t3_t *t, uint32_t now)
{
    t->now = now;
    switch (t->state) {
    case T3_ST_HUNT:
        if (now - t->state_t0 >= t->cfg.dwell_ms) { t->hunt_idx++; tune_hunt(t); }
        break;
    case T3_ST_CAMPED:
        if (now - t->last_cc_rx > CC_LOSS_MS && now - t->state_t0 > CC_LOSS_MS) {
            t3_event_t ev;
            ev0(&ev, T3_EV_LOST_CC);
            ev.lpcn = t->cc_lpcn;
            t3_emit(t, &ev);
            ra_done(t);
            t->hunt_idx++;
            tune_hunt(t);
            break;
        }
        if (t->ra_active) {
            if (!t->ra_sent && t->ra_due && now >= t->ra_due) {
                if (!aloha_permits(t)) { ra_schedule(t); break; }
                if (t->cb.tx_csbk) t->cb.tx_csbk(t->cb.user, &t->ra_csbk, t->cfg.tscc_slot);
                t->ra_sent = true;
                t->ra_tries++;
                uint32_t w = nrand_wait_frames(t->aloha_seen ? t->aloha.nrand_wait : 4);
                t->ra_timeout = now + (w + 2u) * TDMA_FRAME_MS;
            } else if (t->ra_sent && now >= t->ra_timeout) {
                if (t->ra_tries >= t->cfg.nrand) {
                    t3_event_t ev;
                    ev0(&ev, t->ra_kind == T3_SK_REGISTRATION ? T3_EV_REG_FAILED : T3_EV_REQUEST_FAILED);
                    t->reg_pending = false;
                    ra_done(t);
                    t3_emit(t, &ev);
                } else {
                    t->ra_sent = false;
                    ra_schedule(t);
                }
            }
        }
        break;
    case T3_ST_TRAFFIC:
        if (now - t->last_traffic_rx > (t->voice_seen ? t->cfg.hang_ms : TRAFFIC_LOSS_MS)) return_to_cc(t);
        break;
    default:
        break;
    }
}
