/* USART2 = GPS receiver (stock: NMEA $GNRMC/$GPGGA..., CLAUDE.md), 9600 8N1, interrupt per byte into the shared NMEA
 * parser (app/gps.c). PCLK1 = 120 MHz. Pins as stock FUN_08063498 (USART2 init, called with 0x2580 = 9600 baud):
 * PA2 = TX, PA3 = RX, alternate function 7, pull-down, high speed. (FUN_080635c8 is the UART4 init: PC10 / PC11.)
 * TX: the stock module set-up lines (gps_uart_send_cfg). No GPS power switch: toggling Menu > GPS > GPS On/Off in the
 * stock emulator changes no GPIO (GPIOA-I MODER / ODR dumps identical); only these lines go out (unverified on
 * hardware). */
#include "stm32u5.h"
#include "hal.h"
#include "board.h"

#define USART2_BASE 0x40004400u
#define U2(o) REG32(USART2_BASE + (o))
void gps_uart_byte(uint8_t b);

void USART2_IRQHandler(void)
{
    uint32_t isr = U2(0x1C);
    if(isr & (1u << 5)) { gps_uart_byte((uint8_t)U2(0x24)); }       /* RXNE */
    if(isr & (1u << 3)) { U2(0x20) = 1u << 3; }                    /* ORE */
}

void gps_uart_hw_init(void)
{
    RCC_APB1ENR1 |= 1u << 17;                                       /* USART2EN */
    RCC_AHB2ENR1 |= 1u << 0;                                        /* GPIOA */
    (void)RCC_APB1ENR1;
    for(int pin = 2; pin <= 3; pin++) { gpio_af(GPIOA, pin, 7); gpio_mode(GPIOA, pin, GPIO_AF, PULL_DOWN); }
    U2(0x00) = 0;
    U2(0x0C) = (120000000u + 4800u) / 9600u;
    U2(0x04) = 0;
    U2(0x08) = 0;
    U2(0x00) = (1u << 5) | (1u << 3) | (1u << 2) | 1u;              /* RXNEIE, TE, RE, UE */
    nvic_enable(62);
}

/* stock V1.05 boot / GPS on (emulator usart2.txt): GNSS system h00000001 (GPS), then NMEA output enables in this order,
 * GGA (0), RMC (4) and VTG (5) on, the others off */
static void tx_str(const char *p)
{
    for(; *p; p++) {
        for(uint32_t n = 0; n < 200000u && !(U2(0x1C) & (1u << 7)); n++) { }   /* TXE */
        U2(0x28) = (uint8_t)*p;
    }
}

void gps_uart_send_cfg(void)
{
    static const uint8_t order[10] = { 0, 1, 2, 5, 3, 4, 6, 7, 8, 9 };
    char b[20] = "$CFGMSG,0,x,y\r\n";
    tx_str("$CFGSYS,h00000001\r\n");
    for(int i = 0; i < 10; i++) {
        uint8_t m = order[i];
        b[10] = (char)('0' + m); b[12] = (char)(m == 0 || m == 4 || m == 5 ? '1' : '0');
        tx_str(b);
    }
}
