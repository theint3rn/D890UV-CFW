#!/usr/bin/env python3
"""Scripted UI test of the cfw on either radio's emulator, with a cfw codeplug preloaded into internal flash.

  python3 cfw/tools/ui_test.py NAME [--radio d890|d168] [--codeplug FILE|none] [--elf ELF] [--boot S]
                               [--extra MONITOR_LINE] [--runarg RUN_PY_ARG] STEP...

The codeplug (default cfw_cps/examples/synth.d8cp, converted to the radio model) is encoded as the CPS writes it to
a radio (with the journal spare) and loaded at the radio's codeplug base before the firmware starts.
STEP is "SECS|monitor command" (as run.py --then) or a shortcut:
  key:CODE[xN]  lcd Press CODE 150, 0.35 s (N times), then a screenshot (13 Menu, 14 Exit, 15/16 Up/Down)
  keys:C,C,..   several key presses, one screenshot at the end
  long:CODE     lcd Press CODE 1300 (long press) + screenshot
  pf1 / pf2 / pf3 / ptt   side key press (150 ms) + screenshot (D168UV: PF1 PA13, PF2 PA14, PTT PD0)
  pttdown / pttup         hold / release PTT (no screenshot)
  knob:N        knob Turn N + screenshot
  shot          screenshot now          wait:S   run S seconds, then a screenshot
Output: <emu>/out/NAME/lcd_NN.png, montage.png, text.txt (strings the firmware drew, per screenshot)."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))            # D890UV_src
CPS = os.path.join(SRC, "cfw_cps")
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))
RADIOS = {
    "d890": dict(emu=os.path.join(SRC, "emu"), elf=os.path.join(SRC, "cfw", "build", "cfw.elf"), base=0x08180000,
                 model="D890UV", side={"pf1": "E2", "pf2": "A14", "pf3": "A8", "ptt": "A10"}, args=[]),
    "d168": dict(emu=os.path.join(D168, "emu"), elf=os.path.join(D168, "cfw", "build", "cfw168.elf"), base=0x080A0000,
                 model="D168UV", side={"pf1": "A13", "pf2": "A14", "ptt": "D0"}, args=["--cfw"]),
}

if os.environ.get("CFW_SNAP"):                 # snapshot of builds: <dir>/build/cfw.elf, build_emu/..., cfw168.elf (tests on a busy tree)
    for _k, _f in (("d890", "cfw.elf"), ("d168", "cfw168.elf")):
        RADIOS[_k]["elf"] = os.path.join(os.environ["CFW_SNAP"], "build", _f)


def codeplug_bin(path, model, out):
    sys.path.insert(0, CPS)
    from cps import codeplug as C, stock as ST
    cp = C.load(path)
    if cp.model != model:
        ST.convert_model(cp, model)
    data = C.encode(cp, for_radio=True)
    p = os.path.join(out, "codeplug.bin")
    open(p, "wb").write(data)
    return p


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    name, a = a[0], a[1:]
    radio, cpf, elf, boot, steps, extra = "d890", os.path.join(CPS, "examples", "synth.d8cp"), None, 4.0, [], []
    runargs = []
    while a:
        x = a.pop(0)
        if x == "--radio":
            radio = a.pop(0)
        elif x == "--codeplug":
            cpf = a.pop(0)
        elif x == "--elf":
            elf = a.pop(0)
        elif x == "--boot":
            boot = float(a.pop(0))
        elif x == "--extra":
            extra.append(a.pop(0))
        elif x == "--runarg":                 # passed to run.py as is (e.g. --runarg --bootstub --runarg FILE)
            runargs.append(a.pop(0))
        else:
            steps.append(x)
    R = RADIOS[radio]
    elf = os.path.abspath(elf) if elf else R["elf"]
    out = os.path.join(R["emu"], "out", name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith("lcd_"):
            os.remove(os.path.join(out, f))
    cmd = [sys.executable, os.path.join(R["emu"], "run.py")] + R["args"] + ["--elf", elf, "--seconds", str(boot),
                                                                          "--name", name, "--timeout", "2400"] + runargs
    if cpf != "none":
        cmd += ["--extra", "sysbus LoadBinary @%s 0x%08X" % (codeplug_bin(cpf, R["model"], out), R["base"])]
    for e in extra:
        cmd += ["--extra", e]
    nm = subprocess.check_output(["arm-none-eabi-nm", elf], text=True)
    gfx = next(int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == "gfx_text")
    txt = os.path.join(out, "text.txt")
    if os.path.exists(txt):
        os.remove(txt)
    cmd += ["--extra", ("cpu AddHook 0x%08X \"import System; p = self.GetRegister(2).RawValue; "
                        "s = ''.join([chr(self.Bus.ReadByte(p + i)) for i in range(40)]).split(chr(0))[0]; "
                        "System.IO.File.AppendAllText('%s', s + chr(10))\"") % (gfx & ~1, txt)]
    th = []
    for x in steps:
        if x == "shot":
            th.append("0|shot")
        elif x.startswith("wait:"):
            th.append("%s|shot" % x[5:])
        elif x.startswith("key:"):
            k, _, n = x[4:].partition("x")
            for _ in range(int(n or 1)):
                th += ["0|lcd Press %s 150" % k, "0.35|"]
            th[-1] = "0.35|shot"
        elif x.startswith("keys:"):
            for k in x[5:].split(","):
                th += ["0|lcd Press %s 150" % k, "0.35|"]
            th[-1] = "0.35|shot"
        elif x.startswith("long:"):
            th += ["0|lcd Press %s 1300" % x[5:], "1.6|shot"]
        elif x.startswith("knob:"):
            th += ["0|knob Turn %s" % x[5:], "0.4|shot"]
        elif x in R["side"]:
            th += ['0|board Press "%s" 150' % R["side"][x], "0.4|shot"]
        elif x == "pttdown":
            th.append('0|board Set "%s" false' % R["side"]["ptt"])
        elif x == "pttup":
            th.append('0|board Set "%s" true' % R["side"]["ptt"])
        else:
            th.append(x)
    # empty "SECS|" steps just advance time: run.py needs a command, use a harmless one
    th = [t if not t.endswith("|") else t + "version" for t in th]
    for t in th:
        cmd += ["--then", t]
    r = subprocess.run(cmd, cwd=R["emu"], capture_output=True, text=True)
    tail = r.stdout.strip().splitlines()[-4:]
    print("\n".join(tail))
    if r.returncode:
        print(r.stderr[-2000:])
    shots = sorted(f for f in os.listdir(out) if f.startswith("lcd_") and f.endswith(".png"))
    if shots:
        args = ["%d=%s" % (i + 1, os.path.join(out, f)) for i, f in enumerate(shots)]
        subprocess.run([sys.executable, os.path.join(R["emu"], "montage.py"), os.path.join(out, "montage.png")] + args,
                       cwd=R["emu"])
        print("montage:", os.path.join(out, "montage.png"), len(shots), "shots")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
