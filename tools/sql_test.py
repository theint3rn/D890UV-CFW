#!/usr/bin/env python3
"""Stock squelch calibration in the cfw (drivers/dac.c dac_sql_threshold, drivers/rf.c reg 0x49, app/radio.c open /
close), emulator, either radio.

  python3 cfw/tools/sql_test.py [--radio d890|d168] [--elf ELF] [--level 1..5]

The threshold the stock firmware would write to RF reg 0x49 is computed here, independently, from the calibration
block in the radio's emu/images/synth.pages (D890 FUN_080e373c: base UHF +0x10 / VHF +0x116 / 200-300 MHz +0x124,
level offsets 0 7 13 19 23, tables +0x1400 / +0x1500; D168 FUN_0807bf6c: base +0x10 / +0x50, 4 per level, tables
+0x620 / +0x600; threshold = base + level offset + table[ref] - table[f]). Then, with the model's reg 0x0E squelch
flag tied to reg 0x49 (`rf0 SquelchFlag true`):
  1. reg 0x49 on the receive channel = that threshold;
  2. rf0 Signal at RSSI threshold - 1: closed; threshold: open; threshold - 12: still open (stock hysteresis);
     threshold - 13: closed; threshold: open; SignalOff: closed   (g_dspb_fm_mute = squelch closed);
  3. same image without the calibration block: reg 0x49 = the threshold from drivers/cal_default.c.
RSSI scale as the emulator model: raw = 2 * (dBm + 160)."""
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
import cal_crc_test as CC  # noqa: E402

sys.path.insert(0, os.path.join(U.CPS))
from cps import codeplug as C, stock as ST  # noqa: E402

SQ = {   # base offsets (UHF, VHF, 200-300 MHz), tables (UHF off, ref, n), (VHF off, ref, n, 200-300 index), offsets
    "d890": dict(bu=0x10, bv=0x116, b2=0x124, tu=(0x1400, 8, 0x19), tv=(0x1500, 5, 0x19, 20), off=(0, 7, 13, 19, 23)),
    "d168": dict(bu=0x10, bv=0x50, b2=None, tu=(0x620, 8, 0x19), tv=(0x600, 10, 0x14, None), off=(0, 4, 8, 12, 16)),
}


def threshold(radio, blk, hz, level):
    s, f = SQ[radio], hz // 10
    off = s["off"][max(0, min(4, level - 1))]
    u16 = lambda o: struct.unpack_from("<H", blk, o)[0]  # noqa: E731
    if f > 30000000:
        t, ref, n = s["tu"]
        i = min(max(0, (f - 40000000) // 500000), n - 1) if f >= 40000000 else 0
        return (u16(s["bu"]) + off + blk[t + ref] - blk[t + i]) & 0xFFFF
    t, ref, n, i2 = s["tv"]
    if f < 20000000 or s["b2"] is None:
        i = min(max(0, (f - 13600000) // 200000), n - 1) if f >= 13600000 else 0
        return (u16(s["bv"]) + off + blk[t + ref] - blk[t + i]) & 0xFFFF
    return (u16(s["b2"]) + off + blk[t + ref] - blk[t + i2]) & 0xFFFF


def cal_default(radio):
    src = os.path.join(U.SRC, "cfw") if radio == "d890" else os.path.join(U.D168, "cfw")
    txt = open(os.path.join(src, "drivers", "cal_default.c")).read()
    body = txt[txt.index("{", txt.index("cal_default[")):]
    body = re.sub(r"/\*.*?\*/", "", body[:body.index("};")], flags=re.S)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})\b", body))


def run(radio, elf, nand, cpf, name, steps):
    args = ["--runarg", "--nand", "--runarg", nand] + (["--runarg", "--no-rfcal"] if radio == "d890" else [])
    subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", radio, "--elf", elf,
                    "--codeplug", cpf] + args + steps, capture_output=True, text=True)
    return CC.monitor_values(os.path.join(U.RADIOS[radio]["emu"], "out", name, "then_out.txt"))


