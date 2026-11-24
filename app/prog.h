/* CPS programming link (codeplug read/write over USART1). See app/prog.c. */
#ifndef CFW_PROG_H
#define CFW_PROG_H
#include <stdbool.h>
void prog_init(void);      /* after radio_init / power_set_save_hook */
void prog_poll(void);      /* every main-loop pass */
bool prog_active(void);    /* PROGRAM received, END / timeout not yet */
void prog_draw(void);      /* ui_draw() shows this screen while prog_active() */
#endif
