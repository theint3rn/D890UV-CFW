/* Runtime side of the stock CPS common sections (app/stockopt.c reads them): SMS / call ring, TOT pre-alert, GPS
 * clock with the time zone, APRS auto beacon, roaming (Roaming Zone / Channel, auto roaming, GPS roaming), hot keys. */
#ifndef CFW_STOCKRUN_H
#define CFW_STOCKRUN_H
#include <stdint.h>
#include <stdbool.h>

void stockrun_tick(void);                 /* radio_tick (10 ms) */

/* roaming (Menu > Roaming) */
int  roam_zone(void);                     /* active roaming zone (stock CurRoamZone, 0-based) */
void roam_set_zone(int z);
bool roam_start(void);                    /* measure the zone's roaming channels now; false = nothing to do */
bool roam_busy(void);
int  roam_current(void);                  /* roaming channel in use (index), -1 = none */
const char *roam_result(void);            /* last result text ("Alpha Roam -62dBm", "Repeater not found") */
int  roam_last_rssi(int member);          /* dBm measured for zone member k in the last search (-200 = none) */
bool roam_auto_on(void);                  /* stock Auto Roaming (TimeRoamOn) */
int  gpsroam_active(void);                /* GPS roaming entry in effect (index), -1 none */

/* hot keys: long press of digit key d (1..9, 0 = hot key 10). Returns a menu to open (UM_* of ui_int.h), -1 = done
 * (call target set / quick call sent / popup), -2 = no hot key on that digit */
int  stockrun_hotkey(int digit, const char **l1, const char **l2);
extern volatile uint32_t g_stockrun_dbg[6];  /* emulator tests: rings, TOT pre-alerts, clock sets, beacons, roams, gps roams */
#endif
