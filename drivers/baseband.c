/* SCT3288 baseband link on SPI2 (CS# PB12, byte-ready PD11, frame-pending PD7, reset PD9).
 *
 * Frames both ways: 84 A9 61 LenH LenL B0 ID data... [00 pad if Len odd], Len = 1 + data length
 * (stock parser FUN_0813af78, message entry = frame + 5; MCU writer FUN_0810a674, reader FUN_0810a6dc).
 * Reading = clocking a dummy byte equal to the last byte sent (stock FUN_08103838 resends DAT_2001deee).
 *
 * Init (stock FUN_080fa16c -> FUN_080e70ec -> FUN_080e6c64, see CLAUDE.md "Baseband"):
 *   1. wait ID 0x17 status 0x0E (FUN_0810061c)
 *   2. send the raw 6-byte config 74 A9 01 00 01 00 (FUN_08100664, not a framed message)
 *   3. wait ID 0x17 status 00/01 (FUN_081005cc)
 *   4. cmd 0x2D 04 (CODEC_SELECT) -> reply 00, then codec register 0x0B = 02, 82 (stock FUN_080f2540)
 *   5. cmd 0x33 with B0 0x23 -> 9-byte version (BCD date at +3..7)
 *   6. cmd 0x10 0C (VOCODER_SEL, the last value the stock boot sends) -> ID 0x10 status 00 (stock waits long for it)
 * Runs as a non-blocking state machine from bb_poll(). bb_send() refuses everything that hal/bb_allow.h does not list. */
#include <string.h>
#include "stm32u5.h"
#include "board.h"
#include "hal.h"
#include "bb_allow.h"
#include "voc_sct.h"

enum { ST_OFF, ST_WAIT_BOOT, ST_WAIT_READY, ST_WAIT_2D, ST_WAIT_33, ST_WAIT_10, ST_READY, ST_FAIL };
static const char *const k_state_txt[] = {
    "off", "boot?", "cfg", "2D?", "ver?", "10?", "ready", "no reply",
};

static uint8_t s_state;
static uint32_t s_t0;
static uint8_t s_last_tx;
static uint8_t s_ver[9];
static bb_msg_cb s_listener;
static const uint8_t k_codec[1] = { 0x04 };
static uint16_t s_refused;                          /* commands stopped by the allow-list */
static uint8_t s_refused_b0, s_refused_id;

/* receive assembly */
static uint8_t s_rx[6 + 1 + BB_MAX_DATA + 2];
static uint16_t s_rx_n;

static uint8_t xfer(uint8_t b)
{
    for(uint32_t n = 0; n < 100000u && !(SPI2_SR & (1u << 1)); n++) { }   /* TXP */
    SPI2_TXDR8 = b;
    for(uint32_t n = 0; n < 100000u && !(SPI2_SR & (1u << 0)); n++) { }   /* RXP */
    s_last_tx = b;
    return SPI2_RXDR8;
}

static void write_bytes(const uint8_t *p, uint16_t n)
{
    gpio_clr(BB_CS_PORT, BB_CS);
    for(uint16_t i = 0; i < n; i++) { (void)xfer(p[i]); }
    gpio_set(BB_CS_PORT, BB_CS);
}

bool bb_send(uint8_t b0, uint8_t id, const uint8_t *data, uint16_t len)
{
    static uint8_t f[6 + 1 + BB_MAX_DATA + 1];     /* vocoder PCM packets carry 321 data bytes */
    if(len > BB_MAX_DATA) { return false; }
    if(!bb_allowed(b0, id, data, len)) {
        s_refused++; s_refused_b0 = b0; s_refused_id = id;
        return false;
    }
    uint16_t n = (uint16_t)(1 + len);
    f[0] = 0x84; f[1] = 0xA9; f[2] = 0x61; f[3] = (uint8_t)(n >> 8); f[4] = (uint8_t)n; f[5] = b0; f[6] = id;
    if(len) { memcpy(&f[7], data, len); }
    uint16_t total = (uint16_t)(6 + n);
    if(n & 1) { f[total++] = 0; }
    write_bytes(f, total);
    return true;
}

