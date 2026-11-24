/* RTC (STM32U5 @0x46007800, battery backed, set up by the stock firmware: LSE, 1 Hz calendar) + LCD backlight.
 *
 * RTC: the cfw only reads the calendar and writes it from the radio menu (Settings > Date / time); it does not touch
 * the RTC clock source or prescalers. Write: PWR_DBPR.DBP, write protection keys, INIT mode, TR/DR, leave INIT.
 * Not verified on a real radio (emulator: emu/models/Rtc.cs).
 * Backlight: PB0 = TIM3_CH3 PWM, ARR 0xFFF, PSC 3, duty levels as stock (docs/AUDIO_PATHS.md, table 0x0814A90A). */
#include "hal.h"
#include "stm32u5.h"

#define RTC_BASE   0x46007800u
#define RTC_TR     REG32(RTC_BASE + 0x00)
#define RTC_DR     REG32(RTC_BASE + 0x04)
#define RTC_ICSR   REG32(RTC_BASE + 0x0C)
#define RTC_WPR    REG32(RTC_BASE + 0x24)
#define PWR_DBPR   REG32(PWR_BASE + 0x28)

static uint32_t bcd(uint32_t v) { return (v >> 4) * 10u + (v & 15u); }
static uint32_t tobcd(uint32_t v) { return ((v / 10u) << 4) | (v % 10u); }

static void clocks(void)
{
    RCC_AHB3ENR |= 1u << 2;                     /* PWR */
    RCC_APB3ENR |= 1u << 21;                    /* RTCAPB */
}

uint32_t rtc_now(void)
{
    clocks();
    uint32_t tr = RTC_TR, dr = RTC_DR;
    if(RTC_TR != tr) { tr = RTC_TR; dr = RTC_DR; }
    int y = 2000 + (int)bcd(dr >> 16 & 0xFF), m = (int)bcd(dr >> 8 & 0x1F), d = (int)bcd(dr & 0x3F);
    if(m < 1 || m > 12 || d < 1) { return 0; }
    return cal_days(y, m, d) * 86400u + bcd(tr >> 16 & 0x3F) * 3600u + bcd(tr >> 8 & 0x7F) * 60u + bcd(tr & 0x7F);
}

void rtc_set(uint32_t t)
{
    int y, m, d;
    cal_date(t / 86400u, &y, &m, &d);
    uint32_t s = t % 86400u, wd = (t / 86400u + 3u) % 7u + 1u;   /* 1 = Monday */
    clocks();
    PWR_DBPR = 1;
    RTC_WPR = 0xCA; RTC_WPR = 0x53;
    RTC_ICSR |= 1u << 7;                                          /* INIT */
    for(int n = 0; n < 100000 && !(RTC_ICSR & (1u << 6)); n++) { }
    RTC_TR = tobcd(s / 3600u) << 16 | tobcd(s / 60u % 60u) << 8 | tobcd(s % 60u);
    RTC_DR = tobcd((uint32_t)(y - 2000) % 100u) << 16 | wd << 13 | tobcd((uint32_t)m) << 8 | tobcd((uint32_t)d);
    RTC_ICSR &= ~(1u << 7);
    RTC_WPR = 0xFF;
}

/* ---- backlight ---- */
#define TIM3_BASE 0x40000400u
static const uint16_t k_bl[5] = { 2550, 2750, 2850, 2950, 4095 };
static bool s_bl_init;

void backlight_set(uint8_t level)
{
    if(!s_bl_init) {
        RCC_APB1ENR1 |= 1u << 1;                                  /* TIM3 */
        gpio_af(GPIOB, 0, 2);
        gpio_mode(GPIOB, 0, GPIO_AF, PULL_NONE);
        REG32(TIM3_BASE + 0x28) = 3;                              /* PSC */
        REG32(TIM3_BASE + 0x2C) = 0xFFF;                          /* ARR */
        REG32(TIM3_BASE + 0x1C) = (REG32(TIM3_BASE + 0x1C) & ~0xFFu) | 0x68u;   /* CCMR2: OC3 PWM1 + preload */
        REG32(TIM3_BASE + 0x20) |= 1u << 8;                       /* CCER CC3E */
        REG32(TIM3_BASE + 0x00) |= 1u;                            /* CEN */
        s_bl_init = true;
    }
    REG32(TIM3_BASE + 0x3C) = level ? k_bl[(level - 1) % 5] : 0;  /* CCR3 */
}

static uint8_t s_power;
void dac_set_power(uint8_t level) { s_power = level & 3u; }
uint8_t dac_power(void) { return s_power; }
