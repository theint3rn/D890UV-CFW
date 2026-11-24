/* ADC1: 5-channel scan at 48 kHz (TIM15 TRGO) into a circular GPDMA ring.
 *
 * Stock FUN_0805c4ac scans ch1, ch2 (battery), ch3, ch12, ch19 (temperature) continuously into a 5-halfword
 * DMA buffer (DAT_20001d34). The cfw keeps the same channels but triggers each scan from TIM15 at 48 kHz and
 * keeps ADC_RING_FRAMES (3840 = 80 ms) scans, because ch1 is the receiver's FM discriminator / audio: the MCU-DSP
 * demodulators (cfw/dsp) read it from the ring. Frame k = ring[5k .. 5k+4] = ch1, batt, ch3, ch12, temp.
 *   ADC clock HSE 12 MHz, 12 bit, sample time 20 cycles: 5 x 32.5 cycles = 13.5 us per scan (< 20.8 us).
 *   TIM15 on APB2 120 MHz: PSC 0, ARR 2499 -> 48 kHz, TRGO = update; ADC1 EXTSEL 14 (tim15_trgo), rising.
 *   GPDMA1 channel 0, linked list pointing to itself (circular), BR1 = whole ring. The write position is
 *   C0BR1.BNDT (bytes left in the block), so the reader needs no interrupt.
 * Battery = stock FUN_08039024: V*100 = (raw * 0x528 >> 12) + cal - 100, read from frame 0
 * (cal = DAT_200152d2 from NAND in stock; 100 = neutral until the location is decoded).
 * Emulator: models/Analog.cs writes the static scan values into frame 0 (GPDMA channel with SAR = ADC1_DR);
 * the ch1 sample stream injector (analog Stream, cfw-emu session) fills the ring and BNDT. */
#include "stm32u5.h"
#include "hal.h"

static volatile uint16_t s_ring[ADC_RING_FRAMES * 5] __attribute__((aligned(4)));
static uint8_t s_cal = 100;

/* GPDMA linked-list node: reload BR1 + DAR each pass (circular). Must live in RAM, same 64 KB as LBAR. */
static volatile uint32_t s_node[3] __attribute__((aligned(4)));

#define LLR_UB1 (1u << 29)
#define LLR_UDA (1u << 27)
#define LLR_ULL (1u << 16)

#define TIM15_BASE 0x40014000u
#define TIM15_CR1  REG32(TIM15_BASE + 0x00)
#define TIM15_CR2  REG32(TIM15_BASE + 0x04)
#define TIM15_EGR  REG32(TIM15_BASE + 0x14)
#define TIM15_PSC  REG32(TIM15_BASE + 0x28)
#define TIM15_ARR  REG32(TIM15_BASE + 0x2C)

