/* weak defaults for the encryption hooks (glue.h): no keys, no encrypted TX */
#include "glue.h"

__attribute__((weak)) bool cfw_crypt_key(int i, uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id,
                                         const uint8_t **key, uint8_t *len)
{
    (void)i; (void)lo; (void)hi; (void)kind; (void)key_id; (void)key; (void)len;
    return false;
}
__attribute__((weak)) bool cfw_crypt_tx(uint8_t lo, uint8_t hi, uint8_t *kind, uint8_t *key_id, const uint8_t **key,
                                        uint8_t *len)
{
    (void)lo; (void)hi; (void)kind; (void)key_id; (void)key; (void)len;
    return false;
}
__attribute__((weak)) uint32_t cfw_crypt_iv(void) { return 0x5EED1234u; }
__attribute__((weak)) bool cfw_nxdn_any_ran(void) { return false; }
__attribute__((weak)) bool cfw_nxdn_tx_forbidden(uint32_t group) { (void)group; return false; }
