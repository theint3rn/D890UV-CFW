/* Clocks: HSE 12 MHz -> PLL1 (M=1, N=20, R=2) -> 120 MHz SYSCLK = HCLK = PCLKx.
 * Same result as the stock clock setup (FUN_08062654: HCLK 120 MHz, SystemCoreClock 0x20000ff4),
 * written directly against RM0456 instead of the ST HAL. */
#include "stm32u5.h"
#include "hal.h"

uint32_t SystemCoreClock = 4000000u;   /* MSIS 4 MHz after reset */

static bool wait_set(volatile uint32_t *r, uint32_t mask)
{
    for(uint32_t n = 0; n < 2000000u; n++) { if((*r & mask) == mask) { return true; } }
    return false;
}

static bool wait_clr(volatile uint32_t *r, uint32_t mask)
{
    for(uint32_t n = 0; n < 2000000u; n++) { if((*r & mask) == 0) { return true; } }
    return false;
}

static bool wait_sws(uint32_t sw)
{
    for(uint32_t n = 0; n < 2000000u; n++) { if(((RCC_CFGR1 >> 2) & 3u) == sw) { return true; } }
    return false;
}

/* Pre-flash audit F4 / F9: the bootloader may hand over with PLL1 running as SYSCLK, so SYSCLK goes to HSI16 first
 * (PLL1 must not be reconfigured while it is the system clock), then PLL1 off -> configured -> on -> SYSCLK.
 * If HSE or PLL1 does not come up, SYSCLK stays on HSI16 (16 MHz), g_clk_err = 1: the UI shows "CLK ERR" and TX is
 * refused (hal/safety.c); SysTick-based times then run 7.5x slow, UART baud rates are wrong. */
void clock_init(void)
{
    g_clk_err = 0;
    RCC_AHB3ENR |= 1u << 2;                         /* PWREN */
    (void)RCC_AHB3ENR;
    /* Range 1 (VOS = 11) + EPOD boost: required for SYSCLK > 55 MHz from the PLL. */
    PWR_VOSR = (PWR_VOSR & ~(3u << 16)) | (3u << 16) | (1u << 18);
    bool ok = wait_set(&PWR_VOSR, (1u << 15) | (1u << 14));   /* VOSRDY, BOOSTRDY */

    /* Flash: 3 wait states (range 1, <= 128 MHz) + prefetch, before speeding up (also fine at 16 MHz). */
    FLASH_ACR = (FLASH_ACR & ~0xFu) | 3u | (1u << 8);

    RCC_CR |= 1u << 8;                              /* HSION */
    wait_set(&RCC_CR, 1u << 10);                    /* HSIRDY */
    RCC_CFGR2 = 0;                                  /* AHB /1, APB1 /1, APB2 /1 */
    RCC_CFGR3 = 0;                                  /* APB3 /1 */
    RCC_CFGR1 = (RCC_CFGR1 & ~3u) | 1u;             /* SW = HSI16 */
    wait_sws(1u);
    SystemCoreClock = 16000000u;
    RCC_CR &= ~(1u << 24);                          /* PLL1 off while configuring */
    ok = wait_clr(&RCC_CR, 1u << 25) && ok;         /* PLL1RDY low */

    RCC_CR |= 1u << 16;                             /* HSEON (12 MHz crystal) */
    ok = ok && wait_set(&RCC_CR, 1u << 17);         /* HSERDY */
    if(ok) {
        /* PLL1SRC = HSE (3), PLL1RGE = 8-16 MHz (3), PLL1M = 1 (0), MBOOST /1, PLL1REN */
        RCC_PLL1CFGR = 3u | (3u << 2) | (0u << 8) | (0u << 12) | (1u << 18);
        /* N = 20 (VCO 240 MHz), P = 2, Q = 2, R = 2 -> 120 MHz */
        RCC_PLL1DIVR = (20u - 1) | ((2u - 1) << 9) | ((2u - 1) << 16) | ((2u - 1) << 24);
        RCC_CR |= 1u << 24;                         /* PLL1ON */
        ok = wait_set(&RCC_CR, 1u << 25);           /* PLL1RDY */
    }
    if(ok) {
        RCC_CFGR1 = (RCC_CFGR1 & ~3u) | 3u;         /* SW = PLL1 */
        ok = wait_sws(3u);
        if(!ok) { RCC_CFGR1 = (RCC_CFGR1 & ~3u) | 1u; wait_sws(1u); }
    }
    if(ok) { SystemCoreClock = 120000000u; }
    else { g_clk_err = 1; }

    /* Kernel clocks: SysTick = HCLK/8 (CCIPR1.SYSTICKSEL = 00, the reset value),
     * ADC1 = HSE 12 MHz (CCIPR3.ADCDACSEL = 011); without HSE: HCLK (000). */
    RCC_CCIPR1 &= ~(3u << 22);
    RCC_CCIPR3 = (RCC_CCIPR3 & ~(7u << 12)) | (ok ? (3u << 12) : 0u);
}
