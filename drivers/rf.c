/* Two RF transceivers on a 3-wire bus: SCK PB3, SDA PB4 (bidirectional), CS# PB5 (chip 0) / PD3 (chip 1).
 * Frame: CS low, 8 bits (reg << 1 | R), 16 data bits, MSB first (stock FUN_080e2f64 write,
 * FUN_080e2cac read, FUN_080e2e44 byte out, FUN_080e2da4 16-bit in). The chip type is not identified yet
 * (register map below is what the stock firmware writes, see README "RF").
 *
 * Emulator: rf_shift_in16() also has the symbol FUN_080e2da4. emu/run.py hooks that symbol and returns the
 * RF model's register value (Renode cannot pass bit-level SDA reads back on GPIOB, see emu/README.md). */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"
#include "radio.h"
#include "bcast.h"

#define SCK_LO() gpio_clr(RF_SCK_PORT, RF_SCK)
#define SCK_HI() gpio_set(RF_SCK_PORT, RF_SCK)

static uint32_t s_freq[2];
static bool s_txing, s_rxing, s_am;                     /* chip 0 transmitting; AM air band running (drivers/bcast.c) */
static uint8_t s_sw0;                          /* shadow of chip 0's reg 0x52 outputs, all 0 after rf_init */

static inline void bitdelay(void)              /* stock FUN_080e2c28: ~5-iteration spin */
{
    for(volatile int i = 0; i < 5; i++) { }
}

static void cs(int chip, bool active)
{
    if(chip == 0) { gpio_write(RF_CS0_PORT, RF_CS0, !active); }
    else          { gpio_write(RF_CS1_PORT, RF_CS1, !active); }
}

static void shift_out8(uint8_t b)
{
    for(int i = 7; i >= 0; i--) {
        gpio_write(RF_SDA_PORT, RF_SDA, (b >> i) & 1);
        SCK_HI();
        bitdelay();
        SCK_LO();
    }
}

__attribute__((noinline, used)) uint16_t rf_shift_in16(void)
{
    uint16_t v = 0;
    SCK_LO();
    for(int i = 0; i < 16; i++) {
        v = (uint16_t)(v << 1);
        SCK_HI();
        bitdelay();
        if(gpio_read(RF_SDA_PORT, RF_SDA)) { v |= 1; }
        SCK_LO();
    }
    return v;
}
uint16_t FUN_080e2da4(void) __attribute__((alias("rf_shift_in16")));

void rf_write(int chip, uint8_t reg, uint16_t val)
{
    uint32_t p = irq_save();
    SCK_LO();
    bitdelay();
    cs(chip, true);
    bitdelay();
    shift_out8((uint8_t)(reg << 1));
    shift_out8((uint8_t)(val >> 8));
    shift_out8((uint8_t)val);
    cs(chip, false);
    bitdelay();
    gpio_clr(RF_SDA_PORT, RF_SDA);
    irq_restore(p);
}

/* Forced TX stop (hal/safety.h board_tx_kill, from a fault handler or the SysTick TX cap): both CS# high first (a
 * fault may have cut a transfer), SDA back to output, then chip 0 out of TX as rf_tx_fm(off) starts it
 * (reg 0x52 = 0x0041) and reg 0x30 = idle. */
void rf_tx_kill(void)
{
    cs(0, false);
    cs(1, false);
    gpio_mode(RF_SDA_PORT, RF_SDA, GPIO_OUT, PULL_NONE);
    rf_write(0, 0x52, 0x0041);
    s_sw0 |= 1u;
    rf_write(0, 0x56, 0x0000);
    rf_write(0, 0x30, 0x5F00);
    s_txing = false;
}

uint16_t rf_read(int chip, uint8_t reg)
{
    uint32_t p = irq_save();
    SCK_LO();
    cs(chip, true);
    shift_out8((uint8_t)((reg << 1) | 1));
    gpio_set(RF_SDA_PORT, RF_SDA);                     /* stock leaves ODR4 = 1 before turning SDA around */
    gpio_mode(RF_SDA_PORT, RF_SDA, GPIO_IN, PULL_NONE);
    uint16_t v = rf_shift_in16();
    gpio_mode(RF_SDA_PORT, RF_SDA, GPIO_OUT, PULL_NONE);
    cs(chip, false);
    gpio_clr(RF_SDA_PORT, RF_SDA);
    irq_restore(p);
    return v;
}