def status(vals):
    st = [v for c, v in vals if "Status" in c][-1]
    return int(re.search(r"tuned (\d+) Hz", st).group(1)), int(re.search(r"reg 0x49 = 0x([0-9A-Fa-f]+)", st).group(1), 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--level", type=int, default=3)
    ap.add_argument("--nand")
    a = ap.parse_args()
    R, CM, psz = U.RADIOS[a.radio], CC.calmap.CAL[a.radio], CC.PAGE[a.radio]
    elf = a.elf or R["elf"]
    nand = a.nand or os.path.join(R["emu"], "images", "synth.pages")
    if not os.path.exists(nand):
        subprocess.check_call([sys.executable, os.path.join(R["emu"], "make_nand.py"), "--out", nand])
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    tmp = tempfile.mkdtemp(prefix="sqltest_")
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != R["model"]:
        ST.convert_model(cp, R["model"])
    ST.set_stock_value(cp, "OptionalSetting", "SQL1", str(a.level))
    cpf = os.path.join(tmp, "sql.d8cp")
    C.save(cp, cpf)
    pages = CC.load_pages(nand, psz)
    blk = CC.read(pages, psz, CM["a"], 0x2000)
    ok_all = True

    v = run(a.radio, elf, nand, cpf, "sql_%s_a" % a.radio, ["0|rf0 SquelchFlag true", "wait:0.5", "0|rf0 Status"])
    hz, reg = status(v)
    t = threshold(a.radio, blk, hz, a.level)
    ok = reg == t
    print("%s reg 0x49 at %.4f MHz, level %d: cfw 0x%04X, stock formula on the NAND calibration 0x%04X"
          % ("PASS" if ok else "FAIL", hz / 1e6, a.level, reg, t))
    ok_all &= ok

    mute = sym["g_dspb_fm_mute"]
    plan = [(t - 1, True), (t, False), (t - 12, False), (t - 13, True), (t, False), (None, True)]
    steps = ["0|rf0 SquelchFlag true", "wait:0.5"]
    for raw, _ in plan:
        steps += ["0|rf0 SignalOff" if raw is None else "0|rf0 Signal %d %.1f" % (hz, raw / 2 - 160), "wait:0.8",
                  "0|sysbus ReadByte 0x%08X" % mute]
    v = run(a.radio, elf, nand, cpf, "sql_%s_b" % a.radio, steps)
    got = [int(x.strip(), 16) & 1 for c, x in v if "ReadByte" in c]
    ok = len(got) == len(plan)
    for (raw, closed), g in zip(plan, got):
        good = bool(g) == closed
        ok &= good
        print("%s RSSI %s: squelch %s (expect %s)" % ("PASS" if good else "FAIL",
              "off" if raw is None else "0x%03X (%+d, %.1f dBm)" % (raw, raw - t, raw / 2 - 160),
              "closed" if g else "open", "closed" if closed else "open"))
    if len(got) != len(plan):
        print("FAIL only %d of %d read-backs" % (len(got), len(plan)))
    ok_all &= ok

    nocal = os.path.join(tmp, "nocal.pages")
    CC.write_pages(nocal, {pa: p for pa, p in pages.items() if not (CM["a"] <= pa < CM["b"] + CM["block"])})
    v = run(a.radio, elf, nocal, cpf, "sql_%s_c" % a.radio, ["0|rf0 SquelchFlag true", "wait:0.5", "0|rf0 Status"])
    hz2, reg2 = status(v)
    t2 = threshold(a.radio, cal_default(a.radio), hz2, a.level)
    ok = reg2 == t2
    print("%s no calibration block: reg 0x49 0x%04X, drivers/cal_default.c formula 0x%04X"
          % ("PASS" if ok else "FAIL", reg2, t2))
    ok_all &= ok
    print("RESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
