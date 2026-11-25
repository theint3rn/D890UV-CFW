/* GPIO setup for every pin the cfw uses (directions follow the stock drivers cited in hal/board.h). */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

void gpio_mode(uint32_t port, int pin, int mode, int pull)
{
    uint32_t p = irq_save();
    GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (2 * pin))) | ((uint32_t)mode << (2 * pin));
    GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << (2 * pin))) | ((uint32_t)pull << (2 * pin));
    if(mode == GPIO_OUT || mode == GPIO_AF) {
        GPIO_OSPEEDR(port) |= 2u << (2 * pin);         /* high speed */
    }
    irq_restore(p);
}

void gpio_af(uint32_t port, int pin, int af)
{
    volatile uint32_t *r = pin < 8 ? &GPIO_AFRL(port) : &GPIO_AFRH(port);
    int s = (pin & 7) * 4;
    *r = (*r & ~(0xFu << s)) | ((uint32_t)af << s);
    gpio_mode(port, pin, GPIO_AF, PULL_NONE);
}

void gpio_init_board(void)
{
    RCC_AHB2ENR1 |= 0x1FFu;                            /* GPIOA..GPIOI */
    RCC_APB3ENR |= 1u << 1;                            /* SYSCFG */
    (void)RCC_AHB2ENR1;

    /* power latch first: keep the radio on once the power key is released */
    gpio_set(PIN_PWR_LATCH_PORT, PIN_PWR_LATCH);
    gpio_mode(PIN_PWR_LATCH_PORT, PIN_PWR_LATCH, GPIO_OUT, PULL_NONE);
    gpio_mode(PIN_PWR_KEY_PORT, PIN_PWR_KEY, GPIO_IN, PULL_DOWN);

    /* LCD bus: data PE7..14, control PD12..15 (all idle high) */
    for(int i = 0; i < 8; i++) {
        gpio_set(LCD_DATA_PORT, LCD_DATA_SHIFT + i);
        gpio_mode(LCD_DATA_PORT, LCD_DATA_SHIFT + i, GPIO_OUT, PULL_NONE);
    }
    GPIO_BSRR(LCD_CTRL_PORT) = (1u << LCD_DC) | (1u << LCD_RST) | (1u << LCD_CS) | (1u << LCD_WR);
    gpio_mode(LCD_CTRL_PORT, LCD_DC, GPIO_OUT, PULL_NONE);
    gpio_mode(LCD_CTRL_PORT, LCD_RST, GPIO_OUT, PULL_NONE);
    gpio_mode(LCD_CTRL_PORT, LCD_CS, GPIO_OUT, PULL_NONE);
    gpio_mode(LCD_CTRL_PORT, LCD_WR, GPIO_OUT, PULL_NONE);

    /* key matrix columns, side keys (active low, pulled up) */
    gpio_mode(KEY_COL0_PORT, KEY_COL0, GPIO_IN, PULL_UP);
    gpio_mode(KEY_COL1_PORT, KEY_COL1, GPIO_IN, PULL_UP);
    gpio_mode(KEY_COL2_PORT, KEY_COL2, GPIO_IN, PULL_UP);
    gpio_mode(PIN_PTT_PORT, PIN_PTT, GPIO_IN, PULL_UP);
    gpio_mode(PIN_PF1_PORT, PIN_PF1, GPIO_IN, PULL_UP);
#ifndef CFW_KEEP_SWD
    gpio_mode(PIN_PF2_PORT, PIN_PF2, GPIO_IN, PULL_UP);   /* gives up SWCLK, like the stock firmware */
#endif
    gpio_mode(PIN_PF3_PORT, PIN_PF3, GPIO_IN, PULL_UP);

    /* encoder */
    gpio_mode(ENC_PORT, ENC_A, GPIO_IN, PULL_UP);
    gpio_mode(ENC_PORT, ENC_B, GPIO_IN, PULL_UP);

    /* SPI-NAND: CS high, SCK low, IO0 out, IO1 in */
    gpio_set(NAND_CS_PORT, NAND_CS);
    gpio_mode(NAND_CS_PORT, NAND_CS, GPIO_OUT, PULL_NONE);
    gpio_clr(NAND_SCK_PORT, NAND_SCK);
    gpio_mode(NAND_SCK_PORT, NAND_SCK, GPIO_OUT, PULL_NONE);
    gpio_mode(NAND_IO0_PORT, NAND_IO0, GPIO_OUT, PULL_NONE);
    gpio_mode(NAND_IO1_PORT, NAND_IO1, GPIO_IN, PULL_UP);

    /* RF 3-wire: both CS high, SCK low, SDA out low */
    gpio_set(RF_CS0_PORT, RF_CS0);
    gpio_set(RF_CS1_PORT, RF_CS1);
    gpio_mode(RF_CS0_PORT, RF_CS0, GPIO_OUT, PULL_NONE);
    gpio_mode(RF_CS1_PORT, RF_CS1, GPIO_OUT, PULL_NONE);
    gpio_clr(RF_SCK_PORT, RF_SCK);
    gpio_mode(RF_SCK_PORT, RF_SCK, GPIO_OUT, PULL_NONE);
    gpio_clr(RF_SDA_PORT, RF_SDA);
    gpio_mode(RF_SDA_PORT, RF_SDA, GPIO_OUT, PULL_NONE);
    gpio_clr(TX_EN0_PORT, TX_EN0);
    gpio_mode(TX_EN0_PORT, TX_EN0, GPIO_OUT, PULL_NONE);
    gpio_clr(TX_EN1_PORT, TX_EN1);
    gpio_mode(TX_EN1_PORT, TX_EN1, GPIO_OUT, PULL_NONE);

    /* baseband: CS high, off / in reset until bb_init (stock FUN_080d6f5c), ready lines in */
    gpio_set(BB_CS_PORT, BB_CS);
    gpio_mode(BB_CS_PORT, BB_CS, GPIO_OUT, PULL_NONE);
    gpio_clr(BB_RDY_PORT, BB_RST);
    gpio_mode(BB_RDY_PORT, BB_RST, GPIO_OUT, PULL_NONE);
    gpio_clr(BB_RDY_PORT, BB_NRST);
    gpio_mode(BB_RDY_PORT, BB_NRST, GPIO_OUT, PULL_NONE);
    gpio_clr(BB_RDY_PORT, BB_PD10);
    gpio_mode(BB_RDY_PORT, BB_PD10, GPIO_OUT, PULL_NONE);
    gpio_mode(BB_RDY_PORT, BB_RDY, GPIO_IN, PULL_DOWN);
    gpio_mode(BB_RDY_PORT, BB_PEND, GPIO_IN, PULL_DOWN);
}
