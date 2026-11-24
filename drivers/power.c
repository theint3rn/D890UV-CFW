/* Power: PC3 latch + PA0 power key / volume-knob switch.
 * Stock: PA0 high = pressed. At power-on the key is still held; once released, PA0 high again (knob
 * clicked off) is a power-off request: FUN_080d6954 / FUN_08057db0 save state, drop the PC3 latch, then
 * reset through AIRCR (FUN_0813b98c) in case power stays (USB / programming cable). */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

static void (*s_save)(void);
static bool s_armed;          /* power key seen released since boot */
static uint8_t s_high;

void power_hold(void) { gpio_set(PIN_PWR_LATCH_PORT, PIN_PWR_LATCH); }

void power_set_save_hook(void (*fn)(void)) { s_save = fn; }

static bool key_high(void) { return gpio_read(PIN_PWR_KEY_PORT, PIN_PWR_KEY); }

void power_off(void)
{
    if(s_save) { s_save(); }
    lcd_on(false);
    gpio_clr(PIN_PWR_LATCH_PORT, PIN_PWR_LATCH);
    delay_ms(200);                                     /* real radio: power is gone by now */
    dsb();
    SCB_AIRCR = (0x5FAu << 16) | (1u << 2);            /* SYSRESETREQ, as stock FUN_0813b98c */
    for(;;) { }
}

/* PA0 still high 3 s after reset = the knob is in the off position (the power-on press is shorter;
 * the emulator board releases it after 1 s): turn off again, as stock does. */
#define BOOT_PRESS_MAX_MS 3000u

void power_poll(void)
{
    if(!key_high()) { s_armed = true; s_high = 0; return; }
    if(!s_armed) {
        if(hal_millis() > BOOT_PRESS_MAX_MS) { power_off(); }
        return;
    }
    if(++s_high >= 5) { power_off(); }                 /* ~100 ms of PA0 high (20 ms polls) */
}

bool power_armed(void) { return s_armed; }
