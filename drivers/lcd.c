/* 160x128 TFT on the 8080 bus (ST7735-class controller, MIPI DCS).
 * Init sequence = stock FUN_0805d830 (reset PD13, 0x11 sleep-out, frame rate B1..B4, power C0..C5,
 * MADCTL 0x88, gamma E0/E1, 0x3A = 0x55 RGB565). Bus byte = stock FUN_0806e4c0 (PE7..PE14), command
 * FUN_080909a0 (D/C low), data FUN_08090944 (D/C high), window FUN_0808aba0.
 *
 * Panel orientation: the controller column axis (0x2A) is the panel Y axis (0..127) and the row axis
 * (0x2B) is the panel X axis (0..159) - stock FUN_0808aba0(x0, y0, x1, y1) writes 2A = y, 2B = x.
 * Pixels stream with Y fastest, i.e. column by column of the panel. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

static volatile bool s_busy;

#define WR_LO()  (GPIO_BSRR(LCD_CTRL_PORT) = 1u << (LCD_WR + 16))
#define WR_HI()  (GPIO_BSRR(LCD_CTRL_PORT) = 1u << LCD_WR)

static inline void bus_byte(uint8_t b)
{
    /* one BSRR write sets PE7..14 = b without touching PE15 (NAND SCK) or PE2 (PF1 key) */
    GPIO_BSRR(LCD_DATA_PORT) = ((uint32_t)b << LCD_DATA_SHIFT) | ((uint32_t)(uint8_t)~b << (LCD_DATA_SHIFT + 16));
    WR_LO();
    WR_HI();
}

static void cmd(uint8_t c)
{
    GPIO_BSRR(LCD_CTRL_PORT) = (1u << (LCD_DC + 16)) | (1u << (LCD_CS + 16));
    bus_byte(c);
    GPIO_BSRR(LCD_CTRL_PORT) = (1u << LCD_DC);
}

static void data(uint8_t d) { bus_byte(d); }

static void end(void) { GPIO_BSRR(LCD_CTRL_PORT) = 1u << LCD_CS; }

static void cmdn(uint8_t c, const uint8_t *p, int n)
{
    cmd(c);
    for(int i = 0; i < n; i++) { data(p[i]); }
    end();
}

/* window in panel coordinates (x 0..159, y 0..127), then RAMWR */
static void window(int x0, int y0, int x1, int y1)
{
    cmd(0x2A); data(0); data((uint8_t)y0); data(0); data((uint8_t)y1); end();
    cmd(0x2B); data(0); data((uint8_t)x0); data(0); data((uint8_t)x1); end();
    cmd(0x2C);
}

bool lcd_busy(void) { return s_busy; }

void lcd_init(void)
{
    static const uint8_t b1[] = {1, 1, 1}, b2[] = {1, 1, 1}, b3[] = {1, 1, 1, 1, 1, 1}, b4[] = {3};
    static const uint8_t c0[] = {0x28, 0x08, 0x84}, c1[] = {0xC0}, c2[] = {0x0C, 0x00}, c3[] = {0x8C, 0x2A};
    static const uint8_t c4[] = {0x8A, 0xEE}, c5[] = {0x0C}, madctl[] = {0x88}, colmod[] = {0x55};
    static const uint8_t e0[] = {0x05, 0x1A, 0x0C, 0x0E, 0x3A, 0x34, 0x2D, 0x2F, 0x2D, 0x2A, 0x2F, 0x3C, 0x00, 0x01, 0x02, 0x10};
    static const uint8_t e1[] = {0x04, 0x1B, 0x0D, 0x0E, 0x2D, 0x29, 0x24, 0x29, 0x28, 0x26, 0x31, 0x3B, 0x00, 0x00, 0x03, 0x12};

    s_busy = true;
    GPIO_BSRR(LCD_CTRL_PORT) = (1u << LCD_WR) | (1u << LCD_CS) | (1u << LCD_RST);
    delay_ms(100);
    gpio_clr(LCD_CTRL_PORT, LCD_RST);
    delay_ms(100);
    gpio_set(LCD_CTRL_PORT, LCD_RST);
    delay_ms(500);
    cmd(0x11); end();                 /* sleep out */
    delay_ms(120);                    /* stock: FUN_0811ec64(0x78) */
    cmdn(0xB1, b1, 3); cmdn(0xB2, b2, 3); cmdn(0xB3, b3, 6); cmdn(0xB4, b4, 1);
    cmdn(0xC0, c0, 3); cmdn(0xC1, c1, 1); cmdn(0xC2, c2, 2); cmdn(0xC3, c3, 2);
    cmdn(0xC4, c4, 2); cmdn(0xC5, c5, 1);
    cmdn(0x36, madctl, 1);
    cmdn(0xE0, e0, 16); cmdn(0xE1, e1, 16);
    cmdn(0x3A, colmod, 1);
    s_busy = false;
    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, 0x0000);
    lcd_on(true);
}

void lcd_on(bool on)
{
    s_busy = true;
    cmd(on ? 0x29 : 0x28); end();
    s_busy = false;
}

void lcd_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    s_busy = true;
    window(x0, y0, x1, y1);
    int n = (x1 - x0 + 1) * (y1 - y0 + 1);
    while(n--) { data((uint8_t)(c >> 8)); data((uint8_t)c); }
    end();
    s_busy = false;
}

void lcd_blit(int x0, int y0, int x1, int y1, const uint16_t *fb, int stride)
{
    s_busy = true;
    window(x0, y0, x1, y1);
    for(int x = x0; x <= x1; x++) {
        const uint16_t *p = fb + y0 * stride + x;
        for(int y = y0; y <= y1; y++, p += stride) {
            uint16_t c = *p;
            data((uint8_t)(c >> 8));
            data((uint8_t)c);
        }
    }
    end();
    s_busy = false;
}
