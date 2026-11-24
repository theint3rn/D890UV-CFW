/* D890UV cfw hardware abstraction layer: everything above this line is hardware-independent. */
#ifndef CFW_HAL_H
#define CFW_HAL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Board overrides: another board (D168UV/cfw) puts a board_cfg.h on its include path; the D890UV has none. */
#if defined(__has_include)
#if __has_include("board_cfg.h")
#include "board_cfg.h"
#endif
#endif
#include "safety.h"                      /* TX gate / cap, watchdog refresh, fault record (hal/safety.c) */

/* ---- clocks / time (drivers/clock.c, drivers/time.c) ---- */
void     clock_init(void);              /* HSE 12 MHz -> PLL -> 120 MHz; on failure stays on the internal clock (g_clk_err) */
void     time_init(void);               /* SysTick = HCLK/8, 5 ms tick */
uint32_t hal_millis(void);              /* ms since boot (5 ms resolution) */
uint32_t hal_micros(void);              /* us since boot (SysTick based, wraps after 71 min) */
void     delay_us(uint32_t us);         /* busy wait (emulator skips it: symbol alias FUN_0811ece0) */
void     delay_ms(uint32_t ms);
#define  HAL_TICK_MS 5u
extern volatile uint32_t g_ticks;       /* 5 ms ticks */

/* ---- GPIO helpers (drivers/gpio.c) ---- */
enum { GPIO_IN = 0, GPIO_OUT = 1, GPIO_AF = 2, GPIO_ANALOG = 3 };
enum { PULL_NONE = 0, PULL_UP = 1, PULL_DOWN = 2 };
void gpio_mode(uint32_t port, int pin, int mode, int pull);
void gpio_af(uint32_t port, int pin, int af);
#ifndef CFW_BOARD_GPIO_INLINE            /* STM32U5 GPIO; a board_cfg.h may bring its own */
static inline void gpio_set(uint32_t port, int pin) { *(volatile uint32_t *)(port + 0x18) = 1u << pin; }
static inline void gpio_clr(uint32_t port, int pin) { *(volatile uint32_t *)(port + 0x18) = 1u << (pin + 16); }
static inline void gpio_write(uint32_t port, int pin, bool v) { if(v) { gpio_set(port, pin); } else { gpio_clr(port, pin); } }
static inline bool gpio_read(uint32_t port, int pin) { return (*(volatile uint32_t *)(port + 0x10) >> pin) & 1; }
#endif
void gpio_init_board(void);             /* clocks + every pin's mode */

/* ---- LCD 160x128 RGB565 (drivers/lcd.c) ---- */
#ifndef LCD_W                           /* a board_cfg.h may set another panel size (D168UV: 128 x 160) */
#define LCD_W 160
#define LCD_H 128
#endif
void lcd_init(void);
void lcd_blit(int x0, int y0, int x1, int y1, const uint16_t *fb, int stride);  /* panel coords, inclusive */
void lcd_fill(int x0, int y0, int x1, int y1, uint16_t colour);
void lcd_on(bool on);
bool lcd_busy(void);                    /* true while the bus is in use (key scan must not touch PE7..14) */

/* ---- keys (drivers/keys.c), codes as the stock FUN_08101260 / FUN_0811d9cc ---- */
enum {
    KEY_NONE = 0, KEY_0 = 1, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,  /* 1..10 */
    KEY_STAR = 11, KEY_HASH = 12, KEY_MENU = 13, KEY_EXIT = 14, KEY_UP = 15, KEY_DOWN = 16,
    KEY_KNOB_CW = 0x11, KEY_KNOB_CCW = 0x12,   /* posted by the encoder ISR */
    KEY_P1 = 19, KEY_P2 = 20, KEY_PF1 = 21, KEY_PF2 = 22, KEY_PF3 = 23,
    KEY_PTT = 24,
};
#define KEY_RELEASED 0x80                 /* or-ed into the code for a release event */
#define KEY_LONG     0x40                 /* or-ed in once when held > 1 s */
void    keys_init(void);
void    keys_poll(void);                  /* call every 10-20 ms: matrix + side keys, debounced */
int     keys_get(void);                   /* next event or KEY_NONE */
void    keys_post(int ev);                /* ISR-safe */
uint8_t keys_matrix_raw(void);            /* one scan, stock order (0 = none) */
bool    key_ptt_down(void);

/* ---- channel encoder (drivers/encoder.c) ---- */
void    encoder_init(void);
int32_t encoder_count(void);              /* detents, + = clockwise */

