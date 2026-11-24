/* Man down, as the stock firmware does it (both radios have a LIS2DW12 on the I2C bus, hal/accel.c):
 *
 *   Settings (codeplug only, no radio menu item in stock): FailAlarm = on/off (stock settings +0x24), ManDownWait =
 *   seconds (+0x4F).
 *   Stock D168 V1.07 0x08028A20 (D890 V1.05 FUN_08037314 / FUN_080373dc, same scheme): every 2 task ticks read
 *   STATUS (DRDY) then SIXD_SRC bit XL (radio standing upright). XL clear for 30 polls -> countdown = wait + 1 s
 *   (RTC 1 s ticks); XL set again cancels; countdown reaching 1 -> emergency alarm (D168 FUN_08031ef8 -> FUN_0802f060,
 *   "Alarming" popup; Alarm Setting type 1..3 would also transmit, see D168UV/emu/README "Man down").
 *   cfw: same detection (50 ms polls, 30 in a row = 1.5 s), countdown ManDownWait s, then a local alarm: red
 *   "Alarming / MAN DOWN" popup + alarm tone (spk beeps 1750 Hz) until the radio stands up again or a key cancels it.
 *   The stock TX alarm (emergency call on the alarm channel) is not done: the cfw never keys the transmitter by itself
 *   (app/emerg.c: the Alarm Setting types with transmit run in the emulator builds only).
 * Emulator: `i2c Fall` / `i2c Upright` (D168UV emu/models/I2cBus168.cs, D890UV emu/models/I2cBus.cs). */
#include "mandown.h"
#include "hal.h"
#include "gfx.h"
#include "rset.h"
#include "spk.h"
#include "emerg.h"
#include "ui_int.h"

volatile uint32_t g_mandown_dbg[4];

enum { MD_OFF, MD_UP, MD_FALLEN, MD_COUNT, MD_ALARM };
static uint8_t s_state, s_low, s_tries;
static bool s_on, s_chip, s_cancelled;
static uint32_t s_poll, s_cfg_t, s_deadline, s_beep_t, s_pop_t;

bool mandown_alarm(void) { return s_state == MD_ALARM; }

void mandown_cancel(void)
{
    if(s_state != MD_ALARM) { return; }
    s_cancelled = true;
    s_state = MD_FALLEN;
    emerg_cancel();
}

static void alarm_tick(uint32_t now)
{
    if(now - s_beep_t >= 600u) { s_beep_t = now; spk_beep(1750, 300); }
    if(now - s_pop_t >= 1200u) { s_pop_t = now; ui_popup("Alarming", "MAN DOWN", C_RED); }
}

void mandown_tick(uint32_t now)
{
    if(now - s_cfg_t >= 1000u || s_cfg_t == 0) {                 /* codeplug lookup once a second */
        s_cfg_t = now | 1u;
        bool on = rset_mandown();
        if(on && !s_chip && s_tries < 3) { s_tries++; s_chip = accel_init(); g_mandown_dbg[3] = s_chip; }
        if(on != s_on) { s_on = on; s_state = on ? MD_UP : MD_OFF; s_low = 0; }
    }
    if(!s_on || !s_chip) { s_state = s_on ? s_state : MD_OFF; g_mandown_dbg[0] = s_state; return; }
    if(s_state == MD_ALARM) { alarm_tick(now); }
    if(now - s_poll < 50u) { return; }
    s_poll = now;
    int up = accel_upright();
    g_mandown_dbg[2]++;
    if(up < 0) { return; }                                        /* no new sample */
    if(up) {
        s_low = 0; s_cancelled = false;
        if(s_state == MD_ALARM) { emerg_cancel(); spk_stop(); ui_popup("MAN DOWN", "cancelled", C_GREEN); }
        s_state = MD_UP;
    } else if(s_state == MD_UP && ++s_low >= 30) {
        s_state = MD_COUNT;
        s_deadline = now + (uint32_t)rset_mandown_wait() * 1000u;
    }
    if(s_state == MD_UP && s_low) { g_mandown_dbg[0] = MD_FALLEN; } else { g_mandown_dbg[0] = s_state; }
    if(s_state == MD_COUNT && (int32_t)(now - s_deadline) >= 0 && !s_cancelled) {
        s_state = MD_ALARM;
        g_mandown_dbg[1]++;
        emerg_start("MAN DOWN", false);      /* transmit part of the Alarm Setting (not in HW builds) */
        s_beep_t = now - 600u; s_pop_t = now - 1200u;
        alarm_tick(now);
        g_mandown_dbg[0] = s_state;
    }
}
