#!/usr/bin/env python3
"""Power cut during a codeplug journal write (emulator, EMU=1 build, either radio).

  python3 cfw/tools/powercut_test.py [--radio d890|d168] [--elf ELF] [--cuts 1,3,9,...] [--jobs N]

Per cut point N: boot with a cfw codeplug, arm g_dbg_flash_cut = N (the firmware stops the CPU at the N-th erase /
1 KB program step in the codeplug region = power gone mid-write), change Settings > TX timeout one step and leave
Settings (journal write), then reset + power on. After the reboot the codeplug must be valid (g_dbg_cp_boot b0)
and TX timeout must be the old or the new value (new once the redo record was committed). N = 0: no cut.
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

CUTS = [0, 1, 2, 5, 9, 10, 11, 12, 13, 16, 20, 21, 22, 23]


def syms(elf):
    out = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = l.split()
        if len(p) == 3:
            out.setdefault(p[2], int(p[0], 16))
    return out


def one(radio, elf, cut):
    R = U.RADIOS[radio]
    S = syms(elf)
    name = "pcut_%s_%02d" % (radio, cut)
    if radio == "d168":
        vec = S["g_vectors"]
        on = ["0|cpu SP `sysbus ReadDoubleWord 0x%08X`" % vec, "0|cpu PC `sysbus ReadDoubleWord 0x%08X`" % (vec + 4),
              '0|board Set "A0" false']                 # no bootloader in the D168 emulator: vector table by hand
    else:
        on = ["0|board PowerOn"]
    steps = ["0|sysbus WriteDoubleWord 0x%08X %d" % (S["g_dbg_flash_cut"], cut),
             "0|sysbus ReadByte 0x%08X" % (S["s_val"] + 1),            # TX timeout before
             "keys:14,14,13,10", "key:3", "key:16", "key:13",
             "0|lcd Press 14 150", "3|shot",                           # leave Settings: journal write (cut)
             "0|sysbus ReadDoubleWord 0x%08X" % S["g_dbg_flash_cut"],
             "0|machine Reset"] + on + [
             "6|shot",
             "0|sysbus ReadByte 0x%08X" % S["g_dbg_cp_boot"],
             "0|sysbus ReadByte 0x%08X" % (S["s_val"] + 1),            # TX timeout after the reboot
             "0|sysbus ReadDoubleWord 0x%08X" % (R["base"] + 0x60000 - 0x2000)]   # redo record (erased = done)
    r = subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", radio, "--elf", elf] + steps,
                       capture_output=True, text=True)
    outf = os.path.join(R["emu"], "out", name, "then_out.txt")
    vals = []
    if os.path.exists(outf):
        blocks = open(outf).read().split("\n> ")
        for b in blocks:
            l = b.strip().splitlines()
            if l and ("ReadByte" in l[0] or "ReadDoubleWord" in l[0]) and len(l) > 1:
                vals.append(int(l[1].strip(), 16))
    if len(vals) < 5:
        return cut, False, "no monitor output (%s)" % r.stdout[-300:]
    tot0, left, boot, tot1, rec = vals[0], vals[1], vals[2], vals[3], vals[4]
    cut_hit = cut != 0 and left == 0
    ok = bool(boot & 0x80) and bool(boot & 1) and tot1 in (tot0, (tot0 + 1) % 9) and rec != 0x314B424A          # no redo record left ("JBK1")
    if cut == 0:
        ok = ok and tot1 == (tot0 + 1) % 9
    info = "cut %s, boot 0x%02X (%s%s), TOT %d -> %d, record %s" % (
        "hit" if cut_hit else ("none" if cut == 0 else "not reached"), boot, "valid" if boot & 1 else "INVALID",
        ", redo applied" if boot & 2 else "", tot0, tot1, "left!" if rec == 0x314B424A else "none")
    return cut, ok, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890")
    ap.add_argument("--elf")
    ap.add_argument("--cuts", default=",".join(map(str, CUTS)))
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    R = U.RADIOS[a.radio]
    elf = os.path.abspath(a.elf) if a.elf else R["elf"].replace("/build/", "/build_emu/")
    cuts = [int(x) for x in a.cuts.split(",")]
    res = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for cut, ok, info in ex.map(lambda c: one(a.radio, elf, c), cuts):
            print("%s cut %2d: %s" % ("PASS" if ok else "FAIL", cut, info), flush=True)
            res.append(ok)
    print("RESULT:", "PASS" if all(res) else "FAIL", "%d/%d" % (sum(res), len(res)))
    return 0 if all(res) else 1


if __name__ == "__main__":
    sys.exit(main())
