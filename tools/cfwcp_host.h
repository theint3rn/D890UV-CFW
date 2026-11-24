/* forced include for tools/cfwcp_host_test.c: the CFW_FLASH_EXTERNAL flash interface of board_cfg.h (D168UV) */
#include <stdint.h>
#define CFW_FLASH_EXTERNAL 1
void cfw_flash_unlock(void);
void cfw_flash_lock(void);
void cfw_flash_erase(uint32_t addr);
void cfw_flash_program16(uint32_t addr, const uint32_t w[4]);
uint32_t irq_save(void);
void irq_restore(uint32_t p);
