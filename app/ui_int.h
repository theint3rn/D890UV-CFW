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
typedef struct um_scr {
    const char *title;
    int  (*count)(void);
    void (*item)(int i, char *label, char *value);
    void (*select)(int i);
    bool (*key)(int i, int k);                           /* extra keys; true = handled */
    void (*draw)(void);                                  /* custom screen (no list) */
    const char *(*help)(int i);
} um_scr_t;
void um_push(const um_scr_t *scr, int sel);
void um_pop(void);
void um_close(void);
void um_choose(const char *title, const char *const *opt, int n, int cur, void (*set)(int));
/* ui_stock.c: stock CPS common sections (Menu > Radio extras) */
const um_scr_t *ui_stock_root(void);

enum { UM_ROOT, UM_CALLLOG, UM_GPS, UM_SMS, UM_CONTACTS, UM_ZONES, UM_SCAN, UM_SETTINGS };
void um_open(int which);                             /* menu screen (from Main: Menu key, PF functions) */
bool um_active(void);
void um_key(int k);
void um_draw(void);
void um_resume(void);                                /* back from a ui.c screen opened from the menu */
#endif
