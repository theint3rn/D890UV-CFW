/* "Dirty" bootloader exit for the cfw pre-flash test F4 (tools/preflash_test.py): what a careless bootloader could
 * leave behind when it jumps to the app. Built per radio with -DD890 / -DD168 and loaded as the boot stub (D890UV
 * emu/run.py --bootstub, at 0x08000000) or as a vector table in RAM (D168UV, cpu VectorTableOffset).
 *   - PLL running and selected as the system clock (D890 RCC_CFGR1.SW = PLL1, D168 CRM_CFG.SCLKSEL = PLL)
 *   - SysTick running with its interrupt enabled, SysTick + PendSV pending
 *   - two NVIC interrupts enabled and pending (encoder EXTI, USART1)
 *   - MSP left low in RAM, interrupts masked (PRIMASK), then a jump to the app's reset vector.
 * Without the F4 clean-up the first `cpsie i` in the cfw takes the stale interrupts. */
    .syntax unified
    .thumb
#ifdef D890
    .equ APP,     0x0800C000
    .equ STUB_SP, 0x20025FA0
    .equ CLK_CR,  0x46020C00          /* RCC_CR */
    .equ CLK_ON,  (1 << 24) | (1 << 16)  /* PLL1ON, HSEON */
    .equ CLK_SW,  0x46020C1C          /* RCC_CFGR1 */
    .equ CLK_SWV, 3
    .equ IRQ_W0,  (1 << 19)           /* EXTI8 (IRQ 19) */
    .equ IRQ_W1,  (1 << 29)           /* USART1 (IRQ 61) */
#else
    .equ APP,     0x08004000
    .equ STUB_SP, 0x20030000
    .equ CLK_CR,  0x40021000          /* CRM_CTRL */
    .equ CLK_ON,  (1 << 24) | (1 << 16)  /* PLLEN, HEXTEN */
    .equ CLK_SW,  0x40021004          /* CRM_CFG */
    .equ CLK_SWV, 2
    .equ IRQ_W0,  (1 << 23)           /* EXTI9_5 (IRQ 23) */
    .equ IRQ_W1,  (1 << 5)            /* USART1 (IRQ 37) */
#endif
    .section .vectors, "a"
    .word STUB_SP
    .word stub_reset + 1
    .space 0x100 - 8
    .text
    .thumb_func
    .global stub_reset
stub_reset:
    cpsid i
    ldr   r0, =CLK_CR
    ldr   r1, [r0]
    ldr   r2, =CLK_ON
    orrs  r1, r2
    str   r1, [r0]
    ldr   r0, =CLK_SW
    ldr   r1, [r0]
    bic   r1, r1, #3
    orr   r1, r1, #CLK_SWV
    str   r1, [r0]
    ldr   r0, =0xE000E010             /* SysTick: reload, enable + TICKINT */
    ldr   r1, =12000
    str   r1, [r0, #4]
    movs  r1, #3
    str   r1, [r0]
    ldr   r0, =0xE000E100             /* NVIC ISER0/1 */
    ldr   r1, =IRQ_W0
    str   r1, [r0]
    ldr   r1, =IRQ_W1
    str   r1, [r0, #4]
    ldr   r0, =0xE000E200             /* NVIC ISPR0/1 */
    ldr   r1, =IRQ_W0
    str   r1, [r0]
    ldr   r1, =IRQ_W1
    str   r1, [r0, #4]
    ldr   r0, =0xE000ED04             /* ICSR: PENDSVSET, PENDSTSET */
    ldr   r1, =(1 << 28) | (1 << 26)
    str   r1, [r0]
    ldr   r0, =0x20001000             /* MSP left low */
    msr   msp, r0
    ldr   r0, =APP
    ldr   r1, =0xE000ED08             /* VTOR = app (as the stock bootloader) */
    str   r0, [r1]
    ldr   r1, [r0, #4]
    bx    r1
    .ltorg
