#ifndef CFW_UI_H
#define CFW_UI_H
#include <stdbool.h>

typedef struct {
    bool nand_ok;
    bool nand_marker;    /* "CHECKING DATA FOR D890UV" at NAND 0x04EC0000 */
} ui_boot_t;
extern ui_boot_t g_boot;

void ui_init(void);
void ui_boot_fault(const char *l1, const char *l2);   /* fault before the last reset (hal/safety.c) */
void ui_key(int key_event);
void ui_draw(void);
bool ui_on_main(void);

#endif
