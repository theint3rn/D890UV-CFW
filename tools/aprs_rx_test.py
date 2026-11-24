#!/usr/bin/env python3
"""APRS receive test (emulator, either radio): AFSK 1200 audio played into the receiver's ADC (FM discriminator
input) on the FM built-in channel 2M CALL, the cfw's APRS receiver must list the stations (Menu > APRS heard).

  python3 cfw/tools/aprs_rx_test.py [--radio d890|d168]

Stream = two packets made here (not by the firmware): a position report from "XX0STK-9" (another station's format:
dest APAT81, path WIDE1-1,WIDE2-1) and a status text from "XX0ZZZ"; between them silence. If the APRS TX test ran
before (cfw/tools/aprs_test.py), its captured cfw beacon is appended as a third packet (cfw TX -> cfw RX).
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))
sys.path.insert(0, HERE)
from aprs_test import fcs  # noqa: E402

FS = 48000


def addr(call, last):
    c, _, s = call.partition("-")
    b = bytes((ord(ch) << 1) for ch in c.upper().ljust(6)[:6])
    return b + bytes([0x60 | ((int(s or 0) & 15) << 1) | (1 if last else 0)])


def frame(src, dst, path, info):
    calls = [dst, src] + path
    a = b"".join(addr(c, i == len(calls) - 1) for i, c in enumerate(calls))
    f = a + b"\x03\xf0" + info.encode()
    c = fcs(f)
    return f + bytes([c & 0xFF, c >> 8])


def afsk(fr, flags=40, tail=4, dev=3000.0):
    bits = []
    for _ in range(flags):
        bits += [(0x7E >> i) & 1 for i in range(8)]
    ones = 0
    for byte in fr:
        for i in range(8):
            b = (byte >> i) & 1
            bits.append(b)
            ones = ones + 1 if b else 0
            if ones == 5:
                bits.append(0)
                ones = 0
    for _ in range(tail):
        bits += [(0x7E >> i) & 1 for i in range(8)]
    tone, ph, out = 1, 0.0, []
    spb = FS / 1200.0
    n = 0.0
    for b in bits:
        if b == 0:
            tone ^= 1                                  # NRZI: 0 = change
        f = 1200.0 if tone else 2200.0
        n += spb
        k = int(round(n)) - len(out)
        for _ in range(k):
            ph += 2 * np.pi * f / FS
            out.append(np.sin(ph))
    # FM discriminator counts: 2048 + 0.25 count / Hz (emulator scale), deviation `dev` Hz peak
    return 2048 + 0.25 * dev * np.array(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    a = ap.parse_args()
    emu = os.path.join(SRC, "emu") if a.radio == "d890" else os.path.join(D168, "emu")
    sys.path.insert(0, emu)
    import txrx_loop as T
    first = T.first_builtin() if a.radio == "d890" else 1
    name = "aprs_rx_%s" % a.radio
    d = os.path.join(T.OUT, name)
    os.makedirs(d, exist_ok=True)
    gap = np.full(int(0.4 * FS), 2048.0)
    want = ["XX0STK-9", "XX0ZZZ"]
    parts = [gap, afsk(frame("XX0STK-9", "APAT81", ["WIDE1-1", "WIDE2-1"], "!3242.94N/11709.66W>stock-format test")), gap,
             afsk(frame("XX0ZZZ", "APRS", ["WIDE1-1"], ">cfw APRS RX check")), gap]
    cap = os.path.join(T.OUT, "aprs_%s" % a.radio, "tx_aprs.adc16")
    if os.path.exists(cap):
        x = np.fromfile(cap, dtype="<u2").astype(float)
        parts += [x, gap]
        want.append("XX0AAA")
    st = os.path.join(d, "rx.adc16")
    np.clip(np.concatenate(parts), 0, 4095).astype("<u2").tofile(st)
    dur = sum(len(p) for p in parts) / FS
    elf = T.ELF
    syms = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = l.split()
        if len(p) == 3:
            syms.setdefault(p[2], int(p[0], 16))
    steps = T.keys(first) + ["1.0|shot", "0|analog Stream @%s 48000" % st, "%.1f|shot" % (dur + 1.0),
                             "0|sysbus ReadByte 0x%08X" % syms["s_nheard"],
                             "0|sysbus ReadBytes 0x%08X %d" % (syms["s_heard"], 3 * 64),
                             "0|lcd Press 13 150", "0.4|version", "0|lcd Press 15 150", "0.4|shot",
                             "0|lcd Press 13 150", "0.5|shot", "0|lcd Press 13 150", "0.5|shot"]
    T.run(name, steps, seconds=6.0)
    out = open(os.path.join(d, "then_out.txt")).read()
    m = re.search(r"ReadByte 0x[0-9A-F]+\s*\n\s*0x([0-9A-F]+)", out)
    n = int(m.group(1), 16) if m else 0
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})\b", out.split("ReadBytes", 1)[1]))[:3 * 64] if "ReadBytes" in out else b""
    heard = []
    for i in range(min(n, 3)):
        e = raw[i * 64:(i + 1) * 64]
        call = e[:10].split(b"\0")[0].decode(errors="replace")
        info = e[10:54].split(b"\0")[0].decode(errors="replace")
        heard.append(call)
        print("heard %d: %-10s %s" % (i, call, info))
    ok = all(w in heard for w in want)
    shots = sorted(f for f in os.listdir(d) if f.startswith("lcd_") and f.endswith(".png"))
    subprocess.call([sys.executable, os.path.join(emu, "montage.py"), os.path.join(T.OUT, "aprs_rx_%s.png" % a.radio)]
                    + ["%d=%s" % (i + 1, os.path.join(d, f)) for i, f in enumerate(shots[-4:])])
    print("stations expected:", ", ".join(want))
    print("APRS receive:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
