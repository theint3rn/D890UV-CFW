#!/usr/bin/env python3
"""Battery percentage (app/battdisp.c) in the emulator: voltage sweep, both displays, TX hold, smoothing, setting.

  python3 cfw/tools/battery_test.py [--radio d890|d168] [--elf ELF] [--only sweep,hold,menu]

  sweep  boots with the pack at 5.90 / 6.45 / 7.00 / 7.45 / 7.90 / 8.40 V, Battery = Percent in the codeplug: shown
         percent = the curve (computed here from CFW_BATT_CURVE), text and colour in the status bar, nothing but the
         voltage and the percent drawn right of the voltage's x; one boot with Icon: no percent
  hold   7.45 V: +/- 2 cV noise makes no flicker; PTT with the pack sagging to 6.40 V: value and filter stand still, also
         for 5 s after TX; a real drop to 7.00 V then moves at most one step per second and ends at the curve value
  menu   Settings > Battery = Percent: percent drawn, saved (journal), still on after a power cycle; Info screen shows both
Screenshots: <emu>/out/batt_*/ (kept)."""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cal_crc_test as CC  # noqa: E402
import stockopt_test as SO  # noqa: E402
import ui_test as U  # noqa: E402

sys.path.insert(0, U.CPS)
from cps import codeplug as C, stock as ST  # noqa: E402


def curve(radio):
    """CFW_BATT_CURVE: the board's own (D168UV board_cfg.h) if it has one, else battdisp.h"""
    files = [os.path.join(U.D168, "cfw", "include", "board_cfg.h")] if radio == "d168" else []
    for f in files + [os.path.join(HERE, "..", "app", "battdisp.h")]:
        m = re.search(r"#define\s+CFW_BATT_CURVE\s+((?:.*\\\n)*.*)", open(f).read())
        if m:
            pts = [(int(a), int(b)) for a, b in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", m.group(1))]
            return pts
    raise SystemExit("no CFW_BATT_CURVE")


def ui(radio, elf, cpf, name, steps, volts=None):
    old = os.path.join(U.RADIOS[radio]["emu"], "out", name, "tx_1.json")
    if os.path.exists(old):
        os.remove(old)
    ex = ["--extra", "gps Enabled false"] + (["--extra", "analog Battery %.3f" % volts] if volts else [])
    subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", radio, "--elf", elf,
                    "--codeplug", cpf] + ex + steps, capture_output=True, text=True)
    out = os.path.join(U.RADIOS[radio]["emu"], "out", name)
    rd = lambda f: open(os.path.join(out, f), errors="replace").read() if os.path.exists(os.path.join(out, f)) else ""  # noqa: E731
    tp = os.path.join(out, "then_out.txt")
    return (CC.monitor_values(tp) if os.path.exists(tp) else []), rd("text.txt"), out