struct rv { uint8_t r; uint16_t v; };
static void write_list(int chip, const struct rv *l, int n)
{
    for(int i = 0; i < n; i++) { rf_write(chip, l[i].r, l[i].v); }
}

/* stock FUN_0805ffe8 (power-on init of one chip), with its callees inlined:
 * FUN_081155ac(0) (< 30 MHz table, first branch), FUN_0802520c, FUN_080e2edc(0) (reg 0x5B band 0). */
static const struct rv k_init_a[] = {
    {0x50, 0x0001}, {0x58, 0xF1F0},
    {0x40, 0x70E6}, {0x40, 0x60FF}, {0x40, 0x50F4}, {0x40, 0x40E8}, {0x40, 0x30DC}, {0x40, 0x20CD},
    {0x40, 0x10C3}, {0x40, 0x00B9},
    {0x53, 0x773F}, {0x53, 0x673F}, {0x53, 0x567F}, {0x53, 0x467F}, {0x53, 0x363A}, {0x53, 0x263A},
    {0x53, 0x1634}, {0x53, 0x00F0}, {0x42, 0x46A1}, {0x43, 0x1F4F}, {0x49, 0x706A},
    {0x12, 0x20D8}, {0x13, 0x00EE},
    {0x5B, 0x0085},
    {0x41, 0x711A}, {0x42, 0x4A61}, {0x44, 0x1490}, {0x45, 0x8874}, {0x46, 0x006E}, {0x47, 0x27FE},
    {0x49, 0x0050}, {0x49, 0x1078}, {0x49, 0x7034}, {0x49, 0xB0CC}, {0x4D, 0x0000}, {0x57, 0xA008},
    {0x5C, 0x0001}, {0x5D, 0x8611}, {0x5E, 0x0768}, {0x5F, 0x0768}, {0x60, 0x0300}, {0x2A, 0x1081},
    {0x30, 0x5F6B},
};
static const struct rv k_init_b[] = {        /* after the frequency: */
    {0x59, 0x221C}, {0x29, 0x1090}, {0x56, 0x0000}, {0x3C, 0x00FC},
};

/* stock FUN_0810ea14(p1, 0, chip): RX path set-up, p1 = 0 analog (k_rx0, 12.5 and 25 kHz alike), 1 digital (k_rx1):
 * FUN_0811586c(channel type), seen in the register traces of both. Includes FUN_080c3d18 / FUN_080c39e4 coefficient tables. */
