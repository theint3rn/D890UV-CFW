/* Channel scan: codeplug scan list (members, priority channels 1/2, dwell) or the current zone. Stops on a carrier
 * (RF chip carrier detect, the stock squelch register) at or above the squelch level, listens while it lasts, then
 * holds (scan list dwell, default 3 s) and moves on. Priority channel 1 is checked after every 4 other channels.
 * Nuisance delete drops the channel the scan stopped on for this scan session. Scan skip channels are left out. */
#ifndef CFW_SCAN_H
#define CFW_SCAN_H
#include <stdbool.h>
#include <stdint.h>

void scan_start(int list);          /* codeplug scan list index, -1 = current zone (all channels if none) */
void scan_stop(bool stay);          /* stay on the channel scanning stopped on, else back to the start channel */
bool scan_active(void);
bool scan_holding(void);            /* stopped on a busy channel (listening or hold time) */
int  scan_list(void);
void scan_nuisance(void);           /* delete the current channel from this scan session */
int  scan_deleted(void);
void scan_tick(void);               /* every 10 ms (radio_tick) */
const char *scan_name(int list);    /* "Zone" for -1 */
#endif
