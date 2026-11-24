/* D890UV custom firmware: boot + cooperative main loop.
 *
 * Tasks (all in thread mode, run from the loop; interrupts only post events):
 *   every pass   baseband link (bb_poll), modem symbol plumbing (modem_poll)
 *   10 ms        modem tick, radio tick
 *   20 ms        keys + power key
 *   100 ms       UI redraw (diffed, only changed pixels go to the LCD)
 * Idle: WFI until the next SysTick (5 ms) or encoder interrupt. */
#include <string.h>
#include "stm32u5.h"
#include "hal.h"
#include "board.h"
#include "gfx.h"
#include "modem_core.h"
#include "radio.h"
#include "cfwcp.h"
#include "ui.h"
#include "prog.h"
#include "voice.h"
#include "dsp_backend.h"
#include "dbg.h"
#ifndef CFW_BUILD
#define CFW_BUILD "?"
#endif

typedef struct {
    uint32_t period_ms;
    uint32_t last;
    void (*fn)(void);
} task_t;

static void task_10ms(void) { modem_tick(); radio_tick(); voice_tick(); }
static void task_20ms(void)
{
    keys_poll();
    power_poll();
    for(int k; (k = keys_get()) != KEY_NONE; ) { ui_key(k); }
}
static void task_ui(void) { ui_draw(); }

/* debug: longest main-loop pass (us); the ADC/DAC rings hold 80 ms */
volatile uint32_t g_dbg_max_pass_us;

static task_t s_tasks[] = {
    { 10, 0, task_10ms },
    { 20, 0, task_20ms },
    { 100, 0, task_ui },
};

static void boot_checks(void)
{
    g_boot.nand_ok = nand_init();
    if(g_boot.nand_ok) {
        static const char mark[] = "CHECKING DATA FOR D890UV";
        char buf[sizeof mark];
        g_boot.nand_marker = nand_read(0x04EC0000u, buf, sizeof mark - 1) && memcmp(buf, mark, sizeof mark - 1) == 0;
        (void)dac_cal_load();   /* APC + tune tables from the stock RF calibration block (docs/AUDIO_PATHS.md) */
    } else {
        cal_report(0, 0, 0, false, false);   /* no NAND: no calibration, TX off */
    }
    g_radio.rf_ok[0] = rf_init(0);
    g_radio.rf_ok[1] = rf_init(1);
    rf_idle(1);
    ui_draw();
    cfwcp_recover();            /* finish a codeplug journal rewrite cut by power loss */
    channels_init();            /* NAND codeplug channel scan */
}

int main(void)
{
    gpio_init_board();          /* PC3 power latch on first thing (F4: before the clocks, the HSE wait can be long) */
    power_hold();
    clock_init();               /* F4/F9: off the bootloader's PLL first; HSE/PLL failure -> internal clock, g_clk_err */
    time_init();
    safety_init();              /* fault record from before the last reset (hal/safety.c) */
    dbg_init();                 /* RTT log, DEBUG=1 builds only (no-op otherwise) */
    DBG("cfw D890UV boot, build %s%s\n", CFW_BUILD, g_clk_err ? ", CLK ERR" : "");
    __asm volatile("cpsie i");

    lcd_init();
    keys_init();
    encoder_init();
    battery_init();
    ui_init();
    {
        char f1[20], f2[20];
        if(safety_boot_fault(f1, f2)) { ui_boot_fault(f1, f2); }
    }
    ui_draw();                  /* boot screen while the checks run */

    boot_checks();
    bool rf0 = g_radio.rf_ok[0], rf1 = g_radio.rf_ok[1];
    radio_init();               /* needs the channel list (codeplug) */
    g_radio.rf_ok[0] = rf0;
    g_radio.rf_ok[1] = rf1;
    bb_init();
    voice_init();
    modem_init();
    radio_apply();
    power_set_save_hook(radio_save);
    radio_boot_ptt_lock();      /* PTT held at power-on / after a fault reset: no TX until it is released */
    prog_init();                /* CPS link on USART1 (codeplug only) */

    for(;;) {
        uint32_t t_pass = hal_micros();
        safety_main_alive();                /* F2 stall watch + F3 watchdog refresh */
        bb_poll();
        prog_poll();
        modem_poll();
        dspb_fm_poll();
        voice_poll();
        uint32_t now = hal_millis();
        bool ran = false;
        for(unsigned i = 0; i < sizeof s_tasks / sizeof s_tasks[0]; i++) {
            if(now - s_tasks[i].last >= s_tasks[i].period_ms) {
                s_tasks[i].last = now;
                s_tasks[i].fn();
                ran = true;
                modem_poll();               /* keep the DAC / ADC rings serviced between long tasks */
                dspb_fm_poll();
            }
        }
        uint32_t dt = hal_micros() - t_pass;
        if(dt > g_dbg_max_pass_us) { g_dbg_max_pass_us = dt; }
        if(!ran && !modem_transmitting()) { wfi(); }
    }
}