void battery_init(void)
{
    RCC_AHB1ENR |= 1u << 0;                        /* GPDMA1 */
    RCC_AHB2ENR1 |= 1u << 10;                      /* ADC12 */
    RCC_APB2ENR |= 1u << 16;                       /* TIM15 */
    (void)RCC_APB2ENR;

    /* analog pins (U575 datasheet): ADC1 IN1 = PC0, IN2 = PC1, IN3 = PC2, IN12 = PA7 */
    gpio_mode(GPIOC, 0, GPIO_ANALOG, PULL_NONE);
    gpio_mode(GPIOC, 1, GPIO_ANALOG, PULL_NONE);
    gpio_mode(GPIOC, 2, GPIO_ANALOG, PULL_NONE);
    gpio_mode(GPIOA, 7, GPIO_ANALOG, PULL_NONE);

    ADC12_CCR |= 1u << 23;                         /* VSENSEEN: temperature sensor on ch19 */
    ADC1_CR &= ~(1u << 29);                        /* DEEPPWD off */
    ADC1_CR |= 1u << 28;                           /* ADVREGEN */
    for(uint32_t n = 0; n < 100000u && !(ADC1_ISR & (1u << 12)); n++) { }   /* LDORDY */
    delay_us(20);
    ADC1_CR |= 1u << 31;                           /* ADCAL */
    for(uint32_t n = 0; n < 1000000u && (ADC1_CR & (1u << 31)); n++) { }

    /* DMNGT = DMA circular, RES = 12 bit, EXTSEL = 14 (tim15_trgo), EXTEN = rising, single scan per trigger */
    ADC1_CFGR1 = (3u << 0) | (1u << 2) | (14u << 5) | (1u << 10);
    ADC1_PCSEL = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 12) | (1u << 19);
    ADC1_SMPR1 = 0x1B6DB6DBu;                      /* 20 cycles, ch0..9 */
    ADC1_SMPR2 = 0x1B6DB6DBu;                      /* ch10..19 */
    ADC1_SQR1 = (5u - 1) | (1u << 6) | (2u << 12) | (3u << 18) | (12u << 24);
    ADC1_SQR2 = 19u;

    /* GPDMA1 channel 0: ADC1_DR (halfword) -> ring (halfword, incrementing), circular */
    uint32_t lla = (uint32_t)(uintptr_t)s_node;
    s_node[0] = sizeof s_ring;                                     /* BR1 */
    s_node[1] = (uint32_t)(uintptr_t)s_ring;                       /* DAR */
    s_node[2] = LLR_UB1 | LLR_UDA | LLR_ULL | (lla & 0xFFFCu);     /* LLR -> itself */
    GPDMA_C(0, GPDMA_CR) = 0;
    GPDMA_C(0, GPDMA_FCR) = 0x7F00u;
    GPDMA_C(0, GPDMA_LBAR) = lla & 0xFFFF0000u;
    GPDMA_C(0, GPDMA_TR1) = (1u << 0) | (1u << 16) | (1u << 19);   /* SDW halfword, DDW halfword, DINC */
    GPDMA_C(0, GPDMA_TR2) = 0;                                     /* REQSEL 0 = adc1_dma */
    GPDMA_C(0, GPDMA_BR1) = sizeof s_ring;
    GPDMA_C(0, GPDMA_SAR) = ADC1_DR_ADDR;
    GPDMA_C(0, GPDMA_DAR) = (uint32_t)(uintptr_t)s_ring;
    GPDMA_C(0, GPDMA_LLR) = s_node[2];
    GPDMA_C(0, GPDMA_CR) = 1u;                                     /* EN */

    ADC1_ISR = 0xFFFu;
    ADC1_CR |= 1u << 0;                            /* ADEN */
    for(uint32_t n = 0; n < 100000u && !(ADC1_ISR & 1u); n++) { }
    ADC1_CR |= 1u << 2;                            /* ADSTART: waits for the triggers */

    /* TIM15: 48 kHz update -> TRGO */
    TIM15_CR1 = 0;
    TIM15_PSC = 0;
    TIM15_ARR = 120000000u / ADC_FS - 1u;
    TIM15_CR2 = 2u << 4;                           /* MMS = update */
    TIM15_EGR = 1u;
    TIM15_CR1 = 1u;                                /* CEN */
}

const volatile uint16_t *adc_ring(void) { return s_ring; }

uint32_t adc_write_frame(void)
{
    uint32_t left = GPDMA_C(0, GPDMA_BR1) & 0xFFFFu;            /* BNDT */
    uint32_t total = sizeof s_ring;
    if(left == 0 || left > total) { return 0; }
    return ((total - left) / 10u) % ADC_RING_FRAMES;
}

uint16_t battery_raw(int i) { return (i >= 0 && i < 5) ? s_ring[i] : 0; }

uint16_t battery_centivolts(void)
{
    uint32_t raw = s_ring[1];
    int v = (int)((raw * 0x528u) >> 12) + s_cal - 100;
    return v < 0 ? 0 : (uint16_t)v;
}
