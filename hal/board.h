/* D890UV board pin map (from project notes + the stock V1.05 functions cited per line). */
#ifndef CFW_BOARD_H
#define CFW_BOARD_H
#include <stdbool.h>
#include "stm32u5.h"

/* Power */
#define PIN_PWR_LATCH_PORT GPIOC   /* PC3: keeps the radio powered (high). Stock FUN_080d6954 drops it. */
#define PIN_PWR_LATCH      3
#define PIN_PWR_KEY_PORT   GPIOA   /* PA0: power key / volume-knob switch, high = pressed / off request */
#define PIN_PWR_KEY        0

/* LCD 8080 bus (stock FUN_0805d830 init, FUN_080909a0 cmd, FUN_08090944 data, FUN_0806e4c0 bus) */
#define LCD_DATA_PORT      GPIOE   /* PE7..PE14 = D0..D7 (also key-matrix rows) */
#define LCD_DATA_SHIFT     7
#define LCD_CTRL_PORT      GPIOD
#define LCD_DC             12      /* PD12 D/C# (1 = data) */
#define LCD_RST            13      /* PD13 reset (low pulse) */
#define LCD_CS             14      /* PD14 CS# */
#define LCD_WR             15      /* PD15 WR# (strobe on rising edge) */

/* Key matrix columns (stock FUN_08101260), active low, pulled up */
#define KEY_COL0_PORT      GPIOC
#define KEY_COL0           4
#define KEY_COL1_PORT      GPIOB
#define KEY_COL1           1
#define KEY_COL2_PORT      GPIOB
#define KEY_COL2           2

/* Side keys (stock FUN_0811d9cc), active low */
#define PIN_PTT_PORT       GPIOA
#define PIN_PTT            10
#define PIN_PF1_PORT       GPIOE
#define PIN_PF1            2
#define PIN_PF2_PORT       GPIOA
#define PIN_PF2            14      /* = SWCLK: using it as a key input disables SWD (stock does the same) */
#define PIN_PF3_PORT       GPIOA
#define PIN_PF3            8

/* Channel encoder (stock IRQ19_Handler / IRQ20_Handler -> FUN_080c85cc) */
#define ENC_PORT           GPIOC
#define ENC_A              8       /* EXTI8 -> IRQ 19 */
#define ENC_B              9       /* EXTI9 -> IRQ 20 */
#define IRQ_EXTI8          19
#define IRQ_EXTI9          20

/* SPI-NAND, bit-banged (stock FUN_08037db8 / FUN_080377a4 / FUN_080379b4) */
#define NAND_CS_PORT       GPIOC
#define NAND_CS            5
#define NAND_SCK_PORT      GPIOE
#define NAND_SCK           15
#define NAND_IO0_PORT      GPIOB
#define NAND_IO0           10      /* MOSI / IO0 */
#define NAND_IO1_PORT      GPIOB
#define NAND_IO1           8       /* MISO / IO1 */

/* RF transceivers, 3-wire (stock FUN_080e2f64 write, FUN_080e2cac read) */
#define RF_SCK_PORT        GPIOB
#define RF_SCK             3
#define RF_SDA_PORT        GPIOB
#define RF_SDA             4
#define RF_CS0_PORT        GPIOB
#define RF_CS0             5       /* chip 0 (stock param 0) */
#define RF_CS1_PORT        GPIOD
#define RF_CS1             3       /* chip 1 (stock param 1) */
/* TX enables: stock IRQ17_Handler (EXTI6 = baseband TX-state line PD6) sets both high with the APC level on the
 * rising edge and low (APC 0) on the falling edge; FUN_08010c1c drives them low on RX return (docs/AUDIO_PATHS.md). */
#define TX_EN0_PORT        GPIOA
#define TX_EN0             15
#define TX_EN1_PORT        GPIOE
#define TX_EN1             1

/* MCU audio / analog outputs (docs/AUDIO_PATHS.md) */
#define TX_PWM_PORT        GPIOB   /* PB9 = TIM4_CH4 (AF2): MCU audio into the modulator (stock FUN_0811a730) */
#define TX_PWM             9
#define TX_PWM_AF          2
#define TX_PWM_TOP         2500u   /* cfw: TIM4 period at 120 MHz = 48 kHz (= ADC_FS), one sample per period */
#define APC_DAC_PIN        4       /* PA4 = DAC1 ch1: TX power control (stock FUN_0801694c) */
#define TUNE_DAC_PIN       5       /* PA5 = DAC1 ch2: UHF RX front-end tune (stock FUN_08010bf0) */
/* speaker routing (drivers/audio.c, stock FUN_080c32e8 -> FUN_08118df8): PC12 = amplifier, PC7, RF chip 1 reg 0x52
 * switches 1 (baseband audio) / 2 (RF chip 0 audio) */
#define AMP_PORT           GPIOC
#define AMP_PIN            12
#define AUD_PC7_PIN        7
#define MCU_AUD_PORT       GPIOC   /* PC13 (stock FUN_0810e010): high in every chip 0 receive / transmit state, low with AM air; */
#define MCU_AUD            13      /* meaning not measured (probably which receiver the MCU audio input PC0 hears) */
enum { AUDIO_MUTE = 0, AUDIO_RF = 1, AUDIO_DIRECT = 2, AUDIO_BB = 3 };   /* DIRECT: MCU tone (PB9), broadcast receiver */
void audio_route(int mode);
#define TX_PTT_LINE_PORT   GPIOD   /* PD6 = baseband TX-state input (EXTI6, stock FUN_08061770) - not used by the cfw */
#define TX_PTT_LINE        6

/* SCT3288 baseband on SPI2 (stock FUN_0806207c init, FUN_0810a674 write, FUN_0810a6dc read) */
#define BB_CS_PORT         GPIOB
#define BB_CS              12
#define BB_RDY_PORT        GPIOD
#define BB_RDY             11      /* byte ready */
#define BB_PEND            7       /* frame pending */
#define BB_RST             9       /* PD9: high only for a re-init (stock FUN_080f4698), low while running */
#define BB_NRST            8       /* PD8: low pulses at power on, high while running (FUN_080d6f5c) */
#define BB_PD10            10      /* PD10: low (FUN_080d6f5c; FUN_080f2500 sets it, use unknown) */
#define BB_PWR_PORT        GPIOC
#define BB_PWR             6       /* PC6: high while running (FUN_080d6f5c), low = baseband off (FUN_080d7964) */


/* analog outputs (drivers/dac.c) - D890UV only, not part of the shared HAL */
bool     dac_cal_load(void);              /* RF calibration block NAND 0x04F00000 / 0x04F40000 -> APC + tune tables */
bool     dac_cal_ok(void);
uint16_t dac_apc_value(uint32_t hz);      /* stock FUN_0811a75c for the cfw power level (CFW_APC_LEVEL) */
uint16_t dac_tune_value(uint32_t hz);     /* stock FUN_0811c8f0 (UHF, PA5) / FUN_0811cadc (VHF, RF reg 0x5C) */
void     dac_rx_tune(uint32_t hz);        /* called by rf_set_freq(0, hz) */

/* VOX: the radio's own detector (hal.h vox_hw_*), no MCU mic input (stock reads none either) */
#endif
