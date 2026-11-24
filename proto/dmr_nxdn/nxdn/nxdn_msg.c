/* NXDN layer 3 message fields (TS 1-A 6.4, 6.5). Only the elements the trunking/call logic needs;
 * every message is also delivered raw. */
#include "nxdn.h"
#include "../common/bits.h"
#include <string.h>

void nxdn_msg_parse_call(const uint8_t *m, nxdn_call_t *c)
{
    memset(c, 0, sizeof *c);
    c->type = m[0] & 0x3Fu;
    c->cc_option = m[1];
    c->call_type = m[2] >> 5;
    c->call_option = m[2] & 0x1Fu;
    c->src = (uint16_t)((m[3] << 8) | m[4]);
    c->dst = (uint16_t)((m[5] << 8) | m[6]);
    if (c->type == NXDN_MSG_VCALL || c->type == NXDN_MSG_VCALL_IV) {
        c->cipher = m[7] >> 6;
        c->key_id = m[7] & 0x3Fu;
    } else if (c->type == NXDN_MSG_VCALL_ASSGN || c->type == NXDN_MSG_VCALL_ASSGN_DUP ||
               c->type == NXDN_MSG_DCALL_ASSGN || c->type == NXDN_MSG_DCALL_ASSGN_DUP) {
        c->call_timer = m[7] >> 2;
        c->channel = (uint16_t)(((m[7] & 3u) << 8) | m[8]);
    }
}

unsigned nxdn_msg_build_vcall(uint8_t *m, uint8_t type, uint8_t call_type, uint16_t src, uint16_t dst)
{
    memset(m, 0, 10);
    m[0] = type & 0x3Fu;
    m[1] = 0;
    m[2] = (uint8_t)(call_type << 5);              /* voice call option 0: half duplex, 4800/EHR */
    m[3] = (uint8_t)(src >> 8); m[4] = (uint8_t)src;
    m[5] = (uint8_t)(dst >> 8); m[6] = (uint8_t)dst;
    return type == NXDN_MSG_TX_REL ? 7u : 8u;
}

unsigned nxdn_msg_build_assgn(uint8_t *m, uint8_t call_type, uint16_t src, uint16_t dst, uint16_t channel)
{
    nxdn_msg_build_vcall(m, NXDN_MSG_VCALL_ASSGN, call_type, src, dst);
    m[7] = (uint8_t)((channel >> 8) & 3u);         /* call timer 0 */
    m[8] = (uint8_t)channel;
    return 9;
}

void nxdn_locid_parse(uint32_t loc, nxdn_locid_t *l)
{
    l->category = (uint8_t)(loc >> 22);
    unsigned sitebits = l->category == 0 ? 12u : l->category == 2 ? 8u : 5u;   /* global / regional / local */
    l->site = (uint16_t)(loc & ((1u << sitebits) - 1u));
    l->system = (loc & 0x3FFFFFu) >> sitebits;
}

static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

void nxdn_msg_parse_site_info(const uint8_t *m, nxdn_site_info_t *s)
{
    s->loc = be24(m + 1);
    nxdn_locid_parse(s->loc, &s->lid);
    s->chan_struct = (uint16_t)((m[4] << 8) | m[5]);
    s->service = (uint16_t)((m[6] << 8) | m[7]);
    s->restriction = be24(m + 8);
    s->access = be24(m + 11);
    s->version = m[14];
    /* octets 15..17: adjacent site allocation (4), 1st control channel (10), 2nd control channel (10) */
    s->adj_alloc = (uint8_t)bits_field(m, 120, 4);
    s->cc1 = (uint16_t)bits_field(m, 124, 10);
    s->cc2 = (uint16_t)bits_field(m, 134, 10);
}

