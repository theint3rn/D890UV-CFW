#!/usr/bin/env python3
"""RF calibration boot report (hal/safety.c cal_report, docs/CALIBRATION.md section 3) in the emulator, either radio.

  python3 cfw/tools/cal_crc_test.py [--radio d890|d168] [--elf ELF] [--nand PAGES]

1. cfw on the radio's synthetic NAND (emu/images/synth.pages, made by emu/make_nand.py; no AnyTone files): after boot
   g_cal_info must say copy A, CRC-32 = tools/hw/calmap.py crc() of the same block (first 0x2000 / 0x800 bytes),
   span 0x2000 / 0x800, and the Info screen must show "Cal A <crc>".
2. The same image with the calibration copies removed: copy none ("Cal none: TX off" on the Info screen) and PTT
   refused ("No RF cal").
The RTT line itself is DEBUG=1 only (hw.py log); it prints these same fields."""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

sys.path.insert(0, os.path.join(U.SRC, "tools", "hw"))
import calmap  # noqa: E402

PAGE = {"d890": 0x1000, "d168": 0x800}


def load_pages(path, psz):
    d, raw = {}, open(path, "rb").read()
    rec = 4 + psz
    for i in range(0, len(raw) - rec + 1, rec):
        d[struct.unpack_from("<I", raw, i)[0]] = raw[i + 4:i + rec]
    return d


def read(pages, psz, addr, n):
    out = bytearray()
    while len(out) < n:
        a = addr + len(out)
        pa, off = a - a % psz, a % psz
        p = pages.get(pa, b"\xff" * psz)
        out += p[off:off + min(psz - off, n - len(out))]
    return bytes(out)


def write_pages(path, pages):
    with open(path, "wb") as f:
        for pa in sorted(pages):
            f.write(struct.pack("<I", pa) + pages[pa])


def monitor_values(path):
    vals = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        ln = b.strip().splitlines()
        if len(ln) > 1:
            vals.append((ln[0], "\n".join(ln[1:])))
    return vals


def run(radio, elf, nand, name, sym):
    R = U.RADIOS[radio]
    a = sym["g_cal_info"]
    steps = ["0|sysbus ReadDoubleWord 0x%08X" % (a + o) for o in (0, 4, 8)]
    steps += ["keys:" + ",".join(["13"] + ["16"] * 16 + ["13"]),    # Menu, down to "Info", Select
              "keys:14,14", "pttdown", "wait:0.6", "pttup"]               # back to the main screen, PTT
    args = ["--runarg", "--nand", "--runarg", nand] + (["--runarg", "--no-rfcal"] if radio == "d890" else [])
    subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", radio, "--elf", elf] + args
                   + steps, capture_output=True, text=True)
    out = os.path.join(R["emu"], "out", name)
    v = [int(x[1].strip(), 16) for x in monitor_values(os.path.join(out, "then_out.txt")) if "ReadDoubleWord" in x[0]]
    text = open(os.path.join(out, "text.txt")).read() if os.path.exists(os.path.join(out, "text.txt")) else ""
    return v, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--nand", help="NAND pages file (default: the radio's emu/images/synth.pages)")
    a = ap.parse_args()
    R, C, psz = U.RADIOS[a.radio], calmap.CAL[a.radio], PAGE[a.radio]
    elf = a.elf or R["elf"]
    nand = a.nand or os.path.join(R["emu"], "images", "synth.pages")
    if not os.path.exists(nand):
        subprocess.check_call([sys.executable, os.path.join(R["emu"], "make_nand.py"), "--out", nand])
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    pages = load_pages(nand, psz)
    blk_a = read(pages, psz, C["a"], C["block"])
    blk_b = read(pages, psz, C["b"], C["block"])
    exp_copy = "A" if calmap.markers(a.radio, blk_a)["valid"] else "B" if calmap.markers(a.radio, blk_b)["valid"] else None
    blk = blk_a if exp_copy == "A" else blk_b
    exp_crc = calmap.crc(a.radio, blk) if exp_copy else 0
    ok_all = True

    v, text = run(a.radio, elf, nand, "calcrc_%s" % a.radio, sym)
    ok = len(v) == 3
    if ok:
        copy, crc, span = chr(v[0] & 0xFF) if v[0] & 0xFF else None, v[1], v[2]
        ok = copy == exp_copy and crc == exp_crc and span == C["crc_span"]
        shown = ("Cal %s %08X" % (exp_copy, exp_crc)) in text
        print("%s synth.pages: cfw copy %s crc32 %08x span 0x%x / calmap copy %s crc32 %08x span 0x%x; Info screen %s"
              % ("PASS" if ok and shown else "FAIL", copy, crc, span, exp_copy, exp_crc, C["crc_span"],
                 "shows it" if shown else "does NOT show it"))
        ok &= shown
    else:
        print("FAIL synth.pages: no g_cal_info read-back (%r)" % v)
    ok_all &= ok

    tmp = tempfile.mkdtemp(prefix="calcrc_")
    nocal = os.path.join(tmp, "nocal.pages")
    write_pages(nocal, {pa: p for pa, p in pages.items() if not (C["a"] <= pa < C["b"] + C["block"])})
    v, text = run(a.radio, elf, nocal, "calcrc_none_%s" % a.radio, sym)
    ok = len(v) == 3 and v[0] & 0xFF == 0 and "Cal none: TX off" in text and "No RF cal" in text
    print("%s no calibration block: copy byte %s, Info 'Cal none' %s, PTT refused 'No RF cal' %s"
          % ("PASS" if ok else "FAIL", v[0] & 0xFF if v else "?", "Cal none: TX off" in text, "No RF cal" in text))
    ok_all &= ok
    print("RESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
