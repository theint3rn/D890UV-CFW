#!/usr/bin/env python3
"""DMR talker alias TX / RX of app/protos/proto_dmr.c (ta_tx_build, ta_rx taken from the source) on the PC. The sent LCs
are checked against an independent decoder written from the TS 102 361-2 7.2.18 layout as other software reads it:
8 / 16 bit characters start at octet 3 of the header LC (the first of its 49 bits is reserved), 7 bit uses all 49.

  python3 cfw/tools/ta_host_test.py          (also part of `make test-libs`)"""
import os
import subprocess
import sys
import tempfile

CFW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(CFW, "app/protos/proto_dmr.c")).read()
CODE = SRC[SRC.index("/* ---- talker alias"):SRC.index("static uint32_t lpcn_hz")]
HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DMR_FLCO_TALKER_ALIAS_HDR 4
typedef struct { uint8_t flco, raw[9]; } dmr_lc_t;
static struct { uint8_t ta_bits[28], ta_have, ta_fmt, ta_len, ta_tx[4][9], ta_tx_n; struct { char alias[32]; } ci; } s;
static uint8_t g_fmt;
static uint8_t stockdlg_alias_format(void) { return g_fmt; }
%s
int main(int c, char **v)
{
    (void)c;
    g_fmt = (uint8_t)atoi(v[1]);
    ta_tx_build(v[2]);
    for(int b = 0; b < s.ta_tx_n; b++) {
        dmr_lc_t lc;
        memcpy(lc.raw, s.ta_tx[b], 9); lc.flco = lc.raw[0];
        for(int i = 0; i < 9; i++) { printf("%%02x", lc.raw[i]); }
        printf("\\n");
        ta_rx(&lc);
    }
    printf("rx=%%s\\n", s.ci.alias);
    return 0;
}
"""


def independent(lcs):
    """octets 2..8 of each LC in a row; format / length in octet 2 of the header"""
    data = b"".join(bytes.fromhex(x)[2:] for x in lcs)
    fmt, n = data[0] >> 6, data[0] >> 1 & 31
    if fmt == 0:
        v = "".join("{:08b}".format(b) for b in data)[7:]       # bit 0 of octet 2 is the first data bit
        return "".join(chr(int(v[7 * i:7 * i + 7], 2)) for i in range(n))
    raw = data[1:]                                              # octet 3 on; octet 2 bit 0 reserved
    return raw[:n].decode("latin1") if fmt == 1 else raw[:2 * n].decode("utf-16-be")


def main():
    tmp = tempfile.mkdtemp(prefix="ta_host_")
    c, exe = os.path.join(tmp, "t.c"), os.path.join(tmp, "t")
    open(c, "w").write(HARNESS % CODE)
    subprocess.check_call(["gcc", "-O1", "-Wall", "-Wextra", c, "-o", exe])
    fails = 0
    limit = {0: 27, 1: 31, 2: 13}
    want_fmt = {0: 1, 1: 0, 2: 3}
    for f in (0, 1, 2):
        for text in ("A", "XX0AAA", "XX0AAAF", "XX0AAA Alice", "N0CALL abcdefghijklm", "X" * 40):
            o = subprocess.run([exe, str(f), text], capture_output=True, text=True, check=True).stdout.split("\n")
            lcs = [x for x in o if x and not x.startswith("rx=")]
            rx = [x for x in o if x.startswith("rx=")][0][3:]
            exp = text[:limit[f]]
            ind = independent(lcs)
            hdr = bytes.fromhex(lcs[0])
            ok = ind == exp and rx == exp and hdr[2] >> 6 == want_fmt[f] and (f == 1 or not hdr[2] & 1) and \
                [int(x[:2], 16) for x in lcs] == list(range(4, 4 + len(lcs)))
            fails += not ok
            print("%s format %d %r: %d LCs, independent %r, ta_rx %r" % ("PASS" if ok else "FAIL", f, text, len(lcs), ind, rx))
    print("RESULT:", "PASS" if not fails else "FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
