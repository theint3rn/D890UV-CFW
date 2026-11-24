/* Kioxia SPI-NAND 4 Gbit (ID 98 ED / 98 CD), bit-banged, READ ONLY.
 * Pins/commands as stock FUN_08037db8 / FUN_080377a4 / FUN_080379b4: CS# PC5, SCK PE15, IO0 PB10 (MOSI),
 * IO1 PB8 (MISO). SPI mode 0: the chip samples on SCK rising, drives on SCK falling.
 * Read = 0x13 page-read-to-cache (24-bit row), poll 0x0F C0 until OIP clear, 0x03 read-from-cache
 * (16-bit column + dummy). 4 KiB pages; CPS address == NAND byte address. Stock uses the x2 read 0x3B;
 * x1 is simpler and fine for now.
 * There is deliberately no program/erase code here (see CLAUDE.md: NAND writes only via tools/hw). */
#include "stm32u5.h"
#include "board.h"
#include "hal.h"

#define PAGE 4096u
static uint16_t s_id;

#define CS_LO()  gpio_clr(NAND_CS_PORT, NAND_CS)
#define CS_HI()  gpio_set(NAND_CS_PORT, NAND_CS)
#define SCK_LO() gpio_clr(NAND_SCK_PORT, NAND_SCK)
#define SCK_HI() gpio_set(NAND_SCK_PORT, NAND_SCK)

static void tx(uint8_t b)
{
    for(int i = 7; i >= 0; i--) {
        gpio_write(NAND_IO0_PORT, NAND_IO0, (b >> i) & 1);
        SCK_HI();
        SCK_LO();
    }
}

/* one byte out of the chip. The chip drives each output bit on a falling edge, the first one on the
 * falling edge that ends the last command/dummy bit, so: sample IO1, then clock to the next bit. */
static uint8_t rx(void)
{
    uint8_t b = 0;
    for(int i = 0; i < 8; i++) {
        b = (uint8_t)((b << 1) | gpio_read(NAND_IO1_PORT, NAND_IO1));
        SCK_HI();
        SCK_LO();
    }
    return b;
}

static uint8_t get_feature(uint8_t reg)
{
    CS_LO(); tx(0x0F); tx(reg);
    uint8_t v = rx();
    CS_HI();
    return v;
}

bool nand_init(void)
{
    SCK_LO(); CS_HI();
    delay_us(10);
    CS_LO(); tx(0xFF); CS_HI();                    /* reset */
    delay_ms(2);
    CS_LO(); tx(0x9F); tx(0x00);
    uint8_t mid = rx(), did = rx();
    CS_HI();
    s_id = (uint16_t)((mid << 8) | did);
    return mid == 0x98 && (did == 0xED || did == 0xCD);
}

uint16_t nand_id(void) { return s_id; }

static bool wait_ready(void)
{
    for(int n = 0; n < 2000; n++) {
        if(!(get_feature(0xC0) & 1)) { return true; }   /* OIP */
        delay_us(10);
    }
    return false;
}

bool nand_page_load(uint32_t addr)
{
    uint32_t row = addr / PAGE;
    wdg_refresh();                                     /* F3: boot scans read many pages */
    CS_LO(); tx(0x13); tx((uint8_t)(row >> 16)); tx((uint8_t)(row >> 8)); tx((uint8_t)row); CS_HI();
    return wait_ready();
}

void nand_cache_read(uint32_t col, void *buf, uint32_t n)
{
    uint8_t *out = buf;
    CS_LO(); tx(0x03); tx((uint8_t)(col >> 8)); tx((uint8_t)col); tx(0x00);
    for(uint32_t i = 0; i < n; i++) { out[i] = rx(); }
    CS_HI();
}

bool nand_read(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *out = buf;
    while(len) {
        uint32_t col = addr % PAGE;
        uint32_t n = PAGE - col;
        if(n > len) { n = len; }
        if(!nand_page_load(addr)) { return false; }
        nand_cache_read(col, out, n);
        out += n; addr += n; len -= n;
    }
    return true;
}
