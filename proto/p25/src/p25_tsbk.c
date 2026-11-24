/*
 * p25_tsbk.c - TSBK / MAC message field layouts (TIA-102.AABC-E, BBAD).
 * Argument bit offsets below count from the first argument bit (TSBK octet 2).
 */
#include "p25_tsbk.h"
#include "p25_fec.h"
#include <string.h>

#define F(off, n) p25_bits_get(a, (off), (n))

static const uint8_t  tdma_slots[16] = { 1, 1, 1, 2, 4, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint16_t tdma_bw_khz10[16] = { 625, 1250, 625, 1250, 2500, 1250, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

void p25_osp_parse(uint8_t opcode, uint8_t mfid, const uint8_t a[8], p25_tsbk_t *t)
{
    memset(&t->u, 0, sizeof t->u);
    t->opcode = opcode & 0x3F;
    t->mfid = mfid;
    t->known = false;
    memcpy(t->args, a, 8);
    if (mfid != 0x00 && mfid != 0x01) return;     /* standard messages only */
    t->known = true;
    switch (t->opcode) {
    case P25_OSP_GRP_V_CH_GRANT:
        t->u.grant.svc = (uint8_t)F(0, 8);
        t->u.grant.ch = (uint16_t)F(8, 16);
        t->u.grant.grp = (uint16_t)F(24, 16);
        t->u.grant.src = F(40, 24);
        break;
    case P25_OSP_GRP_V_CH_GRANT_UPDT:
        t->u.grant_updt.ch[0] = (uint16_t)F(0, 16);
        t->u.grant_updt.grp[0] = (uint16_t)F(16, 16);
        t->u.grant_updt.ch[1] = (uint16_t)F(32, 16);
        t->u.grant_updt.grp[1] = (uint16_t)F(48, 16);
        t->u.grant_updt.n = 2;
        break;
    case P25_OSP_GRP_V_CH_GRANT_UPDT_EXP:
        t->u.grant_updt.ch[0] = (uint16_t)F(16, 16);      /* transmit (outbound) channel */
        t->u.grant_updt.grp[0] = (uint16_t)F(48, 16);
        t->u.grant_updt.n = 1;
        break;
    case P25_OSP_UU_V_CH_GRANT:
    case P25_OSP_UU_V_CH_GRANT_UPDT:
        t->u.uu_grant.ch = (uint16_t)F(0, 16);
        t->u.uu_grant.dst = F(16, 24);
        t->u.uu_grant.src = F(40, 24);
        break;
    case P25_OSP_IDEN_UP: {
        p25_iden_t *e = &t->u.iden.e;
        t->u.iden.iden = (uint8_t)F(0, 4);
        e->valid = true;
        e->slots = 1;
        e->bw_hz = F(4, 9) * 125u;
        e->tx_offset_hz = (int32_t)(F(14, 8) * 250000u) * (F(13, 1) ? 1 : -1);
        e->spacing_hz = F(22, 10) * 125u;
        e->base_hz = F(32, 32) * 5u;
        break;
    }
    case P25_OSP_IDEN_UP_VU:
    case P25_OSP_IDEN_UP_TDMA: {
        p25_iden_t *e = &t->u.iden.e;
        uint32_t type = F(4, 4);
        t->u.iden.iden = (uint8_t)F(0, 4);
        e->valid = true;
        e->spacing_hz = F(22, 10) * 125u;
        e->tx_offset_hz = (int32_t)(F(9, 13) * e->spacing_hz) * (F(8, 1) ? 1 : -1);
        e->base_hz = F(32, 32) * 5u;
        if (t->opcode == P25_OSP_IDEN_UP_VU) {
            e->slots = 1;
            e->bw_hz = type == 4 ? 6250u : type == 5 ? 12500u : 0u;
        } else {
            e->chan_type = (uint8_t)type;
            e->slots = tdma_slots[type];
            e->tdma = e->slots > 1;
            e->bw_hz = tdma_bw_khz10[type] * 10u;
        }
        break;
    }
    case P25_OSP_RFSS_STS_BCST:
        t->u.rfss.lra = (uint8_t)F(0, 8);
        t->u.rfss.active = F(11, 1) != 0;
        t->u.rfss.sysid = (uint16_t)F(12, 12);
        t->u.rfss.rfss = (uint8_t)F(24, 8);
        t->u.rfss.site = (uint8_t)F(32, 8);
        t->u.rfss.ch = (uint16_t)F(40, 16);
        t->u.rfss.ssc = (uint8_t)F(56, 8);
        break;
    case P25_OSP_NET_STS_BCST:
        t->u.net.lra = (uint8_t)F(0, 8);
        t->u.net.wacn = F(8, 20);
        t->u.net.sysid = (uint16_t)F(28, 12);
        t->u.net.ch = (uint16_t)F(40, 16);
        t->u.net.ssc = (uint8_t)F(56, 8);
        break;
    case P25_OSP_ADJ_STS_BCST:
        t->u.adj.lra = (uint8_t)F(0, 8);
        t->u.adj.cfva = (uint8_t)F(8, 4);
        t->u.adj.sysid = (uint16_t)F(12, 12);
        t->u.adj.rfss = (uint8_t)F(24, 8);
        t->u.adj.site = (uint8_t)F(32, 8);
        t->u.adj.ch = (uint16_t)F(40, 16);
        t->u.adj.ssc = (uint8_t)F(56, 8);
        break;
    case P25_OSP_GRP_AFF_RSP:
        t->u.aff_rsp.lg = F(0, 1) != 0;
        t->u.aff_rsp.gav = (uint8_t)F(6, 2);
        t->u.aff_rsp.aga = (uint16_t)F(8, 16);
        t->u.aff_rsp.ga = (uint16_t)F(24, 16);
        t->u.aff_rsp.ta = F(40, 24);
        break;
    case P25_OSP_U_REG_RSP:
        t->u.reg_rsp.rv = (uint8_t)F(2, 2);
        t->u.reg_rsp.sysid = (uint16_t)F(4, 12);
        t->u.reg_rsp.suid = F(16, 24);
        t->u.reg_rsp.sua = F(40, 24);
        break;
    case P25_OSP_DENY_RSP:
        t->u.deny.aiv_svc = (uint8_t)F(2, 6);
        t->u.deny.reason = (uint8_t)F(8, 8);
        t->u.deny.target = F(40, 24);
        break;
    default:
        t->known = false;
        break;
    }
}

void p25_tsbk_parse(const uint8_t b[12], p25_tsbk_t *t)
{
    p25_osp_parse(b[0] & 0x3F, b[1], b + 2, t);
    t->last_block = (b[0] & 0x80) != 0;
    t->protect = (b[0] & 0x40) != 0;
}

bool p25_chan_to_freq(const p25_iden_t tab[16], uint16_t ch, uint32_t *rx_hz, uint32_t *tx_hz, uint8_t *slot,
                      bool *tdma)
{
    const p25_iden_t *e = &tab[ch >> 12];
    uint32_t num = ch & 0xFFFu;
    if (!e->valid) return false;
    uint32_t slots = e->slots ? e->slots : 1;
    uint32_t f = e->base_hz + e->spacing_hz * (num / slots);
    if (rx_hz) *rx_hz = f;
    if (tx_hz) *tx_hz = (uint32_t)((int32_t)f + e->tx_offset_hz);
    if (slot) *slot = (uint8_t)(num % slots);
    if (tdma) *tdma = e->tdma;
    return true;
}

/* ------------------------------------------------------------- builders */
static void hdr(uint8_t *o, uint8_t op)
{
    memset(o, 0, 10);
    o[0] = op & 0x3F;
    o[1] = 0x00;                 /* standard MFID */
}
#define P(off, n, v) p25_bits_put(o + 2, (off), (n), (uint32_t)(v))

void p25_isp_u_reg_req(uint8_t o[10], uint32_t wacn, uint16_t sysid, uint32_t suid)
{
    hdr(o, P25_ISP_U_REG_REQ);
    P(8, 20, wacn); P(28, 12, sysid); P(40, 24, suid);
}

void p25_isp_u_de_reg_req(uint8_t o[10], uint32_t wacn, uint16_t sysid, uint32_t suid)
{
    hdr(o, P25_ISP_U_DE_REG_REQ);
    P(8, 20, wacn); P(28, 12, sysid); P(40, 24, suid);
}

void p25_isp_grp_aff_req(uint8_t o[10], uint16_t sysid, uint16_t grp, uint32_t suid)
{
    hdr(o, P25_ISP_GRP_AFF_REQ);
    P(12, 12, sysid); P(24, 16, grp); P(40, 24, suid);
}

void p25_isp_grp_v_req(uint8_t o[10], uint8_t svc, uint16_t grp, uint32_t suid)
{
    hdr(o, P25_ISP_GRP_V_REQ);
    P(0, 8, svc); P(24, 16, grp); P(40, 24, suid);
}

void p25_isp_uu_v_req(uint8_t o[10], uint8_t svc, uint32_t target, uint32_t suid)
{
    hdr(o, P25_ISP_UU_V_REQ);
    P(0, 8, svc); P(16, 24, target); P(40, 24, suid);
}

void p25_osp_grp_v_grant(uint8_t o[10], uint8_t svc, uint16_t ch, uint16_t grp, uint32_t src)
{
    hdr(o, P25_OSP_GRP_V_CH_GRANT);
    P(0, 8, svc); P(8, 16, ch); P(24, 16, grp); P(40, 24, src);
}

void p25_osp_grp_v_grant_updt(uint8_t o[10], uint16_t ch1, uint16_t grp1, uint16_t ch2, uint16_t grp2)
{
    hdr(o, P25_OSP_GRP_V_CH_GRANT_UPDT);
    P(0, 16, ch1); P(16, 16, grp1); P(32, 16, ch2); P(48, 16, grp2);
}

static void iden_common(uint8_t *o, uint8_t iden, uint8_t type, int32_t off, uint32_t spacing, uint32_t base)
{
    uint32_t sp = spacing / 125u;
    uint32_t mag = (uint32_t)(off < 0 ? -off : off) / (spacing ? spacing : 1);
    P(0, 4, iden); P(4, 4, type); P(8, 1, off >= 0); P(9, 13, mag); P(22, 10, sp); P(32, 32, base / 5u);
}

void p25_osp_iden_up_vu(uint8_t o[10], uint8_t iden, uint8_t bw_code, int32_t off, uint32_t spacing, uint32_t base)
{
    hdr(o, P25_OSP_IDEN_UP_VU);
    iden_common(o, iden, bw_code, off, spacing, base);
}

void p25_osp_iden_up_tdma(uint8_t o[10], uint8_t iden, uint8_t type, int32_t off, uint32_t spacing, uint32_t base)
{
    hdr(o, P25_OSP_IDEN_UP_TDMA);
    iden_common(o, iden, type, off, spacing, base);
}

void p25_osp_net_sts(uint8_t o[10], uint8_t lra, uint32_t wacn, uint16_t sysid, uint16_t ch, uint8_t ssc)
{
    hdr(o, P25_OSP_NET_STS_BCST);
    P(0, 8, lra); P(8, 20, wacn); P(28, 12, sysid); P(40, 16, ch); P(56, 8, ssc);
}

static void site_sts(uint8_t *o, uint8_t lra, uint16_t sysid, uint8_t rfss, uint8_t site, uint16_t ch, uint8_t ssc)
{
    P(0, 8, lra); P(12, 12, sysid); P(24, 8, rfss); P(32, 8, site); P(40, 16, ch); P(56, 8, ssc);
}

void p25_osp_rfss_sts(uint8_t o[10], uint8_t lra, uint16_t sysid, uint8_t rfss, uint8_t site, uint16_t ch, uint8_t ssc)
{
    hdr(o, P25_OSP_RFSS_STS_BCST);
    site_sts(o, lra, sysid, rfss, site, ch, ssc);
    P(11, 1, 1);
}

void p25_osp_adj_sts(uint8_t o[10], uint8_t lra, uint16_t sysid, uint8_t rfss, uint8_t site, uint16_t ch, uint8_t ssc)
{
    hdr(o, P25_OSP_ADJ_STS_BCST);
    site_sts(o, lra, sysid, rfss, site, ch, ssc);
}
