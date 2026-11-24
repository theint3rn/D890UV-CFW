/* Vector table + reset for the D890UV cfw. The stock bootloader (0x08000000) sets VTOR = 0x0800C000,
 * MSP = word 0, and jumps to word 1 (see emu/bootstub.S, which stands in for it in the emulator). */
#include <stdint.h>
#include <string.h>
#include "stm32u5.h"
#include "safety.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

#define WEAK __attribute__((weak, alias("Default_Handler")))
/* every fault / unexpected exception: PA off, record, reset (hal/safety.c fault_handle) */
#define FAULT __attribute__((alias("HardFault_Handler")))
void NMI_Handler(void) FAULT;
void MemManage_Handler(void) FAULT;
void BusFault_Handler(void) FAULT;
void UsageFault_Handler(void) FAULT;
void SecureFault_Handler(void) FAULT;
void SVC_Handler(void) WEAK;
void DebugMon_Handler(void) WEAK;
void PendSV_Handler(void) WEAK;
void SysTick_Handler(void) WEAK;
void EXTI8_IRQHandler(void) WEAK;    /* IRQ 19 (stock IRQ19_Handler 0x08030a4c) */
void EXTI9_IRQHandler(void) WEAK;    /* IRQ 20 (stock IRQ20_Handler 0x08030b54) */
void USART1_IRQHandler(void) WEAK;   /* IRQ 61: CPS programming link (app/prog.c) */
void USART2_IRQHandler(void) WEAK;   /* IRQ 62: GPS NMEA (app/gps.c, drivers/gps_uart.c) */
void UART4_IRQHandler(void) WEAK;    /* IRQ 64: NR/BT accessory module (drivers/btmod.c, VOX) */
void RTC_IRQHandler(void) WEAK;      /* IRQ 2 */

#define NIRQ 141
typedef void (*vec_t)(void);

__attribute__((section(".isr_vector"), used))
const vec_t g_vectors[16 + NIRQ] = {
    [2 ... 16 + NIRQ - 1] = Default_Handler,   /* overridden below (-Wno-override-init) */
    [0] = (vec_t)(uintptr_t)&_estack,
    [1] = Reset_Handler,
    [2] = NMI_Handler,
    [3] = HardFault_Handler,
    [4] = MemManage_Handler,
    [5] = BusFault_Handler,
    [6] = UsageFault_Handler,
    [7] = SecureFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 + 2] = RTC_IRQHandler,
    [16 + 19] = EXTI8_IRQHandler,
    [16 + 20] = EXTI9_IRQHandler,
    [16 + 61] = USART1_IRQHandler,
    [16 + 62] = USART2_IRQHandler,
    [16 + 64] = UART4_IRQHandler,
};

/* Bootloader hand-over (pre-flash audit F4): the stock bootloader's exit state is unknown, so do not trust it.
 * MSP from our own vector table, thread mode on MSP, SysTick off, every NVIC enable / pending bit cleared,
 * PendSV / SysTick pending cleared, before anything else runs. main() then sets the power latch before the clocks. */
__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile(
        "cpsid i\n"
        "ldr r0, =g_vectors\n"
        "ldr r0, [r0]\n"
        "msr msp, r0\n"
        "movs r0, #0\n"
        "msr control, r0\n"
        "isb\n"
        "b reset_c\n");
}

__attribute__((used, noreturn)) void reset_c(void)
{
    SCB_VTOR = 0x0800C000u;                          /* our vectors first: a fault from here on is ours */
    SYST_CSR = 0;
    for(int n = 0; n < 16; n++) { NVIC_ICER(n) = 0xFFFFFFFFu; NVIC_ICPR(n) = 0xFFFFFFFFu; }
    REG32(0xE000ED04) = (1u << 27) | (1u << 25);    /* ICSR: PENDSVCLR, PENDSTCLR */
    REG32(0xE000ED24) &= ~((1u << 16) | (1u << 17) | (1u << 18));   /* SHCSR: fault enables back to reset value */
    dsb(); isb();
    /* FPU on (CP10/CP11 full access): stock Keil build uses the FPU. */
    SCB_CPACR |= (0xFu << 20);
    dsb(); isb();
    uint32_t *src = &_sidata, *dst = &_sdata;
    while(dst < &_edata) { *dst++ = *src++; }
    for(dst = &_sbss; dst < &_ebss; ) { *dst++ = 0; }
    __asm volatile("cpsie i");
    main();
    for(;;) { wfi(); }
}

/* All faults (pre-flash audit F1): stacked frame of the faulting context -> hardfault_c -> hal/safety.c fault_handle
 * (TX off with register writes, record in no-init RAM, system reset; DEBUG=1 with a probe: bkpt). */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_c\n");
}

__attribute__((used, noreturn)) void hardfault_c(uint32_t *frame)
{
    fault_handle(frame);
    for(;;) { }
}

/* unused IRQ vectors, SVC, PendSV, DebugMon: unexpected here, handled as a fault */
void Default_Handler(void) __attribute__((alias("HardFault_Handler")));