/* ---- power (drivers/power.c) ---- */
void power_hold(void);                    /* PC3 high: keep the radio on */
void power_poll(void);                    /* call every 20 ms: PA0 off-request -> callbacks, PC3 low, reset */
void power_set_save_hook(void (*fn)(void));
bool power_armed(void);                   /* power key released since boot */
void power_off(void);                     /* save, drop latch, reset (stock FUN_080d6954 / FUN_0813b98c) */

/* ---- battery (drivers/battery.c) ---- */
void     battery_init(void);
uint16_t battery_centivolts(void);        /* V*100, stock FUN_08039024 */
uint16_t battery_raw(int index);          /* ADC1 scan slot 0..4 (ch1, ch2=batt, ch3, ch12, ch19=temp) */
/* ADC1 scan ring (drivers/adc.c): frame k = ring[5k..5k+4], ch1 = FM discriminator for the MCU-DSP demods */
#ifndef  ADC_FS
#define  ADC_FS          48000u
#endif
#ifndef  ADC_RING_FRAMES
#define  ADC_RING_FRAMES 3840u              /* 80 ms */
#endif
#ifndef  ADC_FRAME_CH
#define  ADC_FRAME_CH    5u                 /* halfwords per scan frame; slot 0 = discriminator */
#endif
const volatile uint16_t *adc_ring(void);
uint32_t adc_write_frame(void);           /* next frame the DMA writes (0..ADC_RING_FRAMES-1) */
#define  BATTERY_LOW_CV 601

/* ---- TX audio + APC (drivers/dac.c): D890UV = TIM4_CH4 PWM on PB9 (stock MCU audio output, docs/AUDIO_PATHS.md),
 *      APC on DAC1 ch1 PA4; the ring holds CCR4 values ---- */
#ifndef  DAC_CODE                         /* 12-bit sample (0..4095) -> value the TX DMA writes; board_cfg.h may override */
#define  DAC_CODE(c) ((uint16_t)(((uint32_t)(c) * 2500u + 2048u) >> 12))   /* D890UV: TIM4 period 2500 (48 kHz) */
#endif
#ifndef  DAC_RING
#define  DAC_RING 3840u
#endif
                                     /* DAC_RING: 80 ms at 48 kHz (UI redraws / vocoder bursts must not underrun it) */
void     audio_tone(bool on);              /* speaker on the MCU tone output (beeps) / back to the previous source */
bool     audio_tone_muted(void);           /* D890UV: volume knob at 0 (stock plays no tone then); D168UV: false */
void     audio_volume_limit(uint8_t maxvol); /* stock Vol Level 0 = Indoor, 1..8 (D168UV: baseband attenuation,
                                              * D890UV: I2C volume pot table[maxvol][knob]) - see docs/AUDIO_PATHS.md */
uint8_t  audio_codec_att(void);            /* SCT3288 codec speaker attenuation 0..0x1F (D168UV: from the volume limit) */
void     audio_poll(void);                 /* D890UV: follows the volume knob (pot); D168UV: no-op */

/* ---- VOX detector (stock: the hardware decides, the MCU only polls) ----
 *      D168UV: RF chip regs 0x12/0x13 thresholds, detect line PD3 low; D890UV: UART4 NR/BT module, status cmd 3 */
void     vox_hw_config(uint8_t level);     /* 0 = off, 1..3 = stock VOX level */
bool     vox_hw_active(void);              /* voice present now */

/* ---- GPS receiver configuration (stock $CFGSYS / $CFGMSG lines on USART2; no power switch on either radio) ---- */
void     gps_uart_send_cfg(void);

/* ---- bit-banged I2C bus (both radios: SDA PB6, SCL PB7; drivers/i2c.c), 7-bit addresses ---- */
bool     i2c_reg_write(uint8_t a7, uint8_t reg, uint8_t v);
bool     i2c_reg_read(uint8_t a7, uint8_t reg, uint8_t *v);

/* ---- accelerometer (LIS2DW12 0x19 on that bus, man down; hal/accel.c) ---- */
bool     accel_init(void);                 /* true = WHO_AM_I 0x44 answered */
int      accel_upright(void);              /* 1 = SIXD_SRC XL set (radio standing), 0 = not, -1 = no new data / no chip */
void     dac_apc(bool tx);                   /* TX: APC level + TX enables (stock), RX: 0 */
void     dac_stream_start(void);             /* TX audio from the ring (digital modes only with CFW_DIGITAL_TX) */
void     dac_stream_stop(void);
volatile uint16_t *dac_ring(void);
uint32_t dac_read_pos(void);                 /* next ring index the DMA reads */

