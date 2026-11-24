/* Debug log over the SWD probe: SEGGER RTT compatible control block in RAM (no UART, no pins).
 *
 * Built only with `make DEBUG=1` (defines CFW_RTT; DEBUG=1 also implies KEEP_SWD=1). In every other build the calls
 * below compile to nothing, so release / HW images are unchanged.
 *
 * Read it with:  tools/hw/hw.py --radio d890|d168 log      (J-Link RTT, OpenOCD `rtt`, or a plain memory poll)
 * or any RTT viewer (J-Link RTT Viewer / JLinkRTTLogger, OpenOCD `rtt server`). The control block is the symbol
 * `_SEGGER_RTT`; one up channel ("Terminal", 2 KB), no down channel. Mode = skip on overflow: the radio never
 * blocks on a missing or slow probe. Shared by the D890UV and D168UV cfw (MCU-neutral, plain RAM). */
#ifndef CFW_DBG_H
#define CFW_DBG_H
#include <stdint.h>

#ifdef CFW_RTT
void dbg_init(void);
void dbg_write(const char *s, unsigned n);
void dbg_puts(const char *s);
/* tiny printf: %d %i %u %x %X %p %s %c %%, width / zero pad (e.g. %08x), 'l' ignored. Max 128 chars per call. */
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define DBG(...) dbg_printf(__VA_ARGS__)
#else
static inline void dbg_init(void) {}
static inline void dbg_write(const char *s, unsigned n) { (void)s; (void)n; }
static inline void dbg_puts(const char *s) { (void)s; }
#define dbg_printf(...) ((void)0)
#define DBG(...) ((void)0)
#endif

#endif
