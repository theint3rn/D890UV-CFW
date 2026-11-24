/* Battery percentage (see battdisp.h). The ADC value is sampled every 100 ms and low-pass filtered (3.2 s); while the
 * transmitter is on, and SETTLE_MS after it, the filter and the shown value stand still (the pack sags under load).
 * The shown value moves one step per second at most, and only when the filtered percentage is a full percent away from
 * it: after a change the new value has to drift by another whole percent, so it cannot flicker between two values. */
#include "battdisp.h"
#include "hal.h"
#include "radio.h"

#define SAMPLE_MS 100u
#define SETTLE_MS 5000u
#define STEP_MS   1000u
#define SEED_N    8u

static const struct { uint16_t cv, pct; } k_curve[] = { CFW_BATT_CURVE };
#define N_CURVE (sizeof k_curve / sizeof k_curve[0])

static uint32_t s_f;                   /* filtered V*100 * BATT_Q */
static int16_t  s_shown = -1;
static uint8_t  s_seed;
static uint32_t s_seed_sum, s_t_sample, s_t_step, s_t_tx;
static bool     s_txed;
volatile int32_t g_batt_dbg[4];        /* shown percent, filtered V*100, held (1 = TX / settling), changes */

uint16_t battdisp_curve_x10(uint32_t f)
{
    if(f <= k_curve[0].cv * BATT_Q) { return (uint16_t)(k_curve[0].pct * 10u); }
    for(unsigned i = 1; i < N_CURVE; i++) {
        uint32_t lo = k_curve[i - 1].cv * BATT_Q, hi = k_curve[i].cv * BATT_Q;
        if(f < hi) {
            uint32_t p0 = k_curve[i - 1].pct * 10u, p1 = k_curve[i].pct * 10u;
            return (uint16_t)(p0 + (p1 - p0) * (f - lo) / (hi - lo));
        }
    }
    return (uint16_t)(k_curve[N_CURVE - 1].pct * 10u);
}

void battdisp_tick(void)
{
    uint32_t now = hal_millis();
    if(g_radio.tx) { s_t_tx = now; s_txed = true; g_batt_dbg[2] = 1; return; }
    if(s_txed && now - s_t_tx < SETTLE_MS) { return; }
    s_txed = false;
    g_batt_dbg[2] = 0;
    uint32_t cv = battery_centivolts();
    if(s_seed < SEED_N) {              /* power on: mean of the first samples, shown at once */
        if(!cv && now < 3000u) { return; }              /* ADC not running yet */
        s_seed_sum += cv;
        if(++s_seed < SEED_N) { return; }
        s_f = s_seed_sum * BATT_Q / SEED_N;
        s_shown = (int16_t)((battdisp_curve_x10(s_f) + 5u) / 10u);
        s_t_sample = s_t_step = now;
    }
    if(now - s_t_sample >= SAMPLE_MS) {
        s_t_sample = now;
        s_f = (uint32_t)((int32_t)s_f + ((int32_t)(cv * BATT_Q) - (int32_t)s_f) / 32);
    }
    if(now - s_t_step >= STEP_MS) {
        int d = (int)battdisp_curve_x10(s_f) - s_shown * 10;
        if(d >= 10 || d <= -10) { s_shown += d > 0 ? 1 : -1; s_t_step = now; g_batt_dbg[3]++; }
    }
    g_batt_dbg[0] = s_shown;
    g_batt_dbg[1] = (int32_t)(s_f / BATT_Q);
}

int battdisp_percent(void) { return s_shown; }
uint16_t battdisp_centivolts(void) { return s_seed < SEED_N ? battery_centivolts() : (uint16_t)(s_f / BATT_Q); }
