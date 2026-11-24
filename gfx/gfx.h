/* RAM framebuffer (160x128 RGB565, panel coordinates) + dirty-rectangle flush to the LCD. */
#ifndef CFW_GFX_H
#define CFW_GFX_H
#include <stdint.h>
#include <stdbool.h>

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK  RGB(0, 0, 0)
#define C_WHITE  RGB(255, 255, 255)
#define C_GREY   RGB(110, 110, 110)
#define C_DGREY  RGB(40, 40, 48)
#define C_BLUE   RGB(30, 80, 200)
#define C_CYAN   RGB(0, 200, 220)
#define C_GREEN  RGB(40, 200, 60)
#define C_YELLOW RGB(250, 210, 0)
#define C_ORANGE RGB(255, 140, 0)
#define C_RED    RGB(230, 30, 30)

void gfx_clear(uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);          /* filled */
void gfx_frame(int x, int y, int w, int h, uint16_t c);         /* outline */
int  gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);   /* returns end x */
int  gfx_text_width(const char *s, int scale);
void gfx_text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void gfx_flush(void);                                           /* push the dirty area */
void gfx_invalidate(void);

/* tiny formatting helpers (no libc printf) */
char *fmt_u(char *p, uint32_t v, int min_digits);
char *fmt_s(char *p, const char *s);
char *fmt_hex(char *p, uint32_t v, int digits);

#endif
