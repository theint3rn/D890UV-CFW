/* D890UV broadcast receiver (app/bcast.h): AM/FM receiver IC at I2C 0x35 + RF chip 1 as the AM air band down-
 * converter. Register sequences as the stock firmware V1.05 (read-modify-write like its shadow copies, which it reads
 * from the IC at boot, FUN_080df8b8):
 *   power up   FUN_080e47ac: 0x0E &= ~0x20, 1 ms, 0x76 |= 0x02, 5 ms, 0x10 &= ~0x40
 *   set-up     FUN_08060a54: 0x06 = (r & 0xF8) | 2, 0x07 = 0x9C, 0x08 = (r & 0xF0) | 8, 0x0A = 0, 0x0D &= ~0x10,
 *              0x04 = (r & 0xF8) | 0x80, wait for 0x1B bit 2, 0x66 &= ~8, 0x38 = (r & 0x8F) | 0x50, 0x62 = 0x51,
 *              0x2B = (r & 0x76) | 8, 0x2F = 0x3F, 0x7E = (r & 0x0F) | 0x20, 0x16 |= 0x10, 0x57 = 0x20,
 *              0x24 &= 0x7F, 0x41 = (r & 0xC0) | 0x10, 0x43 = (r & 0x3F) | 0x40, 0x2D &= 0x9F, 0x42 = 0x32,
 *              0x3B |= 0x80, volume (0x0F low 5 bits = 16 + level)
 *   band       FUN_081167a8: FM 0x88 &= ~0x40; AM 0x88 |= 0x40, 0x8D = 0x4C, 0x8C = (r & 0x80) | 4,
 *              0x3A = (r & 0xC0) | 0x19, 0x52 = (r & 0xC0) | 0x19, 0x0E |= 0x10
 *   tune FM    FUN_081169c0(f / 10 kHz, 0): ch = f / 50 kHz; 0x2F &= ~2; 0x87 bit 3 + 0x85 low nibble for the
 *              channels 84.45 / 103.70 / 105.60 MHz; 0x89 = ch & 0xFF; 0x88 = (r & 0x70) | (ch >> 8) | 0x80
 *   tune AM    FUN_081169c0(2925000, 1): 0x8D = (f / 100) & 0xFF, 0x8C = (r & 0x80) | ((f / 100) >> 8),
 *              0x88 |= 0x80 (the IC at the 2.925 MHz IF)
 *   power down FUN_080e4698: 0x10 |= 0x40, 5 ms, 0x76 &= ~2, 1 ms, 0x0E |= 0x20
 * AM path on RF chip 1 (stock FUN_08010c1c AM branch): reg 0x56 = 0, 0x16 = 0, 0x2F = 0x0A0F, GPIO pins of reg 0x52
 * (FUN_08019ab4: chip 1, (pin << 12) | 0x40 | value) 5, 7, 4 = 1, IC set-up + AM band + IF, frequency = air band + 2.925 MHz
 * (AM Offset positive; negative: - 2.925 MHz, FUN_0810df0c), reg 0x5C = 2, reg 0x56 = 6. Off: pins 7, 4, 5 = 0. FM: pin 5 = 1, pin 4 = 0 (FUN_08111a58).
 * Speaker: both audio switches off (stock source 4 / AM: mask 0x10), the IC feeds the amplifier itself. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"
#include "bcast.h"
#include "stockopt.h"

#define KT 0x35
#define AM_IF 2925000u

static int s_band;
static uint32_t s_hz, s_writes;
static bool s_ack = true, s_up;
static uint8_t s_vol = 13;

static void w(uint8_t r, uint8_t v) { s_ack = i2c_reg_write(KT, r, v) && s_ack; s_writes++; }
static uint8_t rd(uint8_t r) { uint8_t v = 0; if(!i2c_reg_read(KT, r, &v)) { s_ack = false; } return v; }
static void rmw(uint8_t r, uint8_t keep, uint8_t set) { w(r, (uint8_t)((rd(r) & keep) | set)); }
static void ms(uint32_t n) { delay_us(n * 1000u); }

static void gpio1(int pin, int v) { rf_write(1, 0x52, (uint16_t)((pin << 12) | 0x40 | (v & 1))); }

static void ic_volume(void) { rmw(0x0F, 0xE0, (uint8_t)((16u + (s_vol & 15u)) & 0x1F)); }

static void ic_up(void)
{
    if(s_up) { return; }
    s_ack = true;
    rmw(0x0E, (uint8_t)~0x20, 0); ms(1);
    rmw(0x76, 0xFF, 0x02); ms(5);
    rmw(0x10, (uint8_t)~0x40, 0);
    rmw(0x06, 0xF8, 2); w(0x07, 0x9C); rmw(0x08, 0xF0, 8); w(0x0A, 0); rmw(0x0D, (uint8_t)~0x10, 0);
    rmw(0x04, 0xF8, 0x80);
    for(int i = 0; i < 200 && s_ack && !(rd(0x1B) & 4u); i++) { delay_us(100); }
    rmw(0x66, (uint8_t)~8, 0); rmw(0x38, 0x8F, 0x50); w(0x62, 0x51); rmw(0x2B, 0x76, 8); w(0x2F, 0x3F);
    rmw(0x7E, 0x0F, 0x20); rmw(0x16, 0xFF, 0x10); w(0x57, 0x20); rmw(0x24, 0x7F, 0); rmw(0x41, 0xC0, 0x10);
    rmw(0x43, 0x3F, 0x40); rmw(0x2D, 0x9F, 0); w(0x42, 0x32); rmw(0x3B, 0xFF, 0x80);
    ic_volume();
    s_up = true;
}

static void ic_down(void)
{
    if(!s_up) { return; }
    rmw(0x10, 0xFF, 0x40); ms(5);
    rmw(0x76, (uint8_t)~2, 0); ms(1);
    rmw(0x0E, 0xFF, 0x20);
    s_up = false;
}

static void am_path(bool on)
{
    if(on) {
        rf_write(1, 0x30, 0x5F6B);                      /* chip 1 receiving (its stock power-on state) */
        rf_write(1, 0x56, 0); rf_write(1, 0x16, 0); rf_write(1, 0x2F, 0x0A0F);
        gpio1(5, 1); gpio1(7, 1); gpio1(4, 1);
        gpio_clr(MCU_AUD_PORT, MCU_AUD);                 /* PC13 low with AM on, as stock */
        rf_am_path(true);                               /* chip 0 antenna switches */
    } else {
        gpio1(7, 0); gpio1(4, 0); gpio1(5, 0);
        gpio_set(MCU_AUD_PORT, MCU_AUD);
        rf_am_path(false);
        rf_idle(1);
    }
}

