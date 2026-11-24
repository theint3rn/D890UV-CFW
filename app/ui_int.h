/* Shared between the UI files (ui.c: main / mode / channel settings / info screens, ui_menu.c: menu tree). */
#ifndef CFW_UI_INT_H
#define CFW_UI_INT_H
#include <stdint.h>
#include <stdbool.h>

/* ui.c */
void ui_popup(const char *l1, const char *l2, uint16_t col);
void ui_header(const char *title);
void ui_softkeys(const char *left, const char *right);
char *ui_fmt_freq(char *p, uint32_t hz);
char *ui_fmt_time(char *p, uint32_t t, bool date);   /* "HH:MM" or "MM-DD HH:MM" */
void ui_show_main(void);
void ui_show_mode_picker(void);
void ui_show_chan_edit(void);
void ui_show_info(void);
void ui_keyfn(uint8_t fn);                           /* run a key function (rset.h KF_*) */

/* ui_menu.c */
enum { UM_ROOT, UM_CALLLOG, UM_GPS, UM_SMS, UM_CONTACTS, UM_ZONES, UM_SCAN, UM_SETTINGS };
void um_open(int which);                             /* menu screen (from Main: Menu key, PF functions) */
bool um_active(void);
void um_key(int k);
void um_draw(void);
void um_resume(void);                                /* back from a ui.c screen opened from the menu */
#endif
