/* TX safety net + fault record (hal/safety.h). MCU-neutral: only Cortex-M system registers and the watchdog key
 * address, which is 0x40003000 on both the STM32U575 (IWDG) and the AT32F403A (WDT). */
#include "safety.h"
#include "hal.h"
#include "dbg.h"

#define SCB_CFSR_A   0xE000ED28u
#define SCB_HFSR_A   0xE000ED2Cu
#define SCB_AIRCR_A  0xE000ED0Cu
#define DHCSR_A      0xE000EDF0u
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))

volatile uint8_t g_clk_err;
volatile uint32_t g_safety_kills;
volatile fault_rec_t g_fault_rec __attribute__((section(".noinit")));

static volatile bool s_tx;
static volatile uint32_t s_tx_since, s_alive;
static volatile uint8_t s_kill;               /* forced stop pending for the main loop */

#ifdef CFW_EMU
/* emulator tests (tools/preflash_test.py): shorter cap, main-loop stall, injected fault (1 = reset, 2 = halt after
 * the TX kill so the test can look at the pins) */
volatile uint32_t g_dbg_tx_cap_ms, g_dbg_stall, g_dbg_fault;
#endif

const char *safety_tx_block(uint32_t hz)
{
#ifdef CFW_HW_NOTX
    (void)hz;
    return "TX inhibited";
#else
    if(g_clk_err) { return "CLK ERR"; }
    if(!board_apc_ok(hz)) { return "No RF cal"; }
    if(s_kill) { return "TX limit"; }
    return 0;
#endif
}

void safety_tx_on(bool on)
{
    if(on && !s_tx) { s_tx_since = hal_millis(); }
    s_tx = on;
}

bool safety_tx_active(void) { return s_tx; }

void safety_systick(void)
{
    if(!s_tx) { return; }
    uint32_t now = hal_millis(), cap = CFW_TX_CAP_MS;
#ifdef CFW_EMU
    if(g_dbg_tx_cap_ms) { cap = g_dbg_tx_cap_ms; }
#endif
    uint8_t why = now - s_tx_since >= cap ? 1 : now - s_alive >= CFW_STALL_MS ? 2 : 0;
    if(!why) { return; }
    /* main loop alive: PA off here, it does the rest within one pass. Stalled: also take the RF chip out of TX
     * (rf_write masks interrupts, so this cannot cut into a main-loop RF transfer). */
    board_tx_kill(why == 2);
    s_tx = false;
    s_kill = why;
    g_safety_kills++;
}

void safety_main_alive(void)
{
#ifdef CFW_EMU
    if(g_dbg_fault) {
        g_fault_rec.halt = g_dbg_fault == 2;
        g_dbg_fault = 0;
        __asm volatile("udf #0");             /* UsageFault -> HardFault (UsageFault not enabled) */
    }
    while(g_dbg_stall) { }
#endif
    s_alive = hal_millis();
    wdg_refresh();
}

int safety_take_kill(void)
{
    int k = s_kill;
    s_kill = 0;
    return k;
}

void safety_init(void)
{
    s_alive = hal_millis();
    if(g_fault_rec.magic != FAULT_MAGIC && g_fault_rec.magic != ~FAULT_MAGIC) {   /* power-on: RAM is random */
        g_fault_rec.count = 0;
        g_fault_rec.magic = 0;
    }
    g_fault_rec.halt = 0;
    if(g_fault_rec.magic == FAULT_MAGIC) {
        DBG("FAULT pc=%08x lr=%08x cfsr=%08x hfsr=%08x ipsr=%u (fault #%u)\n", (unsigned)g_fault_rec.pc,
            (unsigned)g_fault_rec.lr, (unsigned)g_fault_rec.cfsr, (unsigned)g_fault_rec.hfsr,
            (unsigned)g_fault_rec.ipsr, (unsigned)g_fault_rec.count);
    }
}

cal_info_t g_cal_info;

void cal_report(char copy, uint32_t base, uint32_t span, bool uhf, bool vhf)
{
    g_cal_info.copy = copy;
    g_cal_info.uhf = copy && uhf;
    g_cal_info.vhf = copy && vhf;
    g_cal_info.span = span;
    g_cal_info.crc = 0;
    if(!copy) {
        DBG("cal: no valid copy, TX off\n");
        return;
    }
    uint8_t b[64];
    uint32_t c = 0xFFFFFFFFu;
    for(uint32_t o = 0; o < span; o += sizeof b) {
        uint32_t n = span - o < sizeof b ? span - o : sizeof b;
        if(!nand_read(base + o, b, n)) { DBG("cal: copy %c read error at +0x%x\n", copy, (unsigned)o); return; }
        for(uint32_t i = 0; i < n; i++) {
            c ^= b[i];
            for(int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); }
        }
        wdg_refresh();
    }
    g_cal_info.crc = ~c;
    DBG("cal: copy %c crc32 %08x span 0x%x uhf %d vhf %d\n", copy, (unsigned)g_cal_info.crc, (unsigned)span,
        (int)g_cal_info.uhf, (int)g_cal_info.vhf);
}

static char *hex8(char *p, uint32_t v)
{
    for(int i = 28; i >= 0; i -= 4) { *p++ = "0123456789ABCDEF"[(v >> i) & 0xFu]; }
    *p = 0;
    return p;
}

bool safety_boot_fault(char *l1, char *l2)
{
    if(g_fault_rec.magic != FAULT_MAGIC) { return false; }
    g_fault_rec.magic = ~FAULT_MAGIC;         /* shown once */
    static const char a[] = "FAULT pc=", b[] = "lr=";
    char *p = l1;
    for(const char *s = a; *s; ) { *p++ = *s++; }
    hex8(p, g_fault_rec.pc);
    p = l2;
    for(const char *s = b; *s; ) { *p++ = *s++; }
    hex8(p, g_fault_rec.lr);
    return true;
}

/* Fault handlers (startup.c) land here on the handler stack (MSP). Order: PA off first, then the record, then reset. */
void fault_handle(uint32_t *frame)
{
    board_tx_kill(true);
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    uintptr_t f = (uintptr_t)frame;
    bool ok = f >= 0x20000000u && f < 0x200FFFE0u && (f & 3u) == 0;   /* a bad SP must not fault again here */
    g_fault_rec.pc = ok ? frame[6] : 0xFFFFFFFFu;
    g_fault_rec.lr = ok ? frame[5] : 0xFFFFFFFFu;
    g_fault_rec.cfsr = R32(SCB_CFSR_A);
    g_fault_rec.hfsr = R32(SCB_HFSR_A);
    g_fault_rec.ipsr = ipsr & 0x1FFu;
    g_fault_rec.count++;
    g_fault_rec.magic = FAULT_MAGIC;
    bool halt = g_fault_rec.halt != 0;
#ifdef CFW_DEBUG
    if(R32(DHCSR_A) & 1u) {                   /* C_DEBUGEN: a probe is attached -> stop here for it */
        __asm volatile("bkpt #0");
        halt = true;
    }
#endif
    if(halt) { for(;;) { } }
    __asm volatile("dsb" ::: "memory");
    R32(SCB_AIRCR_A) = (0x5FAu << 16) | (1u << 2);   /* SYSRESETREQ */
    for(;;) { }
}
