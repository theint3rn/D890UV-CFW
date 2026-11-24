/*
 * dv_modem.h - the narrow interface between a digital-voice protocol layer and
 * whatever produces/consumes the over-the-air bits.
 *
 * cfw/hal/hal.h has no modem API yet (2026-11-18), so this header defines a
 * clean one. The platform implements dv_modem_ops_t; the protocol layers do
 * not call it themselves (they are pure functions + state machines), the glue
 * code does:
 *
 *   RX:  modem ISR/task -> bits (D-STAR) or dibits (YSF)
 *        -> dstar_rx_push_bits() / ysf_rx_push_dibits()
 *        -> callbacks: opaque vocoder frames + events
 *   TX:  vocoder frames -> dstar_tx_*() / ysf_tx_*() -> bit/dibit buffer
 *        -> ops->tx_write()
 *
 * If the RF chip or the SCT3288 can do GMSK/C4FM itself, the platform feeds
 * bits straight in and never touches dstar_gmsk.h / ysf_c4fm.h. Those are
 * reference software modems for the "MCU-DSP" path (discriminator on an ADC,
 * baseband to the modulator via DAC/PWM), see cfw/docs/FEASIBILITY.md.
 */
#ifndef DV_MODEM_H
#define DV_MODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DV_MOD_GMSK_4800 = 0, /* D-STAR: 4800 bit/s, BT 0.5, h 0.5, '1' = +deviation */
    DV_MOD_C4FM_4800 = 1, /* YSF: 4800 sym/s, 9600 bit/s, dibit 01=+3 00=+1 10=-1 11=-3 */
} dv_mod_t;

/* Unit of the tx_write()/RX stream: */
typedef enum {
    DV_UNIT_BIT = 0,   /* one uint8_t per bit (0/1), D-STAR air order */
    DV_UNIT_DIBIT = 1, /* one uint8_t per symbol (0..3), first bit in bit 1 */
} dv_unit_t;

typedef struct dv_modem_ops {
    /* Switch the radio path to `mod`; RX data is pushed by the platform into
     * the protocol layer from then on. Return 0 on success. */
    int (*start_rx)(void *ctx, dv_mod_t mod);
    int (*start_tx)(void *ctx, dv_mod_t mod);
    /* Queue n units for transmission; returns how many were accepted. */
    size_t (*tx_write)(void *ctx, const uint8_t *units, size_t n, dv_unit_t unit);
    /* Units still queued (to pace the producer). */
    size_t (*tx_pending)(void *ctx);
    void (*stop)(void *ctx);
    /* Optional: received signal strength, -1 if unknown. */
    int (*rssi)(void *ctx);
} dv_modem_ops_t;

#ifdef __cplusplus
}
#endif
#endif
