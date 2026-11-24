#!/usr/bin/env python3
"""Scripted cfw run in the Renode emulator with screenshots and a montage.

  python3 cfw/tools/emutest.py NAME [--elf cfw/build/cfw.elf] [--boot 4] STEP...

STEP is "SECS|monitor command" (as emu/run.py --then) or one of these shortcuts:
  key:CODE     lcd Press CODE 150, then 0.4 s, then a screenshot   (13 Menu, 14 Exit, 15/16 Up/Down, 19/20 P1/P2)
  pf3 / pf1 / pf2 / ptt   side key press (150 ms) + screenshot
  knob:N       knob Turn N + screenshot
  shot         screenshot now
  wait:S       run S seconds
Output: emu/out/NAME/lcd_NN.png and emu/out/NAME/montage.png.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
SIDE = {"pf3": "A8", "pf1": "E2", "pf2": "A14", "ptt": "A10"}


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    name, a = a[0], a[1:]
    elf, boot, steps = "cfw/build/cfw.elf", 4.0, []
    while a:
        x = a.pop(0)
        if x == "--elf":
            elf = a.pop(0)
        elif x == "--boot":
            boot = float(a.pop(0))
        elif x == "shot":
            steps.append("0|shot")
        elif x.startswith("wait:"):
            steps.append("%s|shot" % x[5:])
        elif x.startswith("key:"):
            steps += ["0|lcd Press %s 150" % x[4:], "0.4|shot"]
        elif x.startswith("knob:"):
            steps += ["0|knob Turn %s" % x[5:], "0.4|shot"]
        elif x in SIDE:
            steps += ['0|board Press "%s" 150' % SIDE[x], "0.4|shot"]
        else:
            steps.append(x)
    cmd = [sys.executable, "emu/run.py", "--elf", elf, "--seconds", str(boot), "--name", name, "--timeout", "1500"]
    for s in steps:
        cmd += ["--then", s]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    print("\n".join(r.stdout.strip().splitlines()[-6:]))
    out = os.path.join(SRC, "emu", "out", name)
    shots = sorted(f for f in os.listdir(out) if f.startswith("lcd_") and f.endswith(".png"))
    if shots:
        args = ["%d=%s" % (i + 1, os.path.join(out, f)) for i, f in enumerate(shots)]
        subprocess.run([sys.executable, os.path.join(SRC, "emu", "montage.py"), os.path.join(out, "montage.png")] + args,
                       cwd=SRC)
        print("montage:", os.path.join(out, "montage.png"), len(shots), "shots")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
