/* SysTick 5 ms tick (stock OS tick: HCLK/8 = 15 MHz, FUN_08062654) + microsecond busy wait.
 *
 * delay_us() also carries the symbol FUN_0811ece0 (the stock delay_us): emu/run.py finds that name in an
 * ELF and hooks it to advance virtual time instead of polling SysTick (big emulator speedup). The hook
 * returns straight to LR, so delay_us must stay a real, non-inlined function taking us in r0. */
#include "stm32u5.h"
#include "hal.h"

volatile uint32_t g_ticks;

#define SYSTICK_HZ   15000000u                 /* HCLK / 8 */
#define TICK_RELOAD  (SYSTICK_HZ / 1000u * HAL_TICK_MS)

void time_init(void)
{
    SYST_CSR = 0;
    SYST_RVR = TICK_RELOAD - 1;
    SYST_CVR = 0;
    SYST_CSR = (1u << 1) | (1u << 0);          /* TICKINT | ENABLE, CLKSOURCE = 0 (HCLK/8 via SYSTICKSEL) */
}

void SysTick_Handler(void)
{
    g_ticks++;
    safety_systick();                           /* F2: TX hard cap / main-loop stall (hal/safety.c) */
}

uint32_t hal_micros(void)
{
    uint32_t t1, v;
    do { t1 = g_ticks; v = SYST_CVR; } while(t1 != g_ticks);
    return t1 * (HAL_TICK_MS * 1000u) + (TICK_RELOAD - 1u - v) / (SYSTICK_HZ / 1000000u);
}

uint32_t hal_millis(void)
{
    return g_ticks * HAL_TICK_MS;
}

__attribute__((noinline, used)) void delay_us(uint32_t us)
{
    /* Count SysTick down-counts: 15 counts per us. Works across reloads. */
    uint32_t left = us * (SYSTICK_HZ / 1000000u);
    uint32_t last = SYST_CVR;
    while(left) {
        uint32_t now = SYST_CVR;
        uint32_t d = (now <= last) ? (last - now) : (last + TICK_RELOAD - now);
        if(d >= left) { break; }
        left -= d;
        last = now;
    }
}
void FUN_0811ece0(uint32_t us) __attribute__((alias("delay_us")));

void delay_ms(uint32_t ms)
{
    while(ms--) { delay_us(1000); wdg_refresh(); }     /* F3: long blocking waits keep the watchdog fed */
}
