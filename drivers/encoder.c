/* Channel knob: quadrature encoder PC8 (A) / PC9 (B), both edges on EXTI8 / EXTI9 (IRQ 19 / 20).
 * Stock: IRQ19_Handler 0x08030a4c / IRQ20_Handler 0x08030b54 sample A and B, FUN_080c85cc decodes a
 * detent (two edges, 00 -> 11 or 11 -> 00) and posts key 0x11 / 0x12. Here: Gray-code state table,
 * one detent = two valid transitions in the same direction. */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

static volatile int32_t s_count;
static volatile uint8_t s_state;
static volatile int8_t s_acc;

/* s = A<<1 | B; clockwise (A leads): 00 -> 10 -> 11 -> 01 -> 00 */
static const int8_t k_step[16] = {
    /* from 00 */ 0, -1, +1, 0,
    /* from 01 */ +1, 0, 0, -1,
    /* from 10 */ -1, 0, 0, +1,
    /* from 11 */ 0, +1, -1, 0,
};

static void sample(void)
{
    uint8_t a = gpio_read(ENC_PORT, ENC_A), b = gpio_read(ENC_PORT, ENC_B);
    uint8_t now = (uint8_t)((a << 1) | b);
    int8_t d = k_step[(s_state << 2) | now];
    s_state = now;
    if(!d) { return; }
    if((s_acc > 0 && d < 0) || (s_acc < 0 && d > 0)) { s_acc = 0; }   /* direction change: restart */
    s_acc += d;
    if(s_acc >= 2) { s_acc = 0; s_count++; keys_post(KEY_KNOB_CW); }
    else if(s_acc <= -2) { s_acc = 0; s_count--; keys_post(KEY_KNOB_CCW); }
}

void EXTI8_IRQHandler(void)
{
    EXTI_RPR1 = 1u << ENC_A;
    EXTI_FPR1 = 1u << ENC_A;
    sample();
}

void EXTI9_IRQHandler(void)
{
    EXTI_RPR1 = 1u << ENC_B;
    EXTI_FPR1 = 1u << ENC_B;
    sample();
}

void encoder_init(void)
{
    s_state = (uint8_t)((gpio_read(ENC_PORT, ENC_A) << 1) | gpio_read(ENC_PORT, ENC_B));
    /* EXTICR3: EXTI8 bits 7:0, EXTI9 bits 15:8 = port C (2) */
    EXTI_EXTICR(2) = (EXTI_EXTICR(2) & ~0xFFFFu) | (2u << 0) | (2u << 8);
    EXTI_RTSR1 |= (1u << ENC_A) | (1u << ENC_B);
    EXTI_FTSR1 |= (1u << ENC_A) | (1u << ENC_B);
    EXTI_IMR1 |= (1u << ENC_A) | (1u << ENC_B);
    NVIC_IPR8(IRQ_EXTI8) = 0x80;
    NVIC_IPR8(IRQ_EXTI9) = 0x80;
    nvic_enable(IRQ_EXTI8);
    nvic_enable(IRQ_EXTI9);
}

int32_t encoder_count(void) { return s_count; }
