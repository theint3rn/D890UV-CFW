/* Emergency alarm + work alone (stock CPS Alarm Setting, app/stockdlg.c): see emerg.c */
#ifndef CFW_EMERG_H
#define CFW_EMERG_H
#include <stdint.h>
#include <stdbool.h>

void emerg_start(const char *why, bool local);   /* local false = only the transmit part (man down has its own alarm) */
void emerg_cancel(void);
bool emerg_active(void);
void emerg_key(void);                            /* a key was pressed: work alone response */
bool emerg_warning(void);                        /* work alone warning running */
void emerg_tick(void);                           /* radio_tick */
/* DMR: voice LC of a received call with the emergency bit (Service Options 0x80) - called by the protocol glue */
void dmr_rx_emergency(uint32_t src, uint32_t dst);
bool dmr_tx_emergency(void);                     /* the call being keyed is an emergency call: set Service Options 0x80 */
extern volatile uint32_t g_emerg_dbg[8];         /* tests: state, starts, TX keyed, cycles, ended by itself, work alone warnings, received, cancelled */
#endif
