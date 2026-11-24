/*
 * p25_modem.h - the narrow interface between the P25 protocol layer and the
 * radio's symbol path. cfw/hal/hal.h has no modem API yet (2026-11-19); this
 * mirrors cfw/proto/dstar_ysf/common/dv_modem.h so one platform object can
 * serve all digital modes.
 *
 * The protocol library never calls these itself (it is pure functions + state
 * machines). Glue code does:
 *
 *   RX:  modem ISR/task -> dibits + RSSI
 *          -> p25_p1_rx_push() (FDMA)  or  p25_p2_rx_push() (TDMA)
 *          -> callbacks: IMBE 88-bit / AMBE 49-bit frames to the vocoder,
 *             events to p25_trunk_*() and the UI
 *   TX:  vocoder frames -> p25_p1_voice_tx_*() / p25_p2_build_*()
 *          -> dibit buffer -> ops->tx_write()
 *   Trunking: p25_trunk_cb_t.tune -> ops->set_freq() + start_rx(mode)
 *
 * If the hardware cannot deliver dibits, p25_c4fm.h turns FM-discriminator
 * samples into dibits (and dibits into modulator baseband).
 */
#ifndef P25_MODEM_H
#define P25_MODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P25_MOD_C4FM_4800 = 0,    /* Phase 1: 4800 sym/s, dibit 01=+3 00=+1 10=-1 11=-3 (x600 Hz) */
    P25_MOD_HDQPSK_6000 = 1,  /* Phase 2 outbound (base -> SU), 6000 sym/s */
    P25_MOD_HCPM_6000 = 2,    /* Phase 2 inbound (SU -> base), 6000 sym/s */
} p25_mod_t;

typedef struct p25_modem_ops {
    int    (*set_freq)(void *ctx, uint32_t rx_hz, uint32_t tx_hz);
    int    (*start_rx)(void *ctx, p25_mod_t mod);
    int    (*start_tx)(void *ctx, p25_mod_t mod);          /* only on licensed amateur frequencies */
    /* Queue n dibits (one per byte, first bit in bit 1); returns accepted count. */
    size_t (*tx_write)(void *ctx, const uint8_t *dibits, size_t n);
    size_t (*tx_pending)(void *ctx);
    void   (*stop)(void *ctx);
    int    (*rssi)(void *ctx);                             /* dBm, or -1 if unknown */
} p25_modem_ops_t;

#ifdef __cplusplus
}
#endif
#endif
