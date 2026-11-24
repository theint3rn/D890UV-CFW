/* D890UV accessory module on UART4 (JieLi BT / noise-reduction board): only what VOX needs.
 *
 * Stock V1.05 (decompile):
 *   UART4 = PC10 TX / PC11 RX, AF8, pull-up, 115200 8N1 (FUN_080635c8(0x1C200)).
 *   Frames both ways: A5 5A cmd len data[len] 06 (MCU parser FUN_080c92a8 / FUN_080c93a0, cmd byte = DAT_2001b8fa).
 *   Hello: cmd 02 "D890" (FUN_0801ee18); module answers cmd 01 [02 'V' ...]; every MCU frame cmd X is acked with
 *   cmd 01 [X] (wait mask 0x20005030, resent while unacked).
 *   VOX on/off: cmd 0x12, 14 data bytes from 0x2001446E (FUN_0806a60c / FUN_0806ae54 -> FUN_08111908(0x12)):
 *     [0] = 1 (0 only in the power-off path FUN_08034570), [12] (0x2001447A) = VOX setting (stock clamps it to 0/1,
 *     FUN_080d0000 range check), others 0 in a default codeplug (stock boot frame "A5 5A 12 0E 01 00 .. 00 06").
 *   Voice detected: module status cmd 3, 8 data bytes copied to 0x20015516 (FUN_080c93a0 case 3); byte [3]
 *     (0x20015519) != 0 = voice. The stock VOX poll FUN_08133618 keys TX after 3 polls with it set (DAT_2001d777)
 *     and releases after the VOX delay (settings +0x0D) + 5 ticks without it.
 *   The RF chip VOX regs are fixed on this radio (FUN_0802520c: reg 0x12 = 0x20D8, reg 0x13 = 0x00EE at init).
 * Unverified on hardware. Emulator: models/Bluetooth.cs acks frames; `bt Send "03 00 00 00 01 00 00 00 00"` = voice. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

#define U4_BASE 0x40004C00u
#define U4(o) REG32(U4_BASE + (o))

static bool s_init, s_voice, s_acked;
static uint8_t s_vox, s_rx[48], s_n, s_tries;
static uint32_t s_last_ms;

static void put(uint8_t b)
{
    for(uint32_t n = 0; n < 100000u && !(U4(0x1C) & (1u << 7)); n++) { }   /* TXE */
    U4(0x28) = b;
}

static void frame(uint8_t cmd, const uint8_t *d, uint8_t len)
{
    put(0xA5); put(0x5A); put(cmd); put(len);
    for(uint8_t i = 0; i < len; i++) { put(d[i]); }
    put(0x06);
}

static void on_frame(const uint8_t *f, uint8_t len)
{
    uint8_t cmd = f[2];
    if(cmd == 0x03 && len >= 4) { s_voice = f[4 + 3] != 0; }
    if(cmd == 0x01 && len >= 1 && f[4] == 0x12) { s_acked = true; }
}

void UART4_IRQHandler(void)
{
    uint32_t isr = U4(0x1C);
    if(isr & (1u << 3)) { U4(0x20) = 1u << 3; }                    /* ORE */
    if(!(isr & (1u << 5))) { return; }
    uint8_t b = (uint8_t)U4(0x24);
    if(s_n == 0 && b != 0xA5) { return; }
    if(s_n == 1 && b != 0x5A) { s_n = 0; return; }
    s_rx[s_n++] = b;
    if(s_n >= 4 && (s_rx[3] + 5u > sizeof s_rx)) { s_n = 0; return; }
    if(s_n >= 4 && s_n == s_rx[3] + 5u) { on_frame(s_rx, s_rx[3]); s_n = 0; }
}

static void init(void)
{
    if(s_init) { return; }
    RCC_APB1ENR1 |= 1u << 19;                                       /* UART4EN */
    RCC_AHB2ENR1 |= 1u << 2;                                        /* GPIOC */
    (void)RCC_APB1ENR1;
    for(int p = 10; p <= 11; p++) { gpio_af(GPIOC, p, 8); gpio_mode(GPIOC, p, GPIO_AF, PULL_UP); }
    U4(0x00) = 0;
    U4(0x0C) = (120000000u + 57600u) / 115200u;
    U4(0x04) = 0; U4(0x08) = 0;
    U4(0x00) = (1u << 5) | (1u << 3) | (1u << 2) | 1u;              /* RXNEIE, TE, RE, UE */
    nvic_enable(64);
    static const uint8_t hello[4] = { 'D', '8', '9', '0' };
    frame(0x02, hello, 4);
    s_init = true;
}

static void send_vox(void)
{
    uint8_t d[14] = { 1 };
    d[12] = s_vox ? 1 : 0;
    s_acked = false;
    s_last_ms = hal_micros() / 1000u;
    frame(0x12, d, sizeof d);
}

void vox_hw_config(uint8_t level)
{
    init();
    s_vox = level & 3;
    s_voice = false;
    s_tries = 5;
    send_vox();
}

bool vox_hw_active(void)
{
    if(!s_init) { return false; }
    if(!s_acked && s_tries && hal_micros() / 1000u - s_last_ms > 200u) { s_tries--; send_vox(); }   /* stock resends */
    return s_vox && s_voice;
}
