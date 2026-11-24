#include "gfx.h"
#include "hal.h"

extern const uint8_t g_font5x7[95][7];

/* Draw into `back`; gfx_flush() diffs it against `front` (= what the panel shows) and sends only the
 * bounding box of changed pixels. Lets the UI redraw everything each frame without bus traffic. */
static uint16_t back[LCD_H][LCD_W];
#ifndef CFW_GFX_ROWHASH
static uint16_t front[LCD_H][LCD_W];
#else
/* small-RAM boards (D168UV): one hash per row instead of a copy of the panel; changed rows go out whole */
static uint32_t s_rowhash[LCD_H];
#endif
static bool s_force;

static inline void px(int x, int y, uint16_t c)
{
    if((unsigned)x < LCD_W && (unsigned)y < LCD_H) { back[y][x] = c; }
}

void gfx_clear(uint16_t c) { gfx_rect(0, 0, LCD_W, LCD_H, c); }

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    for(int j = y; j < y + h; j++) {
        for(int i = x; i < x + w; i++) { px(i, j, c); }
    }
}

void gfx_frame(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    for(; *s; s++) {
        unsigned ch = (unsigned char)*s;
        const uint8_t *g = g_font5x7[(ch >= 0x20 && ch <= 0x7E) ? ch - 0x20 : '?' - 0x20];
        for(int r = 0; r < 8; r++) {
            uint8_t bits = r < 7 ? g[r] : 0;
            for(int c = 0; c < 6; c++) {
                bool on = c < 5 && (bits & (0x10 >> c));
                gfx_rect(x + c * scale, y + r * scale, scale, scale, on ? fg : bg);
            }
        }
        x += 6 * scale;
    }
    return x;
}

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    while(*s++) { n++; }
    return n * 6 * scale;
}

void gfx_text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    gfx_text((LCD_W - gfx_text_width(s, scale)) / 2, y, s, fg, bg, scale);
}

void gfx_invalidate(void) { s_force = true; }

#ifdef CFW_GFX_ROWHASH
static uint32_t row_hash(const uint16_t *r)
{
    uint32_t h = 2166136261u;
    for(int x = 0; x < LCD_W; x++) { h = (h ^ r[x]) * 16777619u; }
    return h;
}

void gfx_flush(void)
{
    int y0 = -1;
    for(int y = 0; y <= LCD_H; y++) {
        bool changed = false;
        if(y < LCD_H) {
            uint32_t h = row_hash(back[y]);
            changed = h != s_rowhash[y] || s_force;
            s_rowhash[y] = h;
        }
        if(changed && y0 < 0) { y0 = y; }
        if(!changed && y0 >= 0) { lcd_blit(0, y0, LCD_W - 1, y - 1, &back[0][0], LCD_W); y0 = -1; }
    }
    s_force = false;
}
#else
void gfx_flush(void)
{
    int x0 = LCD_W, y0 = LCD_H, x1 = -1, y1 = -1;
    for(int y = 0; y < LCD_H; y++) {
        for(int x = 0; x < LCD_W; x++) {
            if(back[y][x] != front[y][x] || s_force) {
                front[y][x] = back[y][x];
                if(x < x0) { x0 = x; }
                if(x > x1) { x1 = x; }
                if(y < y0) { y0 = y; }
                if(y > y1) { y1 = y; }
            }
        }
    }
    s_force = false;
    if(x1 >= 0) { lcd_blit(x0, y0, x1, y1, &front[0][0], LCD_W); }
}
#endif

char *fmt_u(char *p, uint32_t v, int min_digits)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while(v);
    while(n < min_digits) { t[n++] = '0'; }
    while(n) { *p++ = t[--n]; }
    *p = 0;
    return p;
}

char *fmt_s(char *p, const char *s)
{
    while(*s) { *p++ = *s++; }
    *p = 0;
    return p;
}

char *fmt_hex(char *p, uint32_t v, int digits)
{
    for(int i = digits - 1; i >= 0; i--) { *p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF]; }
    *p = 0;
    return p;
}
