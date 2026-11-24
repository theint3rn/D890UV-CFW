/* Key beep, volume limit, VOX, power save (app/spk.c) */
#ifndef CFW_SPK_H
#define CFW_SPK_H
#include <stdint.h>
#include <stdbool.h>

void     spk_tick(void);               /* main loop (radio_tick) */
void     spk_key(void);                /* a key was pressed: beep if Key beep is on */
void     spk_beep(uint16_t hz, uint16_t ms);
void     spk_stop(void);               /* end a beep now (PTT) */
bool     spk_vox_tx(void);             /* the current TX was keyed by VOX */
bool     spk_ps_sleeping(void);        /* power save: receiver off right now */
extern volatile uint32_t g_spk_dbg[4];

#endif
