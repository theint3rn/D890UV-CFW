/* CPS programming link (codeplug only) on USART1, the programming-cable UART.
 *
 * Framing = the stock D890UV PC link (CLAUDE.md "PC link"), with a cfw address space:
 *   "PROGRAM"                        -> "QX" 06                      enter program mode (UI shows PROGRAM MODE)
 *   02                               -> 'I' "D890CFW" 00 fwMaj fwMin cpVer caps maxKB(BE16) 06   (16 bytes)
 *   'R' a3 a2 a1 a0 len              -> 'W' a3 a2 a1 a0 len data cs 06    cs = sum(addr,len,data) & 0xFF
 *   'W' a3 a2 a1 a0 len data cs 06   -> 06 ok / 15 refused (checksum, range, alignment, write-protected, verify)
 *   "END"                            -> 06 (codeplug valid or nothing written) / 15 (written but invalid)
 * Addresses (big endian):
 *   0x00000000-0x0005FFFF  the codeplug region (internal flash 0x08180000, app/cfwcp.c). Writes: 16-byte aligned,
 *                          len a multiple of 16; a page is erased on its first write in a session. The CPS writes
 *                          the header block (offset 0) last, so an aborted write leaves an invalid codeplug and the
 *                          radio falls back to the NAND (stock CPS) channels.
 *   0x00F00000 (64 bytes)  info block, read only (layout in cfw_cps/docs/CODEPLUG.md "Link").
 * Program mode ends with END or after 5 s without a byte. Firmware flashing is NOT part of this protocol.
 * USART1 115200 8N1: emulator / non-HW builds PB6 TX / PB7 RX AF7 (on the D890UV these pins are the I2C bus, so not
 * for a real radio). HW=1 builds: link off. HW=1 HW_SAVE=1 (CFW_PROG_HW + CFW_PROG_PA9, pre-flash audit F10): link on
 * the stock programming-cable pins PA9 TX / PA10 RX AF7 (FUN_08063358) with codeplug writes (range-checked); PA10 is
 * also the PTT input, so that build reads no PTT and has no TX. Only after the owner OKs flash writes. */
#include <string.h>
#include "hal.h"
#ifndef CFW_PROG_UART_EXTERNAL
#include "stm32u5.h"
#endif
#include "gfx.h"
#include "cfwcp.h"
#include "prog.h"
#include "radio.h"
#include "rset.h"
#include "channels.h"

#ifndef CFW_BUILD
#define CFW_BUILD "?"
#endif
#define FW_MAJOR 1
#define FW_MINOR 0
#define LINK_VERSION 1

#ifndef CFW_PROG_MODEL
#define CFW_PROG_MODEL 'D', '8', '9', '0'          /* ident "D890CFW"; another board sets its own (board_cfg.h) */
#endif

#ifndef CFW_PROG_UART_EXTERNAL
#define USART1_BASE  0x40013800u
#define U_CR1  REG32(USART1_BASE + 0x00)
#define U_CR2  REG32(USART1_BASE + 0x04)
#define U_CR3  REG32(USART1_BASE + 0x08)
#define U_BRR  REG32(USART1_BASE + 0x0C)
#define U_ISR  REG32(USART1_BASE + 0x1C)
#define U_ICR  REG32(USART1_BASE + 0x20)
#define U_RDR  REG32(USART1_BASE + 0x24)
#define U_TDR  REG32(USART1_BASE + 0x28)
#define USART1_IRQ 61
#define PCLK2_HZ 120000000u
#define BAUD 115200u
#endif

#define INFO_ADDR 0x00F00000u
#define ACK 0x06
#define NAK 0x15

static volatile uint8_t s_rx[1024];
static volatile uint8_t s_tx[1024];                        /* replies go out from the IRQ: prog_poll never waits */
static volatile uint16_t s_tx_r;
static uint16_t s_tx_w;
static volatile uint16_t s_rx_w;
static uint16_t s_rx_r;
static bool s_up, s_active, s_wrote, s_last_ok;
static uint32_t s_last_ms;
static uint8_t s_cmd[6 + 255 + 2];
static uint16_t s_n;
static uint32_t s_bytes_rx, s_writes, s_reads, s_naks;

