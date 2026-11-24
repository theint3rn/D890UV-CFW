/* TX safety net + fault record, shared by the D890UV and D168UV cfw (hal/safety.c). Pre-flash audit
 * (docs/PREFLASH_AUDIT.md) items F1 (fault handlers), F2 (TX hard cap), F3 (watchdog refresh), F5/F7/F9 (TX gate).
 *
 *   - Every PA key-up / release goes through the board's dac_apc(), which reports it here (safety_tx_on) and asks
 *     safety_tx_block() first: no TX in a HW_NOTX build, after a clock failure, without a valid RF calibration for the
 *     frequency, or while a forced stop is pending.
 *   - SysTick (safety_systick) forces the PA off when one transmission lasts longer than CFW_TX_CAP_MS (180 s, every
 *     build, TOT setting or not) or when the main loop has not run for 2 s while transmitting. The main loop then
 *     cleans up (safety_take_kill -> radio_ptt(false), PTT must be released before the next TX).
 *   - Fault handlers (startup.c) call fault_handle(): PA off with register writes only (board_tx_kill), PC/LR/CFSR/HFSR
 *     to a no-init RAM record, then a system reset (DEBUG=1 with a probe attached: bkpt). The next boot shows
 *     "FAULT pc=..." once and logs it on RTT.
 *   - wdg_refresh(): IWDG (D890UV) / WDT (D168UV) key 0xAAAA at 0x40003000 on both MCUs. The cfw never starts the
 *     watchdog; a refresh is harmless while it is stopped. Called every main-loop pass and inside long waits. */
#ifndef CFW_SAFETY_H
#define CFW_SAFETY_H
#include <stdint.h>
#include <stdbool.h>

#ifndef CFW_TX_CAP_MS
#define CFW_TX_CAP_MS 180000u            /* hard limit of one transmission */
#endif
#define CFW_STALL_MS  2000u              /* main loop silent this long while transmitting -> PA off */

static inline void wdg_refresh(void) { *(volatile uint32_t *)0x40003000u = 0xAAAAu; }

/* ---- board side (drivers/) ---- */
void board_tx_kill(bool rf);             /* register writes only: APC DAC 0, TX enables low, TX PWM stopped (+ RF chip
                                            back to receive when rf); safe in a fault handler */
bool board_apc_ok(uint32_t hz);          /* RF calibration valid for this TX frequency */

/* ---- TX gate / cap ---- */
const char *safety_tx_block(uint32_t hz);   /* NULL = TX allowed, else the reason (popup text) */
void safety_tx_on(bool on);              /* dac_apc: PA keyed / released */
bool safety_tx_active(void);
void safety_systick(void);               /* SysTick ISR */
void safety_main_alive(void);            /* every main-loop pass (also refreshes the watchdog) */
int  safety_take_kill(void);             /* main loop: 0 none, 1 TX cap, 2 main loop stall (returns each stop once) */
extern volatile uint8_t g_clk_err;       /* clock_init: HSE / PLL did not start, running on the internal oscillator */
extern volatile uint32_t g_safety_kills; /* forced stops since boot */

/* ---- RF calibration in use (docs/CALIBRATION.md section 3) ----
 * dac.c calls cal_report() right after choosing the stock calibration copy. It hashes the first `span` bytes of
 * that copy (zlib CRC-32, the value tools/hw/calmap.py crc() gives for a dump: span 0x2000 D890UV, 0x800 D168UV) and
 * logs on RTT (DEBUG=1):  "cal: copy A crc32 1a2b3c4d span 0x2000 uhf 1 vhf 1"  or  "cal: no valid copy, TX off".
 * The Info screen shows the same. */
typedef struct {
    char copy;                           /* 'A', 'B', 0 = no valid copy */
    bool uhf, vhf;                       /* APC usable per band */
    uint32_t crc, span;
} cal_info_t;
extern cal_info_t g_cal_info;
void cal_report(char copy, uint32_t base, uint32_t span, bool uhf, bool vhf);

/* ---- fault record (no-init RAM, survives the reset) ---- */
typedef struct {
    uint32_t magic, pc, lr, cfsr, hfsr, ipsr, count, halt;
} fault_rec_t;
#define FAULT_MAGIC 0xFA17C0DEu
extern volatile fault_rec_t g_fault_rec;
void fault_handle(uint32_t *frame);      /* from the startup fault handlers (frame = stacked r0..xpsr) */
void safety_init(void);                  /* first thing in main(): checks the record left in no-init RAM */
bool safety_boot_fault(char *l1, char *l2);  /* after a fault reset (once): "FAULT pc=xxxxxxxx" / "lr=xxxxxxxx" */

#endif
