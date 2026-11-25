/* D890UV speaker path, as the stock V1.05 firmware drives it (docs/AUDIO_PATHS.md "Audio routing").
 *
 * Source select FUN_080c32e8(source) -> FUN_08118df8(mask): bit0 = switch 1 (FUN_080e3c00), bit2 = switch 2
 *   (FUN_080e3580), bit3 = PC7. Both switches are GPIO outputs of RF CHIP 1 (CS PD3): FUN_08019ab4 writes reg 0x52 =
 *   idx << 12 | 0x40 | v with chip argument 1 (chip 0 has its own function, FUN_08010b2c, and other uses of 0x52: rf.c).
 *   mask 4    = switch 2: analog receive of RF chip 0
 *   mask 1    = switch 1: baseband (digital voice): FUN_080d1170() != 0 = digital channel
 *   mask 8    = PC7: analog receive of RF chip 1 (stock dual watch; not used by the cfw)
 *   mask 2    = all off: MCU tone (PB9, source 0); mask 0x10 = all off: broadcast receiver, FM (source 4) and AM
 *   FUN_0811687c is the mute of the broadcast receiver (I2C 0x35 reg 0x0F), not a baseband codec register.
 *   amplifier PC12 on (FUN_08118dbc; off FUN_08118d9c) - and everything off (FUN_080252e0) while the volume knob is at
 *   its 0 step: the stock radio plays no tone and no audio then.
 * Volume: the knob is a plain potentiometer read by ADC1 ch3 (stock buffer 0x20001D38 = cfw ADC frame slot 2) and
 *   converted to a step 0..15 (FUN_0804bc50: raw < 0x79 -> 0, < 0xF60 -> (raw - 0x78) / 0x112 + 1, else 15). The
 *   loudness is an I2C potentiometer at 0x2E (8-bit 0x5C): FUN_0806536c writes [0x00, value] with
 *   value = table 0x0815153E [Vol Level 0..8 (settings +0x3B; 0 = Indoor)][step] (FUN_081137c0, 9 x 16 bytes,
 *   0x00..0x7F = 7-bit pot), resent when it changes (FUN_0801ff48). So the volume limit caps all speaker audio,
 *   FM receive included.
 * Not done: broadcast receiver muted during a beep, the BT-headset case (FUN_08118d9c with DAT_2001af0e).
 * Unverified on hardware. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

static const uint8_t k_vol[9][16] = {                                   /* stock 0x0815153E */
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16 },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x19, 0x1D },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x0A, 0x0D, 0x10, 0x12, 0x16, 0x19, 0x1D, 0x21, 0x25 },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x0A, 0x0D, 0x12, 0x15, 0x19, 0x21, 0x25, 0x29, 0x30 },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x0A, 0x0E, 0x13, 0x19, 0x21, 0x29, 0x30, 0x37, 0x3E },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x0B, 0x10, 0x16, 0x1D, 0x25, 0x30, 0x3E, 0x45, 0x4D },
    { 0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x0A, 0x0F, 0x16, 0x1D, 0x25, 0x30, 0x3E, 0x4D, 0x56, 0x5E },
    { 0x00, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0D, 0x12, 0x19, 0x23, 0x30, 0x3E, 0x4D, 0x5E, 0x67, 0x6F },
    { 0x00, 0x01, 0x03, 0x04, 0x06, 0x0A, 0x0F, 0x16, 0x21, 0x2B, 0x3E, 0x4D, 0x5E, 0x6F, 0x77, 0x7F },
};
#define POT_ADDR 0x2E
#define KNOB_SLOT 2u                                                    /* ADC frame slot of ch3 (drivers/adc.c) */

static bool s_init, s_tone;
static int s_mode = AUDIO_MUTE;
static uint8_t s_maxvol = 8, s_step = 0xFF, s_pot = 0xFF;
static uint8_t s_sw;                                                    /* chip 1 reg 0x52 switches 1 / 2 as written */

static void sw(int idx, bool on)                                        /* stock FUN_08019ab4: write on change */
{
    uint8_t m = (uint8_t)(1u << idx);
    if(on == ((s_sw & m) != 0) && s_init) { return; }
    s_sw = on ? (uint8_t)(s_sw | m) : (uint8_t)(s_sw & ~m);
    rf_write(1, 0x52, (uint16_t)((idx << 12) | 0x40 | (on ? 1 : 0)));
}

