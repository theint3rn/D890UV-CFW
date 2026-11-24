/* Key matrix + side keys.
 * Matrix scan = stock FUN_08101260: drive one row PE7..PE14 low (others high), read the active-low columns
 * PC4 / PB1 / PB2. Table below gives the stock return code for (row, column). The rows are the LCD data
 * lines, so a scan only runs while the LCD bus is idle (stock checks FUN_080cb08c first).
 * Side keys = stock FUN_0811d9cc: PTT PA10 (bit 0), PF3 PA8 (bit 1), PF1 PE2 (bit 2), PF2 PA14 (bit 3). */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

static const uint8_t k_matrix[8][3] = {
    /* col:  PC4     PB1        PB2   */
    { KEY_1, KEY_MENU, KEY_P1 },     /* PE7  */
    { KEY_2, KEY_UP,   KEY_P2 },     /* PE8  */
    { KEY_3, KEY_DOWN, 0 },          /* PE9  */
    { KEY_4, KEY_7,    0 },          /* PE10 */
    { KEY_5, KEY_8,    0 },          /* PE11 */
    { KEY_6, KEY_9,    0 },          /* PE12 */
    { KEY_0, KEY_HASH, 0 },          /* PE13 */
    { KEY_STAR, KEY_EXIT, 0 },       /* PE14 */
};

#define QLEN 16
static volatile uint8_t q[QLEN];
static volatile uint8_t q_head, q_tail;

void keys_post(int ev)
{
    uint32_t p = irq_save();
    uint8_t n = (uint8_t)((q_head + 1) % QLEN);
    if(n != q_tail) { q[q_head] = (uint8_t)ev; q_head = n; }
    irq_restore(p);
}

int keys_get(void)
{
    uint32_t p = irq_save();
    int ev = KEY_NONE;
    if(q_tail != q_head) { ev = q[q_tail]; q_tail = (uint8_t)((q_tail + 1) % QLEN); }
    irq_restore(p);
    return ev;
}

void keys_init(void)
{
    q_head = q_tail = 0;
}

uint8_t keys_matrix_raw(void)
{
    if(lcd_busy()) { return 0; }
    uint8_t found = 0;
    for(int r = 0; r < 8 && !found; r++) {
        uint32_t row = 1u << (LCD_DATA_SHIFT + r);
        uint32_t all = 0xFFu << LCD_DATA_SHIFT;
        GPIO_BSRR(LCD_DATA_PORT) = (all & ~row) | (row << 16);
        delay_us(20);                                    /* stock: FUN_08066164(100) settle */
        bool c0 = !gpio_read(KEY_COL0_PORT, KEY_COL0);
        bool c1 = !gpio_read(KEY_COL1_PORT, KEY_COL1);
        bool c2 = !gpio_read(KEY_COL2_PORT, KEY_COL2);
        if(c0) { found = k_matrix[r][0]; }
        else if(c1) { found = k_matrix[r][1]; }
        else if(c2) { found = k_matrix[r][2]; }
    }
    GPIO_BSRR(LCD_DATA_PORT) = 0xFFu << LCD_DATA_SHIFT;  /* rows idle high */
    return found;
}

#ifdef CFW_PROG_PA9
/* make HW=1 HW_SAVE=1: PA10 is the CPS link's RX (stock USART1 PA9/PA10), so PTT is not read (that build has no TX) */
#define PTT_LOW() false
#else
#define PTT_LOW() (!gpio_read(PIN_PTT_PORT, PIN_PTT))
#endif
bool key_ptt_down(void) { return PTT_LOW(); }

static uint8_t side_raw(void)
{
    uint8_t b = 0;
    if(PTT_LOW()) { b |= 1; }
    if(!gpio_read(PIN_PF3_PORT, PIN_PF3)) { b |= 2; }
    if(!gpio_read(PIN_PF1_PORT, PIN_PF1)) { b |= 4; }
#ifndef CFW_KEEP_SWD
    if(!gpio_read(PIN_PF2_PORT, PIN_PF2)) { b |= 8; }
#endif
    return b;
}

void keys_poll(void)
{
    static uint8_t m_last, m_stable, m_cnt;
    static uint32_t m_since;
    static bool m_long;
    static uint8_t s_last, s_stable;
    static const uint8_t side_code[4] = { KEY_PTT, KEY_PF3, KEY_PF1, KEY_PF2 };

    uint8_t m = keys_matrix_raw();
    if(m != m_last) { m_last = m; m_cnt = 0; }
    else if(m_cnt < 2 && ++m_cnt == 2 && m != m_stable) {   /* stable for 2 polls */
        if(m_stable) { keys_post(m_stable | KEY_RELEASED); }
        m_stable = m;
        m_long = false;
        m_since = hal_millis();
        if(m) { keys_post(m); }
    }
    if(m_stable && !m_long && hal_millis() - m_since > 1000) {
        m_long = true;
        keys_post(m_stable | KEY_LONG);
    }

    uint8_t s = side_raw();
    if(s == s_last && s != s_stable) {
        uint8_t ch = s ^ s_stable;
        for(int i = 0; i < 4; i++) {
            if(ch & (1u << i)) { keys_post(side_code[i] | ((s & (1u << i)) ? 0 : KEY_RELEASED)); }
        }
        s_stable = s;
    }
    s_last = s;
}
