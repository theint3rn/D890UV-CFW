/* M-SMS packets (app/protos/tms.c) */
#ifndef CFW_TMS_H
#define CFW_TMS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* IPv4 + UDP 4007 + TMS for n Latin-1 chars (sent as UTF-16LE); out needs 34 + 2n bytes; returns the length.
 * src / dst = 24-bit radio ids (12.x.x.x), group = destination 225.x.x.x */
size_t tms_build(uint8_t *out, uint32_t src, uint32_t dst, bool group, const uint8_t *txt, uint16_t n, uint8_t seq);
/* text of an IPv4 / UDP 4007 TMS packet as Latin-1 (others '?'), leading CR / LF dropped; -1 = not a TMS packet */
int tms_parse(const uint8_t *p, size_t len, char *out, int max);
#endif