static const struct rv k_rx0[] = {
    {0x11, 0x041A}, {0x2B, 0x708D}, {0x53, 0x773F}, {0x26, 0x6468}, {0x27, 0x6080}, {0x27, 0x199C},
    {0x18, 0x006E}, {0x19, 0x0381}, {0x24, 0xF4AF}, {0x25, 0x0B56}, {0x32, 0x22A2},
    {0x20, 0x8D15}, {0x21, 0x3B99}, {0x22, 0x8D90}, {0x23, 0x3B15},     /* FUN_080c3d18(0xf) */
    {0x1C, 0x8137}, {0x1D, 0x3EE0}, {0x1E, 0x80A8}, {0x1F, 0x3F6F},     /* FUN_080c39e4(6) */
};
static const struct rv k_rx1[] = {
    {0x1A, 0x64C1}, {0x1B, 0x1258}, {0x2B, 0x708D}, {0x53, 0x773F}, {0x26, 0x6468}, {0x27, 0x6080},
    {0x18, 0x006E}, {0x19, 0x0381}, {0x24, 0xF4AF}, {0x25, 0x0B56}, {0x32, 0x0622},
    {0x20, 0x0000}, {0x21, 0x0000}, {0x22, 0x0000}, {0x23, 0x0000},     /* FUN_080c3d18(0) */
    {0x1C, 0x0000}, {0x1D, 0x0000}, {0x1E, 0x0000}, {0x1F, 0x0000},     /* FUN_080c39e4(0) */
};
/* stock FUN_0810ea14(x, 1, 0): TX set-up (chip 0 only), branch FUN_08027bc4() != 0 */
static const struct rv k_tx[] = {
    {0x19, 0x0381}, {0x24, 0xA192}, {0x25, 0x2590}, {0x26, 0x0010}, {0x32, 0x0600}, {0x53, 0x7000},
    {0x2B, 0xF08D}, {0x18, 0x006E},
    {0x20, 0x8D90}, {0x21, 0x3B15}, {0x22, 0x8D15}, {0x23, 0x3B99},     /* FUN_080c3d18(1) */
    {0x1C, 0x80D8}, {0x1D, 0x3F3F}, {0x1E, 0x80F0}, {0x1F, 0x3F27},     /* FUN_080c39e4(0xf) */
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Chip 0 reg 0x52 = eight switch outputs, written as idx << 12 | 0x40 | value (stock FUN_08010b2c, on change). Band /
 * antenna switches, split at 300 MHz. From the decompile of V1.05; receive and transmit on both bands also seen in register
 * traces of the stock firmware in the emulator (tools/rf_compare.py); none measured on a radio:
 *   receive (FUN_0810d490, one channel watched)   UHF 0 = 1, 6 = 0, 7 = 0, 4 = 1    VHF 0 = 0, 6 = 1, 7 = 1, 4 = 0
 *   with the AM air band on chip 1                UHF 0 = 1, 6 = 0, 7 = 1, 4 = 0    VHF 0 = 0, 6 = 0, 7 = 0, 4 = 1
 *   transmit (+ FUN_0811b308)   0 = 0 (before anything else), 6 = 0, 5 = 1 only at 200..300 MHz, 3 = 1 UHF / 0 VHF last
 */
static void sw0(int idx, bool on)
{
    uint8_t m = (uint8_t)(1u << idx);
    if(on == ((s_sw0 & m) != 0)) { return; }
    s_sw0 = on ? (uint8_t)(s_sw0 | m) : (uint8_t)(s_sw0 & ~m);
    rf_write(0, 0x52, (uint16_t)((idx << 12) | 0x40 | (on ? 1 : 0)));
}
static bool is_uhf(uint32_t hz) { return hz >= 300000000u; }
static void band_rx(void)
{
    bool u = is_uhf(s_freq[0]);
    sw0(5, false);
    sw0(0, u);
    sw0(6, !u && !s_am);
    sw0(7, u == s_am);
    sw0(4, u != s_am);
}
void rf_am_path(bool on) { s_am = on; if(!s_txing) { band_rx(); } }

bool rf_init(int chip)
{
    /* boot self-test as stock: write reg 0x57 = 0xA888, read it back */
    rf_write(chip, 0x57, 0xA888);
    bool ok = rf_read(chip, 0x57) == 0xA888;
    for(int i = 0; i < 8; i++) { rf_write(chip, 0x52, (uint16_t)((i << 12) | 0x40)); }   /* stock FUN_0805f640 */
    if(chip == 0) { s_sw0 = 0; s_txing = s_rxing = false; }
    write_list(chip, k_init_a, N(k_init_a));
    write_list(chip, k_rx0, N(k_rx0));
    rf_write(chip, 0x19, 0x0381);
    rf_write(chip, 0x48, 0x9826);                      /* FUN_08116480(1, chip) */
    rf_set_freq(chip, 435125000u);                     /* stock default: FUN_08116674(0x297f2b4) */
    write_list(chip, k_init_b, N(k_init_b));
    return ok;
}

/* stock FUN_08116674: f [10 Hz] -> FUN_08047e10 reference correction (cal 2000 = none) -> Hz -> regs
 * 0x69 (high half) / 0x68 (low half). */
/* squelch threshold of chip 0 (stock FUN_080e373c -> FUN_080e36f4: reg 0x49 = threshold, sub-register 0): from the
 * RF calibration for the frequency and the squelch level, rewritten on every frequency change */
static int s_sql = 3;
static uint16_t s_thr;

static void sql_write(void)
{
    s_thr = dac_sql_threshold(s_freq[0], s_sql);
    rf_write(0, 0x49, s_thr);
}

void rf_set_sql(int level)
{
    s_sql = level;
    sql_write();
}

uint16_t rf_sql_thr(void) { return s_thr; }

static void rx_on(void);
static void band_table(void);
void rf_set_freq(int chip, uint32_t hz)
{
    chip &= 1;
    bool rx = chip == 0 && s_rxing && !s_txing;
    bool band = is_uhf(hz) != is_uhf(s_freq[0]);
    s_freq[chip] = hz;
    if(rx) { rf_write(0, 0x56, 0x0000); }               /* stock: receiver off around every frequency change */
    rf_write(chip, 0x69, (uint16_t)(hz >> 16));
    rf_write(chip, 0x68, (uint16_t)hz);
    if(chip == 0) {
        dac_rx_tune(hz);                                /* stock FUN_08117034: front-end tune follows the frequency */
        sql_write();
        if(!s_txing) { band_rx(); }
        if(rx) {
            if(band) { band_table(); sql_write(); }
            rx_on();
        }
    }
}

/* Receive, register for register as the stock firmware ends with after a transmission and on a channel change (V1.05
 * register traces in the emulator, tools/rf_compare.py, docs/RF_COMPARE.md; functions in the comments; not measured on a
 * radio). Until 2026-11-24 the cfw never wrote reg 0x56 = 1 (receiver on) here and used the VHF tables on both bands.
 * Left as before: reg 0x30 = 0x5F77 for analog (the stock value is 0x5F74 with the calibration byte DAT_200151c4 = 'f',
 * else 0x5F77; + 8 with channel bit 6 of +0x34). wide = 25 kHz, analog only (digital channels are always narrow). */
static void rx_on(void)                                /* FUN_080e35c0: the receiver runs only with reg 0x56 = 1 */
{
    rf_write(0, 0x58, 0xF1F0);
    rf_write(0, 0x5F, s_freq[0] < 390000000u ? 0x0468 : 0x0569);
    rf_write(0, 0x60, 0x0309);
    rf_write(0, 0x56, 0x0001);
}
static void band_table(void)                           /* FUN_081155ac */
{
    static const struct rv k_vhf[] = { {0x53, 0x773F}, {0x53, 0x673F}, {0x53, 0x567F}, {0x53, 0x467F}, {0x53, 0x363A}, {0x53, 0x263A},
                                       {0x53, 0x1634}, {0x53, 0x00F0}, {0x42, 0x46A1}, {0x43, 0x1F4F}, {0x49, 0x706A} };
    static const struct rv k_uhf[] = { {0x53, 0x773F}, {0x53, 0x673F}, {0x53, 0x567F}, {0x53, 0x463D}, {0x53, 0x363A}, {0x53, 0x2637},
                                       {0x53, 0x1634}, {0x53, 0x00F0}, {0x42, 0x46A1}, {0x43, 0x1F4F}, {0x49, 0x706A} };
    write_list(0, is_uhf(s_freq[0]) ? k_uhf : k_vhf, N(k_vhf));
}
void rf_rx_fm(int chip, bool wide)
{
    bool fm = radio_current()->mode == CFW_MODE_FM;
    s_txing = false;
    s_rxing = true;
    band_rx();
    rf_write(0, 0x56, 0x0000);
    rf_write(0, 0x16, 0x0000);
    rf_write(0, 0x3F, 0x3232);
    rf_write(0, 0x2A, fm ? 0x1081 : 0x108C);           /* FUN_08116f40 -> FUN_08114b48 */
    write_list(0, fm ? k_rx0 : k_rx1, fm ? N(k_rx0) : N(k_rx1));
    rf_write(0, 0x30, fm ? 0x5F77 : 0x5F75);           /* FUN_080e4154 */
    rf_write(0, 0x10, fm ? 0xA3A3 : 0xA3E0);
    rf_write(0, 0x11, fm ? 0x041A : 0x0000);
    if(fm) { rf_write(0, 0x18, 0x006E); }
    rf_write(0, 0x69, (uint16_t)(s_freq[0] >> 16));
    rf_write(0, 0x68, (uint16_t)s_freq[0]);
    rf_write(0, 0x48, !fm ? 0x016E : wide ? 0x9826 : 0x082E);   /* FUN_08116480 */
    rf_write(0, 0x1A, 0x64C1);                         /* FUN_08115ee8 */
    rf_write(0, 0x1B, 0x1258);
    rf_write(0, 0x18, 0x0068);                         /* FUN_08025d04 */
    band_table();
    sql_write();
    dac_rx_tune(s_freq[0]);
    rx_on();
    if(chip == 0 && bcast_band() == BC_OFF) { audio_route(fm ? AUDIO_RF : AUDIO_BB); }   /* drivers/audio.c */
}

/* Transmit keying, as the stock firmware does it (PTT trace by the cfw-emu session, an analog channel at 449.440 FM):
 *   reg 0x52 = 0x0040, 0x56 = 0, frequency, TX set-up regs (k_tx = FUN_0810ea14(x,1,0)), then
 *   reg 0x56 = 0x0002 = transmitter on, then switch 3 (0x3041 on UHF; stock never uses reg 0x30 to key TX);
 *   release: band switches back to receive (band_rx; 0x0041, 0x4041 on UHF), RX regs, reg 0x30 = 0x5F77.
 * The PA control level on DAC1 ch1 (PA4) + TX enables PE1 / PA15 (stock IRQ17_Handler) are set by the caller (dac_apc).
 * Unverified on hardware: test into a dummy load. */
void rf_tx_fm(int chip, bool on)
{
    if(on) {
        if((chip & 1) == 0) { audio_route(AUDIO_MUTE); }           /* stock: speaker off while transmitting */
        s_txing = true;
        s_rxing = false;
        sw0(0, false);
        sw0(6, false);
        rf_write(chip, 0x56, 0x0000);
        rf_set_freq(chip, s_freq[chip & 1]);
        sw0(5, s_freq[0] >= 200000000u && s_freq[0] < 300000000u);
        write_list(chip, k_tx, N(k_tx));
        bool lo = s_freq[0] < 390000000u;              /* FUN_080e3c68, as the trace */
        rf_write(chip, 0x5E, lo ? 0x0468 : 0x0569);
        rf_write(chip, 0x60, lo ? 0x0306 : 0x0309);
        rf_write(chip, 0x56, 0x0002);
        sw0(3, is_uhf(s_freq[0]));
    } else {
        rf_rx_fm(chip, true);
    }
}

/* stock FUN_080e3c20: reg 0x31 = tone | 0x1000 (0 = off), reg 0x2E = x | 0xC000 */
void rf_set_ctcss(int chip, uint16_t tone_dhz)
{
    rf_write(chip, 0x31, tone_dhz ? (uint16_t)(tone_dhz | 0x1000) : 0);
}

/* Receive status as the stock firmware reads it (V1.05):
 *   reg 0x02 [9:0] = RSSI: FUN_08049e3c, squelch FUN_080d22f0 (open when RSSI >= the threshold written to
 *                    reg 0x49 by FUN_080e36f4, close below threshold - 12)
 *   reg 0x01 [15:8] = noise (> 0x50 closes the squelch), [3:0] = glitch count
 *   reg 0x0E bit 14 = carrier / squelch detect (DAT_2000406c, FUN_080df38c)
 * The RSSI -> dBm scale is not in the stock code (it compares raw values against calibration data);
 * rf_rssi_dbm() assumes 0.5 dB steps from -160 dBm, which is also what the emulator model uses. */
uint16_t rf_rssi(int chip) { return rf_read(chip, 0x02) & 0x3FF; }
uint16_t rf_noise(int chip) { return rf_read(chip, 0x01); }
bool rf_carrier(int chip) { return (rf_read(chip, 0x0E) & 0x4000) != 0; }
int rf_rssi_dbm(uint16_t raw) { return (int)raw / 2 - 160; }

void rf_idle(int chip)
{
    if(chip & 1) { rf_write(1, 0x56, 0x0000); return; }   /* stock: chip 1 idle = reg 0x56 0, reg 0x30 stays 0x5F6B */
    rf_write(chip, 0x30, 0x5F00);                      /* stock FUN_0811c83c(1) */
    if((chip & 1) == 0) {
        rf_write(0, 0x56, 0x0000);
        s_rxing = false;
        audio_route(AUDIO_MUTE);
    }
}