void     dac_set_power(uint8_t level);        /* TX power of the channel: 0 low, 1 mid, 2 high, 3 turbo (D890UV: stock APC
                                              * calibration table of that level; D168UV: stored, APC level fixed) */
uint8_t  dac_power(void);

/* ---- LCD backlight (D890UV: PB0 TIM3_CH3 PWM; D168UV: not mapped, no-op) ---- */
void     backlight_set(uint8_t level);       /* 0 off, 1..5 = stock Light 1..5 */

/* ---- RTC (battery backed calendar, set up by the stock firmware) ---- */
uint32_t rtc_now(void);                      /* seconds since 1970 (local time, as the radio shows it), 0 = invalid */
void     rtc_set(uint32_t t);
uint32_t cal_days(int y, int m, int d);      /* days since 1970-01-01 */
void     cal_date(uint32_t days, int *y, int *m, int *d);

/* ---- SPI-NAND, read only (drivers/nand.c) ---- */
bool     nand_init(void);                 /* reads ID; true if Kioxia 98 ED/CD */
uint16_t nand_id(void);
bool     nand_read(uint32_t addr, void *buf, uint32_t len);   /* CPS address == NAND address */
bool     nand_page_load(uint32_t addr);   /* page containing addr -> chip cache (0x13) */
void     nand_cache_read(uint32_t col, void *buf, uint32_t n);  /* from the cache (0x03), col < 4096 */

/* ---- RF transceivers (drivers/rf.c) ---- */
bool     rf_init(int chip);               /* self-test + base register set; true if chip answers */
void     rf_write(int chip, uint8_t reg, uint16_t val);
uint16_t rf_read(int chip, uint8_t reg);
void     rf_set_freq(int chip, uint32_t hz);
void     rf_rx_fm(int chip, bool wide);   /* analog FM receive */
void     rf_tx_fm(int chip, bool on);     /* analog FM transmit (see README: TX register values unverified) */
void     rf_set_ctcss(int chip, uint16_t tone_dhz);   /* 0 = off; tone in 0.1 Hz */
void     rf_idle(int chip);
/* Receive status, the registers the stock squelch reads (FUN_080d22f0, see drivers/rf.c). */
uint16_t rf_rssi(int chip);              /* reg 0x02 [9:0], raw */
bool     rf_carrier(int chip);           /* reg 0x0E bit 14 */
int      rf_rssi_dbm(uint16_t raw);      /* raw -> dBm, ASSUMED scale (0.5 dB steps, raw 0 = -160 dBm) */
uint16_t rf_noise(int chip);             /* reg 0x01: [15:8] noise, [3:0] glitch count */
void     rf_set_sql(int level);          /* chip 0 squelch level 0..5: reg 0x49 = stock threshold for the frequency */
uint16_t rf_sql_thr(void);               /* chip 0 threshold last written to reg 0x49 (RSSI raw units) */
uint16_t dac_sql_threshold(uint32_t hz, int level);   /* stock squelch calibration (drivers/dac.c) */

/* ---- SCT3288 baseband link (drivers/baseband.c) ---- */
#define  BB_MAX_DATA 400u                /* largest frame payload (vocoder PCM = 0xA0 + 320 bytes) */
typedef void (*bb_msg_cb)(uint8_t b0, uint8_t id, const uint8_t *data, uint16_t len);
void     bb_init(void);                   /* SPI2 + state machine */
void     bb_poll(void);                   /* call every 5-10 ms: drains frames, advances init */
bool     bb_ready(void);
const char *bb_status(void);              /* short text for the UI (ready: what the vocoder probe found) */
bool     bb_send(uint8_t b0, uint8_t id, const uint8_t *data, uint16_t len);   /* allow-list: hal/bb_allow.h */
uint16_t bb_refused(uint8_t *b0, uint8_t *id);   /* commands refused so far + the last one */
void     bb_set_listener(bb_msg_cb cb);   /* all frames after init go here */
const uint8_t *bb_version(void);          /* 9 bytes from cmd 0x33, valid once ready */

/* ---- settings page in internal flash (drivers/settings.c) ----
 * One opaque blob (the app's persisted state), appended as a new record on every save; the newest record
 * with a good CRC wins. version must match on load (format changes simply start from defaults). */
#define SETTINGS_MAX 4096u
uint32_t settings_load(uint32_t version, void *buf, uint32_t max);   /* bytes loaded, 0 = none */
bool     settings_save(uint32_t version, const void *buf, uint32_t len);

#endif
