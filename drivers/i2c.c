/* Bit-banged I2C bus of the D890UV: SDA PB6, SCL PB7 (stock V1.05 FUN_080585b4 start: PB6 = 1, PB7 = 1, PB6 = 0,
 * PB7 = 0; FUN_080583ac stop; FUN_080584d4 byte write; edges 2 us apart, FUN_0811ed70(2)). Devices (stock addresses
 * in FUN_080584d4 calls): 0x2E (8-bit 0x5C) speaker volume potentiometer (drivers/audio.c), 0x19 (0x32 / 0x33)
 * LIS2DW12 accelerometer (hal/accel.c), 0x35 (0x6A / 0x6B) not identified.
 * Note: the cfw's CPS link (app/prog.c) uses USART1 on PB6 / PB7 in non-HW builds; on the radio these pins are this
 * bus (stock USART1 = PA9 / PA10, FUN_08063358). HW=1 builds have the link off. In the emulator the USART model does not depend on
 * the pin mode, so both work there. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

#define SDA 6
#define SCL 7

static bool s_init;
static void dly(void) { delay_us(2); }
static void pin(int p, bool v) { GPIO_BSRR(GPIOB) = v ? (1u << p) : (1u << (p + 16)); }
static bool sda_get(void) { return (GPIO_IDR(GPIOB) >> SDA) & 1; }
static void sda_out(void) { gpio_mode(GPIOB, SDA, GPIO_OUT, PULL_NONE); }
static void sda_in(void) { pin(SDA, true); gpio_mode(GPIOB, SDA, GPIO_IN, PULL_UP); }

static void init(void)
{
    if(s_init) { return; }
    RCC_AHB2ENR1 |= 1u << 1;                                  /* GPIOB */
    pin(SDA, true); pin(SCL, true);
    sda_out();
    gpio_mode(GPIOB, SCL, GPIO_OUT, PULL_NONE);
    s_init = true;
}

static void start(void)
{
    sda_out();
    pin(SDA, true); dly(); pin(SCL, true); dly();
    pin(SDA, false); dly(); pin(SCL, false); dly();
}

static void stop(void)
{
    sda_out();
    pin(SDA, false); dly(); pin(SCL, true); dly(); pin(SDA, true); dly();
}

static bool put(uint8_t b)
{
    sda_out();
    for(int i = 7; i >= 0; i--) {
        pin(SDA, (b >> i) & 1); dly();
        pin(SCL, true); dly(); pin(SCL, false);
    }
    sda_in(); dly();
    pin(SCL, true); dly();
    bool ack = !sda_get();
    pin(SCL, false); dly();
    return ack;
}

static uint8_t get(bool ack)
{
    uint8_t v = 0;
    sda_in(); dly();
    for(int i = 0; i < 8; i++) {
        dly(); pin(SCL, true); dly();
        v = (uint8_t)(v << 1 | (sda_get() ? 1 : 0));
        pin(SCL, false);
    }
    sda_out();
    pin(SDA, !ack); dly();
    pin(SCL, true); dly(); pin(SCL, false); dly();
    return v;
}

bool i2c_reg_write(uint8_t a7, uint8_t reg, uint8_t v)
{
    init();
    start();
    bool ok = put((uint8_t)(a7 << 1)) && put(reg) && put(v);
    stop();
    return ok;
}

bool i2c_reg_read(uint8_t a7, uint8_t reg, uint8_t *v)
{
    init();
    start();
    bool ok = put((uint8_t)(a7 << 1)) && put(reg);
    if(ok) { start(); ok = put((uint8_t)(a7 << 1 | 1)); }
    if(ok) { *v = get(false); }
    stop();
    return ok;
}
