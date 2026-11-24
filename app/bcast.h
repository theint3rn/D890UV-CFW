/* Broadcast receiver (board driver drivers/bcast.c): FM broadcast on both radios, AM air band on the D890UV.
 *   D890UV: AM/FM receiver IC on the I2C bus, address 0x35 (8-bit 0x6A / 0x6B; stock V1.05 FUN_08139728 write,
 *           FUN_080df0fc read). FM: the IC tunes 64-108 MHz itself. AM air band (108-137 MHz): RF chip 1 converts
 *           the air band frequency +/- 2.925 MHz (stock AM Offset) down to 2.925 MHz, the IC demodulates AM there.
 *   D168UV: FM receiver at I2C 0x10 (RDA5807-style, sequential writes of regs 0x02..0x05; stock V1.07
 *           FUN_08078058 off / FUN_08078078 tune). No AM.
 * The receiver's audio path to the speaker is not traced (assumed: it shares the RF audio path; the cfw selects
 * AUDIO_RF). Unverified on hardware. */
#ifndef CFW_BCAST_H
#define CFW_BCAST_H
#include <stdint.h>
#include <stdbool.h>

enum { BC_OFF = 0, BC_FM = 1, BC_AM = 2 };
bool     bcast_has_am(void);
bool     bcast_on(int band, uint32_t hz);      /* power up (if needed) and tune; false = receiver not answering */
void     bcast_off(void);
int      bcast_band(void);                     /* BC_* */
uint32_t bcast_hz(void);
void     bcast_volume(uint8_t v);              /* 0..15 */
bool     bcast_ack(void);                      /* last I2C transfer acknowledged */
uint32_t bcast_writes(void);                   /* I2C register writes so far (tests) */
#define BC_FM_MIN  64000000u
#define BC_FM_MAX 108000000u
#define BC_AM_MIN 108000000u
#define BC_AM_MAX 137000000u
#endif
