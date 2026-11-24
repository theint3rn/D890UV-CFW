/* Minimal STM32U575 register map for the D890UV custom firmware.
 * Only what the cfw drivers use. Offsets from RM0456 (rm0456-*.pdf in the project root). */
#ifndef CFW_STM32U5_H
#define CFW_STM32U5_H
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REG16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define REG8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

/* ---- Cortex-M33 system ---- */
#define SCB_VTOR      REG32(0xE000ED08)
#define SCB_AIRCR     REG32(0xE000ED0C)
#define SCB_SCR       REG32(0xE000ED10)
#define SCB_CPACR     REG32(0xE000ED88)
#define SYST_CSR      REG32(0xE000E010)
#define SYST_RVR      REG32(0xE000E014)
#define SYST_CVR      REG32(0xE000E018)
#define NVIC_ISER(n)  REG32(0xE000E100 + 4 * (n))
#define NVIC_ICER(n)  REG32(0xE000E180 + 4 * (n))
#define NVIC_ICPR(n)  REG32(0xE000E280 + 4 * (n))
#define NVIC_IPR8(n)  REG8(0xE000E400 + (n))

static inline void nvic_enable(int irq)  { NVIC_ICPR(irq >> 5) = 1u << (irq & 31); NVIC_ISER(irq >> 5) = 1u << (irq & 31); }
static inline void nvic_disable(int irq) { NVIC_ICER(irq >> 5) = 1u << (irq & 31); }
static inline uint32_t irq_save(void) { uint32_t p; __asm volatile("mrs %0, primask; cpsid i" : "=r"(p) :: "memory"); return p; }
static inline void irq_restore(uint32_t p) { __asm volatile("msr primask, %0" :: "r"(p) : "memory"); }
static inline void wfi(void) { __asm volatile("dsb; wfi" ::: "memory"); }
static inline void dsb(void) { __asm volatile("dsb" ::: "memory"); }
static inline void isb(void) { __asm volatile("isb" ::: "memory"); }

/* ---- RCC @0x46020C00 ---- */
#define RCC_BASE      0x46020C00u
#define RCC_CR        REG32(RCC_BASE + 0x00)
#define RCC_CFGR1     REG32(RCC_BASE + 0x1C)
#define RCC_CFGR2     REG32(RCC_BASE + 0x20)
#define RCC_CFGR3     REG32(RCC_BASE + 0x24)
#define RCC_PLL1CFGR  REG32(RCC_BASE + 0x28)
#define RCC_PLL1DIVR  REG32(RCC_BASE + 0x34)
#define RCC_AHB1ENR   REG32(RCC_BASE + 0x88)
#define RCC_AHB2ENR1  REG32(RCC_BASE + 0x8C)
#define RCC_AHB3ENR   REG32(RCC_BASE + 0x94)
#define RCC_APB1ENR1  REG32(RCC_BASE + 0x9C)
#define RCC_APB2ENR   REG32(RCC_BASE + 0xA4)
#define RCC_APB3ENR   REG32(RCC_BASE + 0xA8)
#define RCC_CCIPR1    REG32(RCC_BASE + 0xE0)
#define RCC_CCIPR3    REG32(RCC_BASE + 0xE8)

/* ---- PWR @0x46020800 ---- */
#define PWR_BASE      0x46020800u
#define PWR_VOSR      REG32(PWR_BASE + 0x0C)

/* ---- FLASH @0x40022000 (non-secure) ---- */
#define FLASH_BASE_R  0x40022000u
#define FLASH_ACR     REG32(FLASH_BASE_R + 0x00)
#define FLASH_NSKEYR  REG32(FLASH_BASE_R + 0x08)
#define FLASH_NSSR    REG32(FLASH_BASE_R + 0x20)
#define FLASH_NSCR    REG32(FLASH_BASE_R + 0x28)

/* ---- GPIO ---- */
#define GPIOA 0x42020000u
#define GPIOB 0x42020400u
#define GPIOC 0x42020800u
#define GPIOD 0x42020C00u
#define GPIOE 0x42021000u
#define GPIO_MODER(p)   REG32((p) + 0x00)
#define GPIO_OTYPER(p)  REG32((p) + 0x04)
#define GPIO_OSPEEDR(p) REG32((p) + 0x08)
#define GPIO_PUPDR(p)   REG32((p) + 0x0C)
#define GPIO_IDR(p)     REG32((p) + 0x10)
#define GPIO_ODR(p)     REG32((p) + 0x14)
#define GPIO_BSRR(p)    REG32((p) + 0x18)
#define GPIO_AFRL(p)    REG32((p) + 0x20)
#define GPIO_AFRH(p)    REG32((p) + 0x24)
#define GPIO_BRR(p)     REG32((p) + 0x28)

/* ---- EXTI @0x46022000 ---- */
#define EXTI_BASE     0x46022000u
#define EXTI_RTSR1    REG32(EXTI_BASE + 0x00)
#define EXTI_FTSR1    REG32(EXTI_BASE + 0x04)
#define EXTI_RPR1     REG32(EXTI_BASE + 0x0C)
#define EXTI_FPR1     REG32(EXTI_BASE + 0x10)
#define EXTI_EXTICR(n) REG32(EXTI_BASE + 0x60 + 4 * (n))   /* n = 0..3 (EXTICR1..4) */
#define EXTI_IMR1     REG32(EXTI_BASE + 0x80)

/* ---- ADC1 @0x42028000 ---- */
#define ADC1_BASE     0x42028000u
#define ADC1_ISR      REG32(ADC1_BASE + 0x00)
#define ADC1_CR       REG32(ADC1_BASE + 0x08)
#define ADC1_CFGR1    REG32(ADC1_BASE + 0x0C)
#define ADC1_SMPR1    REG32(ADC1_BASE + 0x14)
#define ADC1_SMPR2    REG32(ADC1_BASE + 0x18)
#define ADC1_PCSEL    REG32(ADC1_BASE + 0x1C)
#define ADC1_SQR1     REG32(ADC1_BASE + 0x30)
#define ADC1_SQR2     REG32(ADC1_BASE + 0x34)
#define ADC1_DR_ADDR  (ADC1_BASE + 0x40)
#define ADC12_CCR     REG32(0x42028308u)

/* ---- GPDMA1 @0x40020000, channel x at 0x50 + 0x80*x ---- */
#define GPDMA1_BASE   0x40020000u
#define GPDMA_C(x, off) REG32(GPDMA1_BASE + 0x50 + 0x80 * (x) + (off))
#define GPDMA_LBAR  0x00
#define GPDMA_FCR   0x0C
#define GPDMA_SR    0x10
#define GPDMA_CR    0x14
#define GPDMA_TR1   0x40
#define GPDMA_TR2   0x44
#define GPDMA_BR1   0x48
#define GPDMA_SAR   0x4C
#define GPDMA_DAR   0x50
#define GPDMA_LLR   0x7C

/* ---- SPI2 @0x40003800 (U5 "SPI v2" IP) ---- */
#define SPI2_BASE     0x40003800u
#define SPI2_CR1      REG32(SPI2_BASE + 0x00)
#define SPI2_CR2      REG32(SPI2_BASE + 0x04)
#define SPI2_CFG1     REG32(SPI2_BASE + 0x08)
#define SPI2_CFG2     REG32(SPI2_BASE + 0x0C)
#define SPI2_SR       REG32(SPI2_BASE + 0x14)
#define SPI2_IFCR     REG32(SPI2_BASE + 0x18)
#define SPI2_TXDR8    REG8(SPI2_BASE + 0x20)
#define SPI2_RXDR8    REG8(SPI2_BASE + 0x30)

#endif