bool bcast_has_am(void) { return true; }
int bcast_band(void) { return s_band; }
uint32_t bcast_hz(void) { return s_hz; }
bool bcast_ack(void) { return s_ack; }
uint32_t bcast_writes(void) { return s_writes; }

bool bcast_on(int band, uint32_t hz)
{
    if(band == BC_FM && (hz < BC_FM_MIN || hz > BC_FM_MAX)) { return false; }
    if(band == BC_AM && (hz < BC_AM_MIN || hz > BC_AM_MAX)) { return false; }
    if(band != BC_FM && band != BC_AM) { bcast_off(); return true; }
    if(s_band == BC_AM && band != BC_AM) { am_path(false); }
    ic_up();
    if(band == BC_FM) {
        gpio1(5, 1); gpio1(4, 0);                        /* FUN_08111a58 */
        rmw(0x88, (uint8_t)~0x40, 0);
        ic_volume();
        uint32_t ch = hz / 50000u;                       /* FUN_081169c0: (f / 10 kHz) / 5 */
        rmw(0x2F, (uint8_t)~2, 0);
        if(ch == 0x699 || ch == 0x81A || ch == 0x840) {
            rmw(0x87, 0xFF, 8);
            rmw(0x85, 0xF0, ch == 0x699 ? 3 : 8);
        } else {
            rmw(0x87, (uint8_t)~8, 0);
        }
        w(0x89, (uint8_t)ch);
        rmw(0x88, 0x70, (uint8_t)(((ch >> 8) & 0x0F) | 0x80));
    } else {
        if(s_band != BC_AM) {
            am_path(true);
            rmw(0x88, 0xFF, 0x40); w(0x8D, 0x4C); rmw(0x8C, 0x80, 4);
            rmw(0x3A, 0xC0, 0x19); rmw(0x52, 0xC0, 0x19); rmw(0x0E, 0xFF, 0x10);
            ic_volume();
            uint32_t f = AM_IF / 100u;
            w(0x8D, (uint8_t)f); rmw(0x8C, 0x80, (uint8_t)((f & 0x7FFF) >> 8)); rmw(0x88, 0xFF, 0x80);
        }
        bool neg = stockopt_opt(186, 0) != 0;            /* stock AM Offset: 0 positive, 1 negative */
        rf_set_freq(1, neg ? hz - AM_IF : hz + AM_IF);
        rf_write(1, 0x5C, 2);
        rf_write(1, 0x56, 6);
        rf_write(1, 0x5C, 0x1002);                       /* trace: ends at 0x1002 */
    }
    s_band = band;
    s_hz = hz;
    audio_route(AUDIO_DIRECT);                           /* stock mask 0x10: RF / baseband switches off */
    return s_ack;
}

void bcast_off(void)
{
    if(s_band == BC_OFF) { return; }
    if(s_band == BC_AM) { am_path(false); } else { gpio1(5, 0); }
    ic_down();
    s_band = BC_OFF;
    s_hz = 0;
}

void bcast_volume(uint8_t v)
{
    s_vol = v > 15 ? 15 : v;
    if(s_up) { ic_volume(); }
}
