/* LIS2DW12 accelerometer (I2C 0x19, both radios) for man down, as the stock firmware drives it:
 *   detect: WHO_AM_I 0x0F = 0x44 ('D'; stock D168 FUN_0803d5xx retries until it reads it, D890 the same device on
 *           the same bus: FUN_080584d4(0x32 / 0x33))
 *   set-up: stock D168 FUN_0803e148: CTRL2 0x21 = 04, CTRL1 0x20 = 94, CTRL3 0x22 = 10, CTRL4 0x23 = 80 (6D on INT1,
 *           not wired / not used), CTRL5 0x24 = 0F, CTRL6 0x25 = 04, FIFO 0x2E = 00, TAP_THS_X 0x30 = C0 (6D threshold
 *           60 deg, 4D off), 0x31..0x36 = 00, CTRL7 0x3F = 20 (interrupts enabled)
 *   poll:   STATUS 0x27 bit0 (DRDY), then SIXD_SRC 0x3A bit0 = XL: X axis below -60 deg = radio standing upright
 *           (stock D168 0x08028A20, D890 FUN_08037314 / FUN_080373dc). */
#include "hal.h"

#define ACC 0x19
static bool s_ok;

__attribute__((weak)) bool i2c_reg_write(uint8_t a7, uint8_t reg, uint8_t v) { (void)a7; (void)reg; (void)v; return false; }
__attribute__((weak)) bool i2c_reg_read(uint8_t a7, uint8_t reg, uint8_t *v) { (void)a7; (void)reg; (void)v; return false; }

bool accel_init(void)
{
    static const uint8_t k[][2] = {
        { 0x21, 0x04 }, { 0x20, 0x94 }, { 0x22, 0x10 }, { 0x23, 0x80 }, { 0x24, 0x0F }, { 0x25, 0x04 }, { 0x2E, 0x00 },
        { 0x30, 0xC0 }, { 0x31, 0x00 }, { 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x00 }, { 0x35, 0x00 }, { 0x36, 0x00 },
        { 0x3F, 0x20 },
    };
    uint8_t id = 0;
    s_ok = false;
    for(int t = 0; t < 3 && !s_ok; t++) { s_ok = i2c_reg_read(ACC, 0x0F, &id) && id == 0x44; }
    if(!s_ok) { return false; }
    for(unsigned i = 0; i < sizeof k / sizeof k[0]; i++) { (void)i2c_reg_write(ACC, k[i][0], k[i][1]); }
    return true;
}

int accel_upright(void)
{
    uint8_t st = 0, sixd = 0;
    if(!s_ok || !i2c_reg_read(ACC, 0x27, &st) || !(st & 1)) { return -1; }
    if(!i2c_reg_read(ACC, 0x3A, &sixd)) { return -1; }
    return (sixd & 1) ? 1 : 0;
}