static void pin(int p, bool v) { GPIO_BSRR(AMP_PORT) = v ? (1u << p) : (1u << (p + 16)); }

/* Modulation source switch FUN_081140ec(n): PA1, PE0, PD2, PD1, PD0. Receive (and the MCU tone, n = 5): PE0 and PD2 high,
 * the others low. The stock transmit positions (0 / 1 microphone, 2 PTT ID, 3 / 4 digital) are not used by the cfw: which
 * one carries what is not measured. */
static void mod_source_rx(void)
{
    RCC_AHB2ENR1 |= (1u << 0) | (1u << 3) | (1u << 4);                  /* GPIOA, D, E */
    GPIO_BSRR(GPIOA) = 1u << (1 + 16);
    GPIO_BSRR(GPIOE) = 1u << 0;
    GPIO_BSRR(GPIOD) = (1u << 2) | (1u << (1 + 16)) | (1u << (0 + 16));
    gpio_set(MCU_AUD_PORT, MCU_AUD);
    gpio_mode(MCU_AUD_PORT, MCU_AUD, GPIO_OUT, PULL_NONE);
    gpio_mode(GPIOA, 1, GPIO_OUT, PULL_NONE);
    gpio_mode(GPIOE, 0, GPIO_OUT, PULL_NONE);
    for(int k = 0; k < 3; k++) { gpio_mode(GPIOD, k, GPIO_OUT, PULL_NONE); }
}

static uint8_t knob_step(void)                                          /* stock FUN_0804bc50 */
{
    uint16_t raw = adc_ring()[KNOB_SLOT];                               /* frame 0 */
    if(raw < 0x79) { return 0; }
    if(raw < 0xF60) { return (uint8_t)((raw - 0x78) / 0x112 + 1); }
    return 15;
}

static void apply(int mode)
{
    if(!s_init) {
        RCC_AHB2ENR1 |= 1u << 2;                                        /* GPIOC */
        pin(AMP_PIN, false); pin(AUD_PC7_PIN, false);
        gpio_mode(AMP_PORT, AMP_PIN, GPIO_OUT, PULL_NONE);
        gpio_mode(AMP_PORT, AUD_PC7_PIN, GPIO_OUT, PULL_NONE);
        mod_source_rx();
    }
    bool on = mode != AUDIO_MUTE && knob_step() != 0;
    pin(AUD_PC7_PIN, false);
    sw(1, on && mode == AUDIO_BB);
    sw(2, on && mode == AUDIO_RF);
    s_init = true;
    pin(AMP_PIN, on);
}

void audio_route(int mode)
{
    s_mode = mode;
    if(s_tone) { return; }                                              /* applied when the beep ends */
    apply(mode);
}

/* beep (app/spk.c): stock FUN_0801a6d0 -> FUN_080c32e8(0): RF audio off, amplifier on, then back */
void audio_tone(bool on)
{
    if(on == s_tone) { return; }
    if(on) { apply(AUDIO_DIRECT); s_tone = true; }
    else { s_tone = false; apply(s_mode); }
}

bool audio_tone_muted(void) { return knob_step() == 0; }
uint8_t audio_codec_att(void) { return 0; }                             /* the I2C pot sets the volume */

static void pot_update(void)
{
    uint8_t v = k_vol[s_maxvol][s_step & 15];
    if(v == s_pot) { return; }
    if(i2c_reg_write(POT_ADDR, 0x00, v)) { s_pot = v; }
}

void audio_volume_limit(uint8_t maxvol)
{
    s_maxvol = maxvol > 8 ? 8 : maxvol;
    if(s_step == 0xFF) { s_step = knob_step(); }
    pot_update();
}

/* stock FUN_0801ff48 / the knob task: follow the knob (called from spk_tick, every 50 ms) */
void audio_poll(void)
{
    uint8_t st = knob_step();
    if(st != s_step) {
        bool was0 = s_step == 0, is0 = st == 0;
        s_step = st;
        if(was0 != is0 && s_init) { apply(s_tone ? AUDIO_DIRECT : s_mode); }   /* amplifier follows the 0 step */
    }
    pot_update();
}