unsigned nxdn_msg_build_site_info(uint8_t *m, const nxdn_site_info_t *s)
{
    memset(m, 0, 18);
    m[0] = NXDN_MSG_SITE_INFO;
    m[1] = (uint8_t)(s->loc >> 16); m[2] = (uint8_t)(s->loc >> 8); m[3] = (uint8_t)s->loc;
    m[4] = (uint8_t)(s->chan_struct >> 8); m[5] = (uint8_t)s->chan_struct;
    m[6] = (uint8_t)(s->service >> 8); m[7] = (uint8_t)s->service;
    m[8] = (uint8_t)(s->restriction >> 16); m[9] = (uint8_t)(s->restriction >> 8); m[10] = (uint8_t)s->restriction;
    m[11] = (uint8_t)(s->access >> 16); m[12] = (uint8_t)(s->access >> 8); m[13] = (uint8_t)s->access;
    m[14] = s->version;
    bits_set_field(m, 120, 4, s->adj_alloc);
    bits_set_field(m, 124, 10, s->cc1);
    bits_set_field(m, 134, 10, s->cc2);
    return 18;
}

void nxdn_msg_parse_cch_info(const uint8_t *m, nxdn_cch_info_t *c)
{
    c->loc = be24(m + 1);
    c->curr = m[4] >> 7;
    c->new_ = (m[4] >> 6) & 1u;
    c->add = (m[4] >> 5) & 1u;
    c->del = (m[4] >> 4) & 1u;
    c->cc1 = (uint16_t)(((m[4] & 3u) << 8) | m[5]);
    c->cc2 = (uint16_t)(((m[6] & 3u) << 8) | m[7]);
}

/* REG_REQ / REG_RESP (figures 6.4-44/45): octet 1..3 = option (5) + home location ID without the
 * site code (category first, padded), unit ID octets 4-5, group ID 6-7, RESP cause (MM) octet 8,
 * REQ subscriber type octets 8-9 and version octet 10 (checked against the rendered figures). (Field labels sit on the last octet of each
 * field, as in VCALL where the source ID label is on octet 4 for octets 3-4.) */
void nxdn_msg_parse_reg(const uint8_t *m, nxdn_reg_t *r)
{
    r->option = m[1] >> 3;
    r->loc = bits_field(m, 13, 19);
    r->unit = (uint16_t)((m[4] << 8) | m[5]);
    r->group = (uint16_t)((m[6] << 8) | m[7]);
    r->cause = m[8];
}

unsigned nxdn_msg_build_reg_req(uint8_t *m, uint8_t loc_category, uint32_t system, uint16_t unit, uint16_t group)
{
    memset(m, 0, 12);
    m[0] = NXDN_MSG_REG;
    unsigned sysbits = loc_category == 0 ? 10u : loc_category == 2 ? 14u : 17u;
    bits_set_field(m, 8, 5, 0);                   /* registration option: normal, home */
    bits_set_field(m, 13, 2, loc_category);
    bits_set_field(m, 15, sysbits, system);
    m[4] = (uint8_t)(unit >> 8); m[5] = (uint8_t)unit;
    m[6] = (uint8_t)(group >> 8); m[7] = (uint8_t)group;
    m[9] = 0;                                     /* subscriber type */
    m[10] = 0;                                    /* version */
    return 11;
}

unsigned nxdn_msg_build_grp_reg_req(uint8_t *m, uint16_t unit, uint16_t group)
{
    memset(m, 0, 8);
    m[0] = NXDN_MSG_GRP_REG;
    m[1] = 0;                                     /* group registration option */
    m[2] = (uint8_t)(unit >> 8); m[3] = (uint8_t)unit;
    m[4] = (uint8_t)(group >> 8); m[5] = (uint8_t)group;
    return 6;
}

unsigned nxdn_msg_build_vcall_req(uint8_t *m, uint8_t call_type, uint16_t src, uint16_t dst)
{
    nxdn_msg_build_vcall(m, NXDN_MSG_VCALL_REQ, call_type, src, dst);
    return 7;
}
