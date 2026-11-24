/* Battery percentage for the status bar (Settings > Battery: Icon / Percent). Smoothed, held during TX. */
#ifndef CFW_BATTDISP_H
#define CFW_BATTDISP_H
#include <stdint.h>
#include <stdbool.h>
#include "hal.h"                       /* board_cfg.h overrides */

/* Voltage -> percent curve of the 2-cell Li-ion pack, {V*100, percent}, rising; a board may set its own (board_cfg.h).
 * From the stock firmware (same numbers in D890UV V1.05 and D168UV V1.07): the VOLTAGES 6.01 V (low limit, "Battery Volt
 * Too Low!") and 6.45 / 6.85 / 7.45 V (steps of the stock 4-state battery icon, u16 table 0x0814A2C6 on the D890UV).
 * The stock firmware has no percentage, so EVERY PERCENT VALUE is assumed (typical 2-cell Li-ion discharge at receive
 * load), and so are the voltages of the points above 7.45 V and of the 7.20 V point. */
#ifndef CFW_BATT_CURVE
#define CFW_BATT_CURVE { 601, 0 }, { 645, 5 }, { 685, 13 }, { 720, 25 }, { 745, 43 }, { 770, 62 }, { 790, 76 }, { 810, 88 }, { 830, 100 }
#endif

void     battdisp_tick(void);          /* every 10 ms (radio_tick) */
int      battdisp_percent(void);       /* 0..100, -1 = not measured yet */
uint16_t battdisp_centivolts(void);    /* smoothed V*100 */
#define  BATT_Q 1024u                  /* fixed point of the filtered V*100 */
uint16_t battdisp_curve_x10(uint32_t f);   /* V*100*BATT_Q -> percent*10, clamped */

#endif
