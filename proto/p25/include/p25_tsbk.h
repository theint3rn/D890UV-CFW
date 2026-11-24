/*
 * p25_tsbk.h - trunking signalling messages: TIA-102.AABC (trunking control
 * channel messages), TIA-102.AABB (formats), TIA-102.BBAD (Phase 2 MAC
 * messages, which reuse most OSP layouts with opcode 0x40|op).
 *
 * Outbound (OSP) messages are parsed into p25_tsbk_t. Inbound (ISP) messages
 * for registration, affiliation and voice requests are only *built* here; the
 * radio sends them via p25_p1_build_tsbk(). No encryption features.
 */
#ifndef P25_TSBK_H
#define P25_TSBK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum p25_osp {
    P25_OSP_GRP_V_CH_GRANT = 0x00,
    P25_OSP_GRP_V_CH_GRANT_UPDT = 0x02,
    P25_OSP_GRP_V_CH_GRANT_UPDT_EXP = 0x03,
    P25_OSP_UU_V_CH_GRANT = 0x04,
    P25_OSP_UU_V_CH_GRANT_UPDT = 0x06,
    P25_OSP_ACK_RSP_FNE = 0x20,
    P25_OSP_DENY_RSP = 0x27,
    P25_OSP_GRP_AFF_RSP = 0x28,
    P25_OSP_U_REG_RSP = 0x2C,
    P25_OSP_U_REG_CMD = 0x2D,
    P25_OSP_U_DE_REG_ACK = 0x2F,
    P25_OSP_IDEN_UP_TDMA = 0x33,
    P25_OSP_IDEN_UP_VU = 0x34,
    P25_OSP_SYS_SRV_BCST = 0x38,
    P25_OSP_SCCB = 0x39,
    P25_OSP_RFSS_STS_BCST = 0x3A,
    P25_OSP_NET_STS_BCST = 0x3B,
    P25_OSP_ADJ_STS_BCST = 0x3C,
    P25_OSP_IDEN_UP = 0x3D,
};

enum p25_isp {
    P25_ISP_GRP_V_REQ = 0x00,      /* group voice service request */
    P25_ISP_UU_V_REQ = 0x04,
    P25_ISP_GRP_AFF_REQ = 0x28,
    P25_ISP_U_DE_REG_REQ = 0x2B,
    P25_ISP_U_REG_REQ = 0x2C,
};

/* channel identifier table entry (from IDEN_UP / IDEN_UP_VU / IDEN_UP_TDMA) */
typedef struct {
    bool     valid;
    bool     tdma;
    uint8_t  slots;              /* logical channels per carrier (1, 2, 4) */
    uint8_t  chan_type;          /* IDEN_UP_TDMA channel type, else 0 */
    uint32_t base_hz;
    uint32_t spacing_hz;
    int32_t  tx_offset_hz;       /* SU transmit = receive + offset */
    uint32_t bw_hz;
} p25_iden_t;

typedef struct {
    uint8_t opcode;              /* 6 bits */
    uint8_t mfid;
    bool    last_block;
    bool    protect;
    bool    known;               /* parsed into u */
    uint8_t args[8];             /* raw argument bytes */
    union {
        struct { uint8_t svc; uint16_t ch; uint16_t grp; uint32_t src; } grant;        /* 0x00 */
        struct { uint16_t ch[2]; uint16_t grp[2]; uint8_t n; } grant_updt;            /* 0x02 (n=2), 0x03 (n=1) */
        struct { uint8_t svc; uint16_t ch; uint32_t dst, src; } uu_grant;             /* 0x04 / 0x06 */
        struct { uint8_t iden; p25_iden_t e; } iden;                                  /* 0x33 0x34 0x3D */
        struct { uint8_t lra; bool active; uint16_t sysid; uint8_t rfss, site; uint16_t ch; uint8_t ssc; } rfss;
        struct { uint8_t lra; uint32_t wacn; uint16_t sysid; uint16_t ch; uint8_t ssc; } net;
        struct { uint8_t lra; uint8_t cfva; uint16_t sysid; uint8_t rfss, site; uint16_t ch; uint8_t ssc; } adj;
        struct { bool lg; uint8_t gav; uint16_t aga, ga; uint32_t ta; } aff_rsp;      /* 0x28 */
        struct { uint8_t rv; uint16_t sysid; uint32_t suid, sua; } reg_rsp;           /* 0x2C */
        struct { uint8_t aiv_svc; uint8_t reason; uint32_t target; } deny;            /* 0x27 */
    } u;
} p25_tsbk_t;

/* Parses an outbound TSBK (12 bytes incl. CRC; CRC is not checked here). */
void p25_tsbk_parse(const uint8_t tsbk[12], p25_tsbk_t *out);
/* Same for an opcode + 8 argument bytes (used for Phase 2 MAC messages). */
void p25_osp_parse(uint8_t opcode, uint8_t mfid, const uint8_t args[8], p25_tsbk_t *out);

/* channel number (iden:4 | number:12) -> frequencies and TDMA slot.
 * Returns false if the iden entry is unknown. */
bool p25_chan_to_freq(const p25_iden_t table[16], uint16_t ch, uint32_t *rx_hz, uint32_t *tx_hz,
                      uint8_t *slot, bool *tdma);

/* ---- inbound (ISP) builders: write 10 bytes (opcode .. args), pass to
 * p25_p1_build_tsbk(). Field layouts per TIA-102.AABC. ---- */
void p25_isp_u_reg_req(uint8_t out[10], uint32_t wacn, uint16_t sysid, uint32_t suid);
void p25_isp_u_de_reg_req(uint8_t out[10], uint32_t wacn, uint16_t sysid, uint32_t suid);
void p25_isp_grp_aff_req(uint8_t out[10], uint16_t sysid, uint16_t grp, uint32_t suid);
void p25_isp_grp_v_req(uint8_t out[10], uint8_t svc, uint16_t grp, uint32_t suid);
void p25_isp_uu_v_req(uint8_t out[10], uint8_t svc, uint32_t target, uint32_t suid);

/* OSP builders (used by tests / a control-channel simulator) */
void p25_osp_grp_v_grant(uint8_t out[10], uint8_t svc, uint16_t ch, uint16_t grp, uint32_t src);
void p25_osp_grp_v_grant_updt(uint8_t out[10], uint16_t ch1, uint16_t grp1, uint16_t ch2, uint16_t grp2);
void p25_osp_iden_up_vu(uint8_t out[10], uint8_t iden, uint8_t bw_code, int32_t tx_offset_hz,
                        uint32_t spacing_hz, uint32_t base_hz);
void p25_osp_iden_up_tdma(uint8_t out[10], uint8_t iden, uint8_t chan_type, int32_t tx_offset_hz,
                          uint32_t spacing_hz, uint32_t base_hz);
void p25_osp_net_sts(uint8_t out[10], uint8_t lra, uint32_t wacn, uint16_t sysid, uint16_t ch, uint8_t ssc);
void p25_osp_rfss_sts(uint8_t out[10], uint8_t lra, uint16_t sysid, uint8_t rfss, uint8_t site, uint16_t ch,
                      uint8_t ssc);
void p25_osp_adj_sts(uint8_t out[10], uint8_t lra, uint16_t sysid, uint8_t rfss, uint8_t site, uint16_t ch,
                     uint8_t ssc);

#ifdef __cplusplus
}
#endif
#endif