#ifdef CFW_PROG_UART_EXTERNAL
/* board UART driver (board_cfg.h): prog_uart_hw_init() / prog_uart_hw_txkick(); its IRQ calls these two */
void prog_uart_rx_byte(uint8_t b)
{
    uint16_t w = s_rx_w, nw = (uint16_t)((w + 1u) % sizeof s_rx);
    if(nw != s_rx_r) { s_rx[w] = b; s_rx_w = nw; }
}
int prog_uart_tx_next(void)
{
    if(s_tx_r == s_tx_w) { return -1; }
    uint8_t b = s_tx[s_tx_r];
    s_tx_r = (uint16_t)((s_tx_r + 1u) % sizeof s_tx);
    return b;
}
#else
void USART1_IRQHandler(void)
{
    uint32_t isr = U_ISR;
    if(isr & (1u << 3)) { U_ICR = 1u << 3; }               /* overrun: clear, the CPS retries */
    while(U_ISR & (1u << 5)) {                             /* RXNE */
        uint8_t b = (uint8_t)U_RDR;
        uint16_t w = s_rx_w, nw = (uint16_t)((w + 1u) % sizeof s_rx);
        if(nw != s_rx_r) { s_rx[w] = b; s_rx_w = nw; }
    }
    while((U_CR1 & (1u << 7)) && (U_ISR & (1u << 7))) {    /* TXEIE + TXE */
        if(s_tx_r == s_tx_w) { U_CR1 &= ~(1u << 7); break; }
        U_TDR = s_tx[s_tx_r];
        s_tx_r = (uint16_t)((s_tx_r + 1u) % sizeof s_tx);
    }
}
#endif

void prog_init(void)
{
#if defined(CFW_HW_BUILD) && !defined(CFW_PROG_HW)
    return;                                                /* real radio: pins not confirmed yet */
#endif
#ifdef CFW_PROG_UART_EXTERNAL
    prog_uart_hw_init();
#else
    RCC_APB2ENR |= 1u << 14;                               /* USART1EN */
#ifdef CFW_PROG_PA9
    RCC_AHB2ENR1 |= 1u << 0;                               /* GPIOA */
    (void)RCC_APB2ENR;
    gpio_af(GPIOA, 9, 7);
    gpio_af(GPIOA, 10, 7);
    gpio_mode(GPIOA, 9, GPIO_AF, PULL_NONE);
    gpio_mode(GPIOA, 10, GPIO_AF, PULL_UP);
#else
    RCC_AHB2ENR1 |= 1u << 1;                               /* GPIOB */
    (void)RCC_APB2ENR;
    gpio_af(GPIOB, 6, 7);
    gpio_af(GPIOB, 7, 7);
    gpio_mode(GPIOB, 6, GPIO_AF, PULL_NONE);
    gpio_mode(GPIOB, 7, GPIO_AF, PULL_UP);
#endif
    U_CR1 = 0;
    U_BRR = (PCLK2_HZ + BAUD / 2u) / BAUD;
    U_CR2 = 0;
    U_CR3 = 0;
    U_CR1 = (1u << 5) | (1u << 3) | (1u << 2) | 1u;        /* RXNEIE, TE, RE, UE */
    nvic_enable(USART1_IRQ);
#endif
    s_up = true;
}

bool prog_active(void) { return s_active; }

static void tx(const uint8_t *b, uint32_t n)              /* queue; the IRQ sends (TXEIE) */
{
    while(n--) {
        uint16_t nw = (uint16_t)((s_tx_w + 1u) % sizeof s_tx);
        if(nw == s_tx_r) { break; }                         /* full: cannot happen (one reply in flight) */
        s_tx[s_tx_w] = *b++;
        s_tx_w = nw;
    }
#ifdef CFW_PROG_UART_EXTERNAL
    prog_uart_hw_txkick();
#else
    uint32_t p = irq_save();
    U_CR1 |= 1u << 7;                                       /* TXEIE */
    irq_restore(p);
#endif
}
static void tx1(uint8_t b) { tx(&b, 1); }

