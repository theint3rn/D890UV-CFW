/* Man down (app/mandown.c) */
#ifndef CFW_MANDOWN_H
#define CFW_MANDOWN_H
#include <stdint.h>
#include <stdbool.h>

void mandown_tick(uint32_t now_ms);     /* from spk_tick */
bool mandown_alarm(void);               /* alarm active */
void mandown_cancel(void);              /* user cancel (key) - until the radio stands up again */
extern volatile uint32_t g_mandown_dbg[4];   /* state (0 off, 1 upright, 2 fallen / counting, 3 countdown, 4 alarm), alarms, polls, chip ok */

#endif
