/* Host unit test of the M-SMS codec (app/protos/tms.c):
 *   gcc -I app/protos -o /tmp/tms_test tools/tms_test.c app/protos/tms.c && /tmp/tms_test
 * Reference packets are written out byte by byte here (not made by tms_build): the layout other radios send
 * (IPv4 12.x.x.x, UDP 4007 -> 4007, TMS length, header 0xA0/0xC0, address length, extension bytes, UTF-16LE text,
 * often starting with CR LF). */
#include <stdio.h>
#include <string.h>
#include "tms.h"

static int fails;
#define CHECK(c, m) do { if(!(c)) { printf("FAIL %s\n", m); fails++; } else { printf("ok   %s\n", m); } } while(0)

static size_t ref_packet(uint8_t *o, int ihl_words, const uint8_t *addr, int alen, uint8_t hdr, const uint8_t *ext,
                         int next, const char *text, int crlf, int dport)
{
    size_t ih = (size_t)ihl_words * 4u, n = strlen(text), pos;
    memset(o, 0, 600);
    o[0] = (uint8_t)(0x40 | ihl_words); o[8] = 64; o[9] = 17;
    o[12] = 12; o[13] = 0x98; o[14] = 0x6F; o[15] = 0xD1;               /* 12.152.111.209 = id 9990097 */
    o[16] = 12; o[17] = 0x98; o[18] = 0x6F; o[19] = 0xD2;
    uint8_t *u = o + ih, *t = u + 8;
    u[0] = 0x0F; u[1] = 0xA7; u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
    pos = 2;
    t[pos++] = hdr; t[pos++] = (uint8_t)alen;
    for(int i = 0; i < alen; i++) { t[pos++] = addr[i]; }
    for(int i = 0; i < next; i++) { t[pos++] = ext[i]; }
    if(crlf) { t[pos++] = '\r'; t[pos++] = 0; t[pos++] = '\n'; t[pos++] = 0; }
    for(size_t i = 0; i < n; i++) { t[pos++] = (uint8_t)text[i]; t[pos++] = 0; }
    t[0] = (uint8_t)((pos - 2) >> 8); t[1] = (uint8_t)(pos - 2);
    size_t ul = 8 + pos, tl = ih + ul;
    u[4] = (uint8_t)(ul >> 8); u[5] = (uint8_t)ul;
    o[2] = (uint8_t)(tl >> 8); o[3] = (uint8_t)tl;
    return tl;
}

int main(void)
{
    uint8_t p[600];
    char out[80];
    int n;
    static const uint8_t ext1[] = { 0x81, 0x04 };
    size_t l = ref_packet(p, 5, NULL, 0, 0xA0, ext1, 2, "HELLO 73", 1, 4007);
    n = tms_parse(p, l, out, 64); out[n < 0 ? 0 : n] = 0;
    CHECK(n == 8 && strcmp(out, "HELLO 73") == 0, "reference: header A0, ext 81 04, CR LF + UTF-16LE text");
    static const uint8_t addr[] = { 0x00, 0x98, 0x6F, 0xD2 };
    static const uint8_t ext2[] = { 0x8A, 0x84, 0x04 };
    l = ref_packet(p, 6, addr, 4, 0xE0, ext2, 3, "QSL?", 0, 4007);
    n = tms_parse(p, l, out, 64); out[n < 0 ? 0 : n] = 0;
    CHECK(n == 4 && strcmp(out, "QSL?") == 0, "reference: IP options, 4-byte address, 3 extension bytes");
    l = ref_packet(p, 5, NULL, 0, 0x80, NULL, 0, "no ext", 0, 4007);
    n = tms_parse(p, l, out, 64); out[n < 0 ? 0 : n] = 0;
    CHECK(n == 6 && strcmp(out, "no ext") == 0, "reference: header without extension bit");
    l = ref_packet(p, 5, NULL, 0, 0xA0, ext1, 2, "other port", 0, 5016);
    CHECK(tms_parse(p, l, out, 64) == -1, "UDP port 5016 is not TMS");
    /* round trip */
    l = tms_build(p, 9990201, 91, true, (const uint8_t *)"HI 73", 5, 3);
    n = tms_parse(p, l, out, 64); out[n < 0 ? 0 : n] = 0;
    CHECK(n == 5 && strcmp(out, "HI 73") == 0, "tms_build -> tms_parse");
    CHECK(p[12] == 12 && p[13] == 0x98 && p[14] == 0x70 && p[15] == 0x39 && p[16] == 225 && p[19] == 91,
          "addresses 12.152.112.57 -> 225.0.0.91");
    uint32_t c = 0;
    for(int i = 0; i < 20; i += 2) { c += (uint32_t)(p[i] << 8 | p[i + 1]); }
    while(c >> 16) { c = (c & 0xFFFF) + (c >> 16); }
    CHECK(c == 0xFFFF, "IPv4 header checksum");
    CHECK(p[22] == 0x0F && p[23] == 0xA7 && p[28 + 2] == 0xA0 && p[28 + 5] == 0x04 && p[28 + 6] == 'H' && p[28 + 7] == 0,
          "UDP 4007, TMS header A0, UTF-16LE encoding byte 04");
    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails != 0;
}
