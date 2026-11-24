#!/usr/bin/env python3
"""Radio edits journal of app/cfwcp.c on the PC (no emulator): builds tools/cfwcp_host_test.c with the real cfwcp.c,
writes the journal twice into synthetic D890UV codeplugs whose CPS stock entries end at every offset mod 4 (0-3), and
checks that the firmware reads its own journal back and that the CPS (cfw_cps decoder) reads the same edits.

Regression: with the stock entries not ending on a 4-byte boundary, stock_walk() parsed the 1-3 zero pad bytes plus
the journal marker as an entry, so the second journal write landed 4 bytes after the first marker and the CPS
decoded garbage (cps168 found it; cps/stock.py and web/js/stock.js skip the pad the same way).

  python3 cfw/tools/cfwcp_host_test.py          (also part of `make test-libs`)"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CFW = os.path.dirname(HERE)
CPS = os.path.join(CFW, "..", "cfw_cps")
sys.path.insert(0, CPS)
from cps import codeplug as C, stock as ST  # noqa: E402


def stock_end(data):
    n = struct.unpack_from("<H", data, 16)[0]
    for i in range(n):
        s, rs, cnt, off, _ = struct.unpack_from("<HHIII", data, 32 + 16 * i)
        if s == C.SEC_STOCK:
            sec, p = data[off:off + rs * cnt], 0
            while p + 6 <= len(sec):
                t, c, r, ln = struct.unpack_from("<BBHH", sec, p)
                if t == 0 and c == 0 and r == 0 and ln == 0:
                    break
                p += 6 + ln
            return p
    raise KeyError("no stock section")


def main():
    tmp = tempfile.mkdtemp(prefix="cfwcp_host_")
    exe = os.path.join(tmp, "cfwcp_host_test")
    subprocess.check_call(["gcc", "-O1", "-Wall", "-Wno-int-to-pointer-cast", "-include", "tools/cfwcp_host.h",
                           "-Iapp", "-Ihal", "-Idrivers", "-Iinclude", "-Idsp", "-Ivoc",
                           "tools/cfwcp_host_test.c", "app/cfwcp.c", "-o", exe], cwd=CFW)
    opt = ST.TABLES["OptionalSetting"]["columns"]
    edits = [(21, opt.index("TOT"), 0, "4"), (0xE0, 2, 0, "5"), (0xE0, 6, 0, "1"), (0xE2, 0, 3, "1"),
             (21, opt.index("TOT"), 0, "2")]          # a later entry wins
    seen, fails = set(), 0
    for extra in range(8):
        cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
        if cp.model != "D890UV":
            ST.convert_model(cp, "D890UV")
        ST.set_stock_value(cp, "OptionalSetting", "Ext_Opt1", str(10 ** extra))   # moves the stock entries' end
        data = C.encode(cp, for_radio=True)
        end = stock_end(data)
        if end % 4 in seen:
            continue
        seen.add(end % 4)
        src, dst = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
        open(src, "wb").write(data)
        r = subprocess.run([exe, src, dst] + ["%X,%d,%d,%s" % e for e in edits], capture_output=True, text=True)
        ok = r.returncode == 0 and r.stdout.startswith("read %d" % len(edits))
        detail = r.stdout.split("\n")[0]
        if ok:
            out = open(dst, "rb").read()
            total = struct.unpack_from("<I", out, 8)[0]
            try:
                cp2 = C.decode(out[:total])
            except Exception as ex:                     # noqa: BLE001  the journal decoded as garbage
                print("FAIL stock entries end %% 4 = %d: CPS decode error %r" % (end % 4, ex))
                fails += 1
                continue
            o = cp2.stock["OptionalSetting"][0]
            ok = (o[opt.index("TOT")] == "2" and cp2.settings.keys[2] == "monitor" and cp2.settings.encryption
                  and cp2.channels[3].power == "mid")
            detail += ", CPS decode TOT %s key2 %s enc %s ch3 %s" % (o[opt.index("TOT")], cp2.settings.keys[2],
                                                                    cp2.settings.encryption, cp2.channels[3].power)
        print("%s stock entries end %% 4 = %d: %s" % ("PASS" if ok else "FAIL", end % 4, detail))
        fails += not ok
    if len(seen) < 4:
        print("FAIL only offsets %s covered" % sorted(seen))
        fails += 1
    print("RESULT:", "PASS" if not fails else "FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
