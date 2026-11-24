/* Emulator-only modem backend (built with `make EMU=1`): receive symbols from the Renode symbol FIFO
 * (emu/models/SymbolSource.cs at 0x5FFF0500, `syms Load @file 4800`). It stands in for the planned
 * MCU-DSP demodulator so the protocol decoders run inside the firmware against a symbol file.
 * Never built for hardware: 0x5FFF0500 is not a peripheral on the real chip. */
#ifdef CFW_EMU
#include "modem_core.h"
#include "hal.h"
#include "dsp_backend.h"

#ifndef SYMS_BASE
#define SYMS_BASE  0x5FFF0500u   /* D168UV: 0x5FFF0600 (board_cfg.h; its I2C model sits at 0x5FFF0500) */
#endif
#define SYMS_ID    (*(volatile uint32_t *)(SYMS_BASE + 0x00))
#define SYMS_AVAIL (*(volatile uint32_t *)(SYMS_BASE + 0x04))
#define SYMS_RATE  (*(volatile uint32_t *)(SYMS_BASE + 0x0C))
#define SYMS_BULK  (*(volatile uint32_t *)(SYMS_BASE + 0x14))
#define SYMS_MAGIC 0x534D5953u                          /* 'SYMS' */

static bool s_rx;

static bool es_supports(const cfw_air_t *a) { (void)a; return SYMS_ID == SYMS_MAGIC; }
static void es_start_rx(const cfw_air_t *a, uint32_t hz) { rf_set_freq(0, hz); s_rx = true; dspb_start(a); }
static void es_start_tx(const cfw_air_t *a, uint32_t hz) { s_rx = false; dspb_tx_start(a, hz); }
static void es_stop(void) { s_rx = false; dspb_stop(); }

/* A file loaded at >= 20000/s is not symbols but 8-bit discriminator samples (ADC count >> 4, 128 = 0 Hz,
 * made by cfw/dsp `make waves`): they go through the MCU-DSP demodulator like ADC1 samples would. */
static void es_poll(void)
{
    if(dspb_tx_active()) { dspb_tx_poll(); dspb_poll_load(); return; }
    if(!s_rx) { return; }
    static uint8_t buf[1024];
    static uint16_t adc[1024];
    bool samples = SYMS_RATE >= 20000u;
    uint32_t n = SYMS_AVAIL;
    if(n > sizeof buf) { n = sizeof buf; }               /* the rest next pass */
    uint32_t got = 0;
    while(got < n) {
        uint32_t k = n - got >= 4 ? 4 : n - got;
        uint32_t w = SYMS_BULK;                          /* pops min(4, due) symbols */
        for(uint32_t i = 0; i < k; i++) { buf[got++] = (uint8_t)(w >> (8 * i)); }
    }
    if(!got) { dspb_poll_load(); return; }
    if(samples) {
        for(uint32_t i = 0; i < got; i++) { adc[i] = (uint16_t)(buf[i] << 4); }
        dspb_feed(adc, got, 1);
        dspb_poll_load();
    } else {
        modem_rx_push(buf, NULL, got);
    }
}

const cfw_modem_backend_t g_backend_emusyms = {
    "emu-syms", es_supports, es_start_rx, es_start_tx, es_stop, es_poll,
};
#endif
