#!/usr/bin/env python3
"""D168UV TX power (APC) check: cfw DAC1 level per power level vs the stock formula on the stock RAM tables.

  python3 cfw/tools/apc_test168.py [--elf D168UV/cfw/build_emu/cfw168.elf] [--nand PAGES] [--boot-seconds 15]

--nand: NAND image for both runs (default: what D168UV/emu/run.py picks, images/extflash.pages if present, else
our own images/synth.pages from make_nand.py, so the test runs on a fresh clone without AnyTone files).

1. Stock V1.07 in the D168 emulator: after boot, read the calibration copies the stock firmware made from the
   emulator NAND (UHF struct 0x2000cc28, VHF struct 0x2000cc48, APC tables 0x2000cd62 / 0x2000cf46).
2. cfw: key the transmitter at power level 0..3 (s_power) and read DAC1 DHR12R1 (PA4).
3. Expected = stock FUN_0808ca20 on the stock tables for the cfw's TX frequency (s_freq while transmitting).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

D168 = U.D168


def monitor_values(path):
    vals = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        l = b.strip().splitlines()
        if len(l) > 1:
            vals.append((l[0], "\n".join(l[1:])))
    return vals


def bytes_of(txt):
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})\b", txt)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(D168, "cfw", "build_emu", "cfw168.elf"))
    ap.add_argument("--nand", help="NAND pages file (default: run.py's choice, extflash.pages else synth.pages)")
    ap.add_argument("--boot-seconds", type=float, default=15.0, help="stock boot time before the reads (default 15)")
    a = ap.parse_args()
    nand = ["--nand", os.path.abspath(a.nand)] if a.nand else []
    emu = os.path.join(D168, "emu")
    reads = [(0x2000cc28, 0x20), (0x2000cc48, 0x20), (0x2000cd62, 4 * 0x79), (0x2000cf46, 4 * 0x27)]
    cmd = [sys.executable, os.path.join(emu, "run.py"), "--seconds", str(a.boot_seconds), "--name", "apc_stock",
           "--timeout", "1800"] + nand
    for ad, n in reads:
        cmd += ["--then", "0|sysbus ReadBytes 0x%08X %d" % (ad, n)]
    subprocess.run(cmd, cwd=emu, capture_output=True, text=True)
    v = monitor_values(os.path.join(emu, "out", "apc_stock", "then_out.txt"))
    cu, cv, tu, tv = (bytes_of(x[1]) for x in v[:4])
    assert len(cu) == 0x20 and len(tu) == 4 * 0x79, (len(cu), len(tu))

    def expect(hz, l):
        f = hz // 10
        if f < 20000000:
            i = min(((f - 13600000) // 100000) & 0xFF, 0x26)
            return (cv[2 + l] + tv[l * 0x27 + i] - tv[l * 0x27 + 9]) & 0xFFFF
        if f > 30000000:
            i = min(((f - 40000000) // 100000) & 0xFF, 0x78)
            return (cu[2 + l] + tu[l * 0x79 + i] - tu[l * 0x79 + 0x28]) & 0xFFFF
        return cv[0x1E]

    S = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", a.elf], text=True).splitlines():
        p = l.split()
        if len(p) == 3:
            S.setdefault(p[2], int(p[0], 16))
    steps = []
    for lvl in range(4):
        steps += ["0|sysbus WriteByte 0x%08X %d" % (S["s_power"], lvl), "pttdown", "0.6|sysbus ReadDoubleWord 0x40007408",
                  "0|sysbus ReadDoubleWord 0x%08X" % S["s_freq"], "pttup", "0.6|version"]
    subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), "apc_cfw168", "--radio", "d168", "--elf", a.elf]
                   + sum((["--runarg", x] for x in nand), []) + steps, capture_output=True, text=True)
    v = [x for x in monitor_values(os.path.join(emu, "out", "apc_cfw168", "then_out.txt")) if "ReadDoubleWord" in x[0]]
    ok = len(v) == 8
    for lvl in range(4):
        if not ok:
            break
        dhr, hz = int(v[2 * lvl][1].strip(), 16), int(v[2 * lvl + 1][1].strip(), 16)
        got, exp = dhr >> 4, expect(hz, lvl) & 0xFFF
        good = got == exp
        ok &= good
        print("%s level %d: TX %.4f MHz, cfw DAC1 0x%03X, stock formula 0x%03X" % ("PASS" if good else "FAIL", lvl,
                                                                                   hz / 1e6, got, exp))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