def colours(png, w):
    """names of the colours in the top right corner (status bar, right of the voltage)"""
    im = Image.open(png).convert("RGB")
    s = im.width // w
    a = np.asarray(im.resize((im.width // s, im.height // s), Image.NEAREST))[0:11, w - 26:w]
    r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
    out = set()
    if ((r > 200) & (g < 60) & (b < 60)).sum() > 4:
        out.add("red")
    if ((r > 200) & (g > 80) & (g < 210) & (b < 60)).sum() > 4:
        out.add("orange")
    if ((r < 80) & (g > 180) & (b < 80)).sum() > 4:
        out.add("green")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--only", default="sweep,hold,menu")
    a = ap.parse_args()
    os.environ["EMU_NO_ANYTONE"] = "1"
    R = U.RADIOS[a.radio]
    elf, only, w = a.elf or R["elf"], a.only.split(","), 160 if a.radio == "d890" else 128
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    tmp = tempfile.mkdtemp(prefix="batt_")
    pts = curve(a.radio)
    pct = lambda cv: float(np.interp(cv, [p[0] for p in pts], [p[1] for p in pts]))      # noqa: E731
    dbg = lambda i: "0|sysbus ReadDoubleWord 0x%08X" % (sym["g_batt_dbg"] + 4 * i)       # noqa: E731
    nums = lambda v, k="ReadDoubleWord": [int(x.strip(), 16) for c, x in v if k in c]    # noqa: E731
    last = lambda d: os.path.join(d, sorted(f for f in os.listdir(d) if re.match(r"lcd_\d+\.png", f))[-1])  # noqa: E731
    res = []

    def cp(on):
        c = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
        if c.model != R["model"]:
            ST.convert_model(c, R["model"])
        ST.use(R["model"])
        ST.set_stock_value(c, "OptionalSetting", "MenuLowPower", "11")      # menus stay open
        c.settings.batt_percent = on
        p = os.path.join(tmp, "p%d.d8cp" % on)
        C.save(c, p)
        return p

    if "sweep" in only:
        lo, orange = pts[0][0], pct(655)
        for volts in (5.90, 6.45, 7.00, 7.45, 7.90, 8.40):
            v, t, d = ui(a.radio, elf, cp(True), "batt_%s_p%d" % (a.radio, round(volts * 100)), ["wait:1", dbg(0), dbg(1)], volts)
            n = nums(v)
            want = pct(volts * 100)
            col = "red" if want < 0.5 else "orange" if n and n[0] < orange else "green"
            got = colours(last(d), w)
            check = bool(n) and abs(n[0] - want) <= 1.0 and "%d%%" % n[0] in t.split("\n") and got == {col}
            SO.check(res, "%.2f V: percent, text, colour %s" % (volts, col), check, "%s want %.1f, colours %s" % (n, want, got))
        v, t, d = ui(a.radio, elf, cp(False), "batt_%s_icon" % a.radio, ["wait:1", dbg(0)], 7.45)
        SO.check(res, "Icon (default): no percent drawn", "%" not in "".join(x for x in t.split("\n") if re.match(r"^(\d+|--)%$", x)) and "7.4" in t, "")

    if "hold" in only:
        bat = lambda x: "0|analog Battery %.3f" % x                                      # noqa: E731
        st = ["wait:1", dbg(0), dbg(3)]
        for k in range(16):
            st += [bat(7.45 + (0.02 if k & 1 else -0.02)), "0.35|version"]
        st += [bat(7.45), dbg(0), dbg(3), "pttdown", "0.5|version", bat(6.40), "4|shot", dbg(0), dbg(1), dbg(2),
               "pttup", "2|version", dbg(0), dbg(1), dbg(2), bat(7.45), "8|version", dbg(0), dbg(2), dbg(3),
               bat(7.00), "5|version", dbg(0), "60|shot", dbg(0)]
        v, t, d = ui(a.radio, elf, cp(True), "batt_%s_hold" % a.radio, st, 7.45)
        n = nums(v)
        ok = len(n) == 15
        SO.check(res, "noise +/- 2 cV: at most one change", ok and n[3] - n[1] <= 1, str(n[:4]))
        tj = os.path.join(d, "tx_1.json")             # the emulator's TX capture of this run (the RF status has no TX)
        txs = json.load(open(tj))["duration_s"] if os.path.exists(tj) else 0
        tx = txs > 4.0
        SO.check(res, "TX on, pack sags to 6.40 V: value held", ok and tx and n[4] == n[2] and n[6] == 1 and n[5] >= 740, "keyed %.1f s, %s" % (txs, n[4:7]))
        SO.check(res, "2 s after TX still held", ok and n[7] == n[2] and n[9] == 1 and n[8] >= 740, str(n[7:10]))
        SO.check(res, "back at 7.45 V: same value", ok and n[11] == 0 and abs(n[10] - n[2]) <= 1, str(n[10:13]))
        SO.check(res, "drop to 7.00 V: no jump, ends at the curve", ok and n[10] - 6 <= n[-2] <= n[10] and abs(n[-1] - pct(700)) <= 1.5,
                 "after 5 s %s, end %s want %.1f" % (n[-2:-1], n[-1:], pct(700)))

    if "menu" in only:
        by = "0|sysbus ReadByte 0x%08X" % sym["s_rs_batt"]
        if a.radio == "d168":                           # no bootloader in the D168 emulator: vector table by hand
            vec = sym["g_vectors"]
            on = ["0|cpu SP `sysbus ReadDoubleWord 0x%08X`" % vec, "0|cpu PC `sysbus ReadDoubleWord 0x%08X`" % (vec + 4), '0|board Set "A0" false']
        else:
            on = ["0|board PowerOn"]
        steps = [by, "keys:14,14,13,10", "key:15x5", "key:13", "key:16", "key:13", "0.3|shot", by, "key:14", "3|shot", "key:14", "0.5|shot",
                 "keys:13,15,15,15,13", "0.3|shot", "keys:14,14",
                 "0|machine Reset"] + on + ["6|shot", by]
        v, t, d = ui(a.radio, elf, cp(False), "batt_%s_menu" % a.radio, steps, 7.90)
        b = nums(v, "ReadByte")
        SO.check(res, "menu: Icon -> Percent, saved, kept after a power cycle", b == [0, 1, 1] and "SAVED" in t, str(b))
        SO.check(res, "Info screen: voltage and percent", bool(re.search(r"^Bat 7\.\d\dV \d+%", t, re.M)), "")

    print("RESULT:", "PASS" if res and all(res) else "FAIL", "(%d/%d)" % (sum(res), len(res)))
    return 0 if res and all(res) else 1


if __name__ == "__main__":
    sys.exit(main())