static void info_block(uint8_t *o)                         /* 64 bytes, little endian */
{
    memset(o, 0, 64);
    memcpy(o, "CFWI", 4);
    uint32_t v[] = { LINK_VERSION | (CFWCP_VERSION << 16), CFWCP_BASE, CFWCP_SIZE, cfwcp_len(), cfwcp_crc(),
                     cfwcp_generation(), (uint32_t)cfwcp_channel_count(), (uint32_t)channels_count() };
    memcpy(o + 4, v, sizeof v);
    o[36] = cfwcp_valid();
    o[37] = cfwcp_write_allowed() ? 0 : 1;                 /* 1 = write-protected build (HW=1) */
    o[38] = FW_MAJOR;
    o[39] = FW_MINOR;
    strncpy((char *)o + 40, CFW_BUILD, 24);                /* F8: tree, build kind, date (Makefile) */
}

static void do_read(uint32_t a, uint8_t len)
{
    uint8_t r[6 + 255 + 2], info[64];
    r[0] = 'W';
    r[1] = (uint8_t)(a >> 24); r[2] = (uint8_t)(a >> 16); r[3] = (uint8_t)(a >> 8); r[4] = (uint8_t)a;
    r[5] = len;
    if(a >= INFO_ADDR && a + len <= INFO_ADDR + 64u) {
        info_block(info);
        memcpy(r + 6, info + (a - INFO_ADDR), len);
    } else if(a < CFWCP_SIZE && len <= CFWCP_SIZE - a) {
        memcpy(r + 6, (const void *)(uintptr_t)(CFWCP_BASE + a), len);
    } else {
        memset(r + 6, 0xFF, len);
    }
    uint8_t cs = 0;
    for(int i = 1; i < 6 + len; i++) { cs = (uint8_t)(cs + r[i]); }
    r[6 + len] = cs;
    r[7 + len] = ACK;
    tx(r, 8u + len);
    s_reads++;
}

static void do_write(void)
{
    uint32_t a = (uint32_t)s_cmd[1] << 24 | (uint32_t)s_cmd[2] << 16 | (uint32_t)s_cmd[3] << 8 | s_cmd[4];
    uint8_t len = s_cmd[5], cs = 0;
    for(int i = 1; i < 6 + len; i++) { cs = (uint8_t)(cs + s_cmd[i]); }
    bool ok = cs == s_cmd[6 + len] && s_cmd[7 + len] == ACK && cfwcp_write_allowed();
    if(ok) {
        if(!s_wrote) { cfwcp_write_begin(); s_wrote = true; }
        ok = cfwcp_write(a, s_cmd + 6, len);
    }
    if(ok) { s_writes++; } else { s_naks++; }
    tx1(ok ? ACK : NAK);
}

static void leave(bool reply)
{
    bool ok = true;
    if(s_wrote) {
        ok = cfwcp_check();
        channels_reload();                                  /* cfw codeplug if valid, else NAND channels */
        cfwcp_apply_radio();
        rset_init();                                        /* menu settings: the new codeplug (its journal) */
        radio_update_alias();
        if(g_radio.zone >= zone_count()) { g_radio.zone = -1; }
        radio_goto(g_radio.channel < channels_count() ? g_radio.channel : 0);
        radio_save();                                       /* forget old per-channel overrides (no-op on HW=1) */
    }
    s_last_ok = ok;
    if(reply) { tx1(ok ? ACK : NAK); }
    s_active = false;
    s_wrote = false;
    gfx_invalidate();
}

