/* M-SMS: Motorola-style text message service (TMS) packets over IPv4 / UDP port 4007, as carried in DMR IP packet
 * data (SAP 4). Built from the public descriptions of the format; see proto_dmr.c for what is (not) verified.
 * Host unit test: cfw/tools/tms_test.c. */
#include <string.h>
#include "tms.h"

#define TMS_PORT 4007
/* ---- M-SMS (Motorola-style TMS over IPv4 / UDP 4007) ---- */
static uint16_t ip_csum(const uint8_t *h, int n)
{
    uint32_t c = 0;
    for(int i = 0; i < n; i += 2) { c += (uint32_t)(h[i] << 8 | h[i + 1]); }
    while(c >> 16) { c = (c & 0xFFFFu) + (c >> 16); }
    return (uint16_t)~c;
}

/* IPv4 + UDP + TMS for text (ASCII / Latin-1 -> UTF-16LE); returns the packet length */
size_t tms_build(uint8_t *o, uint32_t src, uint32_t dst, bool group, const uint8_t *txt, uint16_t n, uint8_t seq)
{
    size_t tms = 2u + 4u + 2u * n, udp = 8u + tms, ip = 20u + udp;
    memset(o, 0, 28);
    o[0] = 0x45; o[2] = (uint8_t)(ip >> 8); o[3] = (uint8_t)ip; o[5] = seq; o[8] = 64; o[9] = 17;
    o[12] = 12; o[13] = (uint8_t)(src >> 16); o[14] = (uint8_t)(src >> 8); o[15] = (uint8_t)src;
    o[16] = group ? 225 : 12; o[17] = (uint8_t)(dst >> 16); o[18] = (uint8_t)(dst >> 8); o[19] = (uint8_t)dst;
    uint16_t c = ip_csum(o, 20); o[10] = (uint8_t)(c >> 8); o[11] = (uint8_t)c;
    uint8_t *u = o + 20;
    u[0] = TMS_PORT >> 8; u[1] = TMS_PORT & 0xFF; u[2] = TMS_PORT >> 8; u[3] = TMS_PORT & 0xFF;
    u[4] = (uint8_t)(udp >> 8); u[5] = (uint8_t)udp;                                 /* checksum 0 = none (IPv4) */
    uint8_t *t = u + 8;
    t[0] = (uint8_t)((tms - 2u) >> 8); t[1] = (uint8_t)(tms - 2u);
    t[2] = 0xA0; t[3] = 0x00; t[4] = (uint8_t)(0x80u | (seq & 0x1Fu)); t[5] = 0x04;
    for(uint16_t i = 0; i < n; i++) { t[6 + 2 * i] = txt[i]; t[7 + 2 * i] = 0; }
    return ip;
}

/* text of a TMS packet (IPv4 / UDP port 4007) as Latin-1 (others '?'), leading CR / LF dropped; -1 = not TMS */
int tms_parse(const uint8_t *p, size_t len, char *out, int max)
{
    if(len < 28 || (p[0] >> 4) != 4 || p[9] != 17) { return -1; }
    size_t ihl = (size_t)(p[0] & 15u) * 4u;
    if(ihl < 20 || len < ihl + 8u) { return -1; }
    const uint8_t *u = p + ihl;
    if((u[2] << 8 | u[3]) != TMS_PORT) { return -1; }
    size_t ul = (size_t)(u[4] << 8 | u[5]);
    if(ul < 8 || ihl + ul > len) { ul = len - ihl; }
    const uint8_t *t = u + 8, *end = u + ul;
    if(end - t < 4) { return -1; }
    size_t tl = (size_t)(t[0] << 8 | t[1]);
    if(t + 2 + tl < end) { end = t + 2 + tl; }
    const uint8_t *q = t + 2;
    uint8_t hdr = *q++;
    if(q >= end) { return -1; }
    q += 1 + *q;                                         /* address length + address */
    if(hdr & 0x20u) { while(q < end && (*q++ & 0x80u)) { } }   /* extension bytes (seq, encoding) */
    int n = 0;
    for(; q + 1 < end && n < max; q += 2) {
        uint16_t ch = (uint16_t)(q[0] | q[1] << 8);
        if(n == 0 && (ch == '\r' || ch == '\n')) { continue; }
        out[n++] = ch >= 0x20 && ch < 0x100 ? (char)ch : '?';
    }
    return n;
}