static void handle(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t st = len ? d[0] : 0xFF;
    switch(s_state) {
    case ST_WAIT_BOOT:
        if(id == 0x17 && st == 0x0E) {
            static const uint8_t cfg[6] = { 0x74, 0xA9, 0x01, 0x00, 0x01, 0x00 };
            write_bytes(cfg, sizeof cfg);
            s_state = ST_WAIT_READY; s_t0 = hal_millis();
        } else if(id == 0x17 && st <= 1) {            /* already configured (warm restart) */
            bb_send(0x00, 0x2D, k_codec, 1);
            s_state = ST_WAIT_2D; s_t0 = hal_millis();
        }
        break;
    case ST_WAIT_READY:
        if(id == 0x17 && st <= 1) {
            bb_send(0x00, 0x2D, k_codec, 1);
            s_state = ST_WAIT_2D; s_t0 = hal_millis();
        }
        break;
    case ST_WAIT_2D:
        if(id == 0x2D) {
            static const uint8_t r0b[2] = { 0x02, 0x82 };
            bb_send(0x40, 0x0B, &r0b[0], 1);
            bb_send(0x40, 0x0B, &r0b[1], 1);
            bb_send(0x23, 0x33, NULL, 0);
            s_state = ST_WAIT_33; s_t0 = hal_millis();
        }
        break;
    case ST_WAIT_33:
        if(id == 0x33) {
            memset(s_ver, 0, sizeof s_ver);
            memcpy(s_ver, d, len < sizeof s_ver ? len : sizeof s_ver);
            static const uint8_t sel[1] = { 0x0C };
            bb_send(0x00, 0x10, sel, 1);
            s_state = ST_WAIT_10; s_t0 = hal_millis();
        }
        break;
    case ST_WAIT_10:
        if(id == 0x10 && st == 0) { s_state = ST_READY; }
        break;
    case ST_READY:
        if(s_listener) { s_listener(b0, id, d, len); }
        break;
    default:
        break;
    }
}

/* byte parser, same framing rules as stock FUN_0813af78 */
static void feed(uint8_t b)
{
    static const uint8_t hdr[3] = { 0x84, 0xA9, 0x61 };
    if(s_rx_n < 3) {
        if(b == hdr[s_rx_n]) { s_rx[s_rx_n++] = b; }
        else { s_rx_n = (b == 0x84) ? 1 : 0; if(s_rx_n) { s_rx[0] = b; } }
        return;
    }
    if(s_rx_n >= sizeof s_rx) { s_rx_n = 0; return; }
    s_rx[s_rx_n++] = b;
    if(s_rx_n < 6) { return; }
    uint16_t n = (uint16_t)((s_rx[3] << 8) | s_rx[4]);
    if(n == 0 || 6u + n + (n & 1u) > sizeof s_rx) { s_rx_n = 0; return; }
    if(s_rx_n >= 6u + n + (n & 1u)) {
        handle(s_rx[5], s_rx[6], &s_rx[7], (uint16_t)(n - 1));
        s_rx_n = 0;
    }
}

void bb_init(void)
{
    RCC_APB1ENR1 |= 1u << 14;                          /* SPI2EN */
    (void)RCC_APB1ENR1;
    /* SPI2 pins: PB13 SCK, PB14 MISO, PB15 MOSI, AF5 (CS PB12 is a GPIO) */
    gpio_af(GPIOB, 13, 5);
    gpio_af(GPIOB, 14, 5);
    gpio_af(GPIOB, 15, 5);

    SPI2_CR1 = 0;
    SPI2_CFG1 = (7u << 0) | (4u << 28);                /* DSIZE 8 bit, MBR /32 (120 MHz -> 3.75 MHz) */
    SPI2_CFG2 = (1u << 22) | (1u << 26);               /* MASTER, SSM (software NSS) */
    SPI2_CR1 = (1u << 12);                             /* SSI */
    SPI2_CR2 = 0;                                      /* TSIZE 0 = endless */
    SPI2_CR1 |= 1u;                                    /* SPE */
    SPI2_CR1 |= 1u << 9;                               /* CSTART */

    s_rx_n = 0;
    s_state = ST_WAIT_BOOT;
    s_t0 = hal_millis();
}

static bool data_ready(void)
{
    return gpio_read(BB_RDY_PORT, BB_RDY) || gpio_read(BB_RDY_PORT, BB_PEND);
}

void bb_poll(void)
{
    if(s_state == ST_OFF) { return; }
    for(int budget = 0; budget < 512 && data_ready(); budget++) {
        gpio_clr(BB_CS_PORT, BB_CS);
        uint8_t b = xfer(s_last_tx);                   /* dummy = last byte sent */
        gpio_set(BB_CS_PORT, BB_CS);
        feed(b);
    }
    if(s_state != ST_READY && s_state != ST_FAIL && hal_millis() - s_t0 > (s_state == ST_WAIT_10 ? 20000u : 5000u)) {
        s_state = ST_FAIL;                             /* UI shows it; the radio still works in FM */
    }
}

bool bb_ready(void) { return s_state == ST_READY; }
const char *bb_status(void) { return s_state == ST_READY ? sct_status_text() : k_state_txt[s_state]; }
uint16_t bb_refused(uint8_t *b0, uint8_t *id)
{
    if(b0) { *b0 = s_refused_b0; }
    if(id) { *id = s_refused_id; }
    return s_refused;
}
void bb_set_listener(bb_msg_cb cb) { s_listener = cb; }
const uint8_t *bb_version(void) { return s_ver; }