static int need(void)                                      /* bytes needed for the command in s_cmd, 0 = drop */
{
    switch(s_cmd[0]) {
    case 'P': return 7;
    case 0x02: return s_active ? 1 : 0;
    case 'R': return s_active ? 6 : 0;
    case 'W': return !s_active ? 0 : s_n < 6 ? 6 : 8 + s_cmd[5];
    case 'E': return s_active ? 3 : 0;
    default:  return 0;
    }
}

static void exec(void)
{
    switch(s_cmd[0]) {
    case 'P':
        if(memcmp(s_cmd, "PROGRAM", 7) != 0) { return; }
        if(g_radio.tx) { radio_ptt(false); }               /* no TX while programming (ui_key ignores PTT too) */
        s_active = true;
        s_wrote = false;
        s_writes = s_reads = s_naks = 0;
        tx((const uint8_t *)"QX\x06", 3);
        gfx_invalidate();
        break;
    case 0x02: {
        uint8_t id[16] = { 'I', CFW_PROG_MODEL, 'C', 'F', 'W', 0, FW_MAJOR, FW_MINOR, CFWCP_VERSION,
                           (uint8_t)(1u | (cfwcp_write_allowed() ? 2u : 4u)), (uint8_t)((CFWCP_SIZE / 1024u) >> 8),
                           (uint8_t)(CFWCP_SIZE / 1024u), ACK };
        tx(id, sizeof id);
        break;
    }
    case 'R':
        do_read((uint32_t)s_cmd[1] << 24 | (uint32_t)s_cmd[2] << 16 | (uint32_t)s_cmd[3] << 8 | s_cmd[4], s_cmd[5]);
        break;
    case 'W':
        do_write();
        break;
    case 'E':
        if(memcmp(s_cmd, "END", 3) == 0) { leave(true); }
        break;
    }
}

void prog_poll(void)
{
    if(!s_up) { return; }
    uint32_t now = hal_millis();
    if(s_n && now - s_last_ms > 500u) { s_n = 0; }         /* half a command, then silence: resync */
    while(s_rx_r != s_rx_w) {
        uint8_t b = s_rx[s_rx_r];
        s_rx_r = (uint16_t)((s_rx_r + 1u) % sizeof s_rx);
        s_bytes_rx++;
        s_last_ms = now;
        s_cmd[s_n++] = b;
        int n = need();
        if(n == 0) { s_n = 0; continue; }
        if(s_cmd[0] == 'P' && b != (uint8_t)"PROGRAM"[s_n - 1]) { s_n = 0; continue; }
        if(s_n >= n) { exec(); s_n = 0; }
    }
    if(s_active && now - s_last_ms > 5000u) { leave(false); }
}

void prog_draw(void)
{
    char t[32], *p;
    gfx_clear(C_BLACK);
    gfx_rect(0, 0, LCD_W, 12, C_BLUE);
    gfx_text(4, 2, "CPS LINK", C_WHITE, C_BLUE, 1);
    gfx_text_center(24, "PROGRAM MODE", C_YELLOW, C_BLACK, 2);
    int y = 50;
    p = fmt_s(t, "rx "); p = fmt_u(p, s_bytes_rx, 1); p = fmt_s(p, " B"); (void)p;
    gfx_text(8, y, t, C_WHITE, C_BLACK, 1); y += 10;
    p = fmt_s(t, "reads "); p = fmt_u(p, s_reads, 1); p = fmt_s(p, "  writes "); fmt_u(p, s_writes, 1);
    gfx_text(8, y, t, C_WHITE, C_BLACK, 1); y += 10;
    if(s_naks) { p = fmt_s(t, "refused "); fmt_u(p, s_naks, 1); gfx_text(8, y, t, C_RED, C_BLACK, 1); }
    y += 10;
    gfx_text(8, y, cfwcp_write_allowed() ? "codeplug: internal flash" : "write-protected (HW=1)", C_GREY, C_BLACK, 1);
    y += 10;
    gfx_text(8, y, "no firmware flashing", C_GREY, C_BLACK, 1);
    gfx_flush();
}
