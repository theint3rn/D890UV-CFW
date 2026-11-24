#!/usr/bin/env python3
"""Pre-flash safety fixes (docs/PREFLASH_AUDIT.md F1-F9) on either radio's emulator, EMU=1 builds:

  f1halt  fault injected during FM TX (g_dbg_fault = 2: handler halts after the TX kill) -> RF model not transmitting,
          PA enables low, APC DAC 0, fault record (magic, PC in the app) written
  f1reset same with a reset (g_dbg_fault = 1) -> the radio boots again, shows "FAULT pc=...", record marked shown,
          PTT still held -> no TX ("PTT HELD") until released; then TX works again
  f2cap   TX cap shortened to 2 s (g_dbg_tx_cap_ms) -> PA off + RF out of TX after 2 s, "TX STOPPED"; next PTT works
  f2stall main loop stalled during TX (g_dbg_stall) -> SysTick takes the PA + RF chip out of TX within ~2 s
  f4      "dirty" bootloader exit (tools/preflash_dirtyboot.S: PLL as SYSCLK, SysTick running + pending, PendSV and two
          IRQs pending, MSP low) -> the cfw boots to the main screen on the PLL, no fault, FM TX works
  f5      APC tables poked so the stock u16 formula would wrap (table < reference) -> APC 0; so it would exceed the band
          maximum -> clamped to the band maximum; calibration marked invalid -> TX refused ("No RF cal")
  f7      make EMU=1 HW_NOTX=1 build: PTT -> not transmitting, "TX inhibited"
  f9      HSE never ready (RCC / CRM model knob) -> g_clk_err, "CLK ERR" on the boot screen, TX refused
  f8      boot screen shows the build string "<tree> EMU <date>"

  python3 cfw/tools/preflash_test.py [--radio d890|d168] [--only f1halt,f5,...]"""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

FAULT_MAGIC = 0xFA17C0DE

BOARD = {
    "d890": dict(cfw=os.environ.get("CFW_SNAP") or os.path.join(U.SRC, "cfw"), elf="build_emu/cfw.elf", notx="build_emu_notx/cfw.elf",
                 pa=[(0x42020014, 15), (0x42021014, 1)], dac=0x46021808, sw=(0x46020C1C, 3), hse_knob=0x46020FFC,
                 cal_ok="s_cal_ok", cal_u="s_b0", cal_v="s_b1", tab_u="s_apc_u", tab_v="s_apc_v", nv=0x2D,
                 tree="D890"),
    "d168": dict(cfw=os.environ.get("CFW_SNAP") or os.path.join(U.D168, "cfw"), elf="build_emu/cfw168.elf", notx="build_emu_notx/cfw168.elf",
                 pa=[(0x4001180C, 6), (0x4001100C, 4)], dac=0x40007408, sw=(0x40021004, 2), hse_knob=0x400213FC,
                 cal_ok="s_apc_ok", cal_u="s_cu", cal_v="s_cv", tab_u="s_apc_u", tab_v="s_apc_v", nv=0x27,
                 tree="D168", stub_ram=0x20030000),
}


def blocks(path):
    res = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        l = b.strip().splitlines()
        res.append((l[0].strip() if l else "", "\n".join(l[1:]).strip()))
    return res


class Run:
    """one emulator run through ui_test.py; results read back in step order"""

    def __init__(self, a, B, name, steps, elf=None, extra=(), runargs=()):
        self.R = U.RADIOS[a.radio]
        self.out = os.path.join(self.R["emu"], "out", name)
        cmd = [sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", a.radio,
               "--elf", elf or os.path.join(B["cfw"], B["elf"])]
        for e in extra:
            cmd += ["--extra", e]
        for r in runargs:
            cmd += ["--runarg", r]
        r = subprocess.run(cmd + list(steps), capture_output=True, text=True)
        to = os.path.join(self.out, "then_out.txt")
        self.B = blocks(to) if os.path.exists(to) else []
        if not self.B:
            print(r.stdout[-1500:], r.stderr[-1500:])
        self.pos = 0
        tp = os.path.join(self.out, "text.txt")
        self.text = open(tp, errors="replace").read() if os.path.exists(tp) else ""

    def nxt(self, cmd):
        while self.pos < len(self.B):
            c, o = self.B[self.pos]
            self.pos += 1
            if cmd in c:
                return o
        return ""

    def num(self, cmd):
        m = re.findall(r"0x[0-9A-Fa-f]+", self.nxt(cmd))
        return int(m[-1], 16) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    B = BOARD[a.radio]
    only = set(x for x in a.only.split(",") if x)
    want = lambda t: not only or t in only   # noqa: E731
    elf = os.path.join(B["cfw"], B["elf"])
    if not os.environ.get("CFW_SNAP"):
        subprocess.run(["make", "-C", B["cfw"], "-j8", "EMU=1"], check=True, capture_output=True)
    nm = subprocess.check_output(["arm-none-eabi-nm", elf], text=True)
    S, dup = {}, set()
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 3:
            if p[2] in S:
                dup.add(p[2])
            S[p[2]] = int(p[0], 16)
    for n in (B["cal_ok"], B["cal_u"], B["cal_v"], B["tab_u"], B["tab_v"], "s_apc_max_u", "s_apc_max_v"):
        assert n not in dup, "static symbol %s is not unique in %s" % (n, elf)
    rec = S["g_fault_rec"]
    W = lambda sym, v: "0|sysbus WriteDoubleWord 0x%08X %d" % (S[sym], v)   # noqa: E731
    RD = lambda addr: "0|sysbus ReadDoubleWord 0x%08X" % addr                # noqa: E731
    pa_reads = [RD(p) for p, _ in B["pa"]]
    res = {}

    def pa_low(run):
        return all((run.num("0x%08X" % p) >> bit) & 1 == 0 for p, bit in B["pa"])

    def pa_high(run):
        return all((run.num("0x%08X" % p) >> bit) & 1 == 1 for p, bit in B["pa"])

    # ---- F1: fault during TX, handler halted after the kill
    if want("f1halt"):
        st = ["pttdown", "1.0|rf0 Transmitting"] + pa_reads + [RD(B["dac"]), W("g_dbg_fault", 2),
              "0.3|rf0 Transmitting"] + pa_reads + [RD(B["dac"]), RD(rec), RD(rec + 4)]
        r = Run(a, B, "preflash_f1halt_" + a.radio, st)
        t0 = r.nxt("rf0 Transmitting")
        hi = pa_high(r)
        dac0 = r.num("0x%08X" % B["dac"])
        t1 = r.nxt("rf0 Transmitting")
        lo = pa_low(r)
        dac1 = r.num("0x%08X" % B["dac"])
        magic, pc = r.num("0x%08X" % rec), r.num("0x%08X" % (rec + 4))
        res["f1halt"] = t0 == "True" and hi and dac0 > 0 and t1 == "False" and lo and dac1 == 0 and \
            magic == FAULT_MAGIC and 0x08000000 <= pc < 0x08200000
        print("f1halt: TX %s (PA %s, DAC 0x%X) -> fault -> TX %s, PA low %s, DAC 0x%X, record 0x%08X pc 0x%08X" %
              (t0, hi, dac0, t1, lo, dac1, magic & 0xFFFFFFFF, pc & 0xFFFFFFFF))

    # ---- F1: fault during TX with the reset, then the boot after it
    if want("f1reset"):
        st = ["pttdown", "1.0|rf0 Transmitting", W("g_dbg_fault", 1), "0.2|rf0 Transmitting", "6|shot",
              "0|rf0 Transmitting", RD(rec), RD(rec + 24), "pttup", "0.5|", "pttdown", "1.0|rf0 Transmitting",
              "pttup", "0.5|shot"]
        r = Run(a, B, "preflash_f1reset_" + a.radio, st)
        t0, t1, t2 = r.nxt("rf0 Transmitting"), r.nxt("rf0 Transmitting"), r.nxt("rf0 Transmitting")
        magic, cnt = r.num("0x%08X" % rec), r.num("0x%08X" % (rec + 24))
        t3 = r.nxt("rf0 Transmitting")
        shown = "FAULT pc=08" in r.text
        res["f1reset"] = t0 == "True" and t1 == "False" and t2 == "False" and magic == (~FAULT_MAGIC & 0xFFFFFFFF) \
            and cnt == 1 and shown and "PTT HELD" in r.text and t3 == "True"
        print("f1reset: TX %s -> fault -> %s; after the reboot (PTT still held) %s, record 0x%08X count %d, "
              "'FAULT pc=' shown %s, 'PTT HELD' %s; PTT again -> TX %s" %
              (t0, t1, t2, magic & 0xFFFFFFFF, cnt, shown, "PTT HELD" in r.text, t3))

    # ---- F2: TX cap (2 s for the test) and main-loop stall
    if want("f2cap") or want("f2stall"):
        st = [W("g_dbg_tx_cap_ms", 2000), "pttdown", "1.0|rf0 Transmitting", "1.6|rf0 Transmitting"] + pa_reads + \
             [RD(B["dac"]), "pttup", W("g_dbg_tx_cap_ms", 0), "0.5|", "pttdown", "0.6|rf0 Transmitting",
              W("g_dbg_stall", 1), "2.6|rf0 Transmitting"] + pa_reads + [RD(B["dac"]), W("g_dbg_stall", 0),
              "0.5|shot", "pttup", "0.5|", RD(S["g_safety_kills"])]
        r = Run(a, B, "preflash_f2_" + a.radio, st)
        t0, t1 = r.nxt("rf0 Transmitting"), r.nxt("rf0 Transmitting")
        lo1, dac1 = pa_low(r), r.num("0x%08X" % B["dac"])
        t2, t3 = r.nxt("rf0 Transmitting"), r.nxt("rf0 Transmitting")
        lo2, dac2 = pa_low(r), r.num("0x%08X" % B["dac"])
        kills = r.num("0x%08X" % S["g_safety_kills"])
        res["f2cap"] = t0 == "True" and t1 == "False" and lo1 and dac1 == 0 and "TX time limit" in r.text and t2 == "True"
        res["f2stall"] = t2 == "True" and t3 == "False" and lo2 and dac2 == 0 and "main loop stall" in r.text and \
            kills == 2
        print("f2cap: TX %s, after 2.6 s %s (PA low %s, DAC 0x%X), popup %s; next PTT -> %s" %
              (t0, t1, lo1, dac1, "TX time limit" in r.text, t2))
        print("f2stall: main loop stalled while transmitting -> after 2.6 s TX %s (PA low %s, DAC 0x%X), popup %s, "
              "forced stops %d" % (t3, lo2, dac2, "main loop stall" in r.text, kills))

    # ---- F4: dirty bootloader exit
    if want("f4"):
        out = os.path.join(U.RADIOS[a.radio]["emu"], "out", "preflash_f4_" + a.radio)
        os.makedirs(out, exist_ok=True)
        base = 0x08000000 if a.radio == "d890" else B["stub_ram"]
        o, binf = os.path.join(out, "dirtyboot.elf"), os.path.join(out, "dirtyboot.bin")
        subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m4", "-mthumb", "-nostdlib", "-x", "assembler-with-cpp",
                        "-D" + B["tree"], "-Wl,-e,stub_reset", "-Wl,--section-start=.vectors=0x%08X" % base,
                        "-Wl,-Ttext=0x%08X" % (base + 0x100), os.path.join(HERE, "preflash_dirtyboot.S"), "-o", o],
                       check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", o, binf], check=True)
        if a.radio == "d890":
            kw = dict(runargs=["--bootstub", binf])
        else:
            kw = dict(extra=["sysbus LoadBinary @%s 0x%08X" % (binf, base), "cpu VectorTableOffset 0x%08X" % base])
        st = ["1|shot", RD(B["sw"][0]), RD(S["g_clk_err"]), RD(rec), "pttdown", "0.8|rf0 Transmitting", "pttup",
              "0.5|shot"]
        r = Run(a, B, "preflash_f4_" + a.radio, st, **kw)
        sw = r.num("0x%08X" % B["sw"][0])
        clk = r.num("0x%08X" % S["g_clk_err"])
        magic = r.num("0x%08X" % rec)
        t0 = r.nxt("rf0 Transmitting")
        main_screen = "Menu" in r.text or "MENU" in r.text or "CH" in r.text
        res["f4"] = (sw >> 2) & 3 == B["sw"][1] and (clk & 0xFF) == 0 and magic != FAULT_MAGIC and t0 == "True" \
            and main_screen
        print("f4: dirty boot -> SYSCLK source %d (PLL = %d), clk_err %d, fault record 0x%08X, main screen %s, TX %s" %
              ((sw >> 2) & 3, B["sw"][1], clk & 0xFF, magic & 0xFFFFFFFF, main_screen, t0))

    # ---- F5: APC clamp + no calibration
    if want("f5"):
        out = os.path.join(U.RADIOS[a.radio]["emu"], "out", "preflash_f5_" + a.radio)
        os.makedirs(out, exist_ok=True)
        nu, nv = 0x79, B["nv"]

        def table(n, ref, fill, refv):
            t = bytearray([fill]) * (4 * n)
            for l in range(4):
                t[l * n + ref] = refv
            return t
        files = {}
        for nm_, data in (("lo_u", table(nu, 0x28, 0x00, 0x80)), ("lo_v", table(nv, 9, 0x00, 0x80)),
                          ("hi_u", table(nu, 0x28, 0xFF, 0x00)), ("hi_v", table(nv, 9, 0xFF, 0x00)),
                          ("cal0", bytes(6)), ("calff", bytes([0xFF]) * 6)):
            files[nm_] = os.path.join(out, nm_ + ".bin")
            open(files[nm_], "wb").write(data)
        L = lambda f, sym: "0|sysbus LoadBinary @%s 0x%08X" % (files[f], S[sym])   # noqa: E731
        st = ["0|sysbus ReadByte 0x%08X" % S["s_apc_max_u"], "0|sysbus ReadByte 0x%08X" % S["s_apc_max_v"],
              L("lo_u", B["tab_u"]), L("lo_v", B["tab_v"]), L("cal0", B["cal_u"]), L("cal0", B["cal_v"]),
              "pttdown", "0.6|rf0 Status", RD(B["dac"]), "pttup", "0.4|",
              L("hi_u", B["tab_u"]), L("hi_v", B["tab_v"]), L("calff", B["cal_u"]), L("calff", B["cal_v"]),
              "pttdown", "0.6|rf0 Transmitting", RD(B["dac"]), "pttup", "0.4|",
              "0|sysbus WriteByte 0x%08X 0" % S[B["cal_ok"]], "pttdown", "0.6|rf0 Transmitting", "pttup", "0.5|shot"]
        r = Run(a, B, "preflash_f5_" + a.radio, st)
        mu, mv = r.num("0x%08X" % S["s_apc_max_u"]) & 0xFF, r.num("0x%08X" % S["s_apc_max_v"]) & 0xFF
        stt = r.nxt("rf0 Status")
        hz = int(re.search(r"tuned (\d+)", stt).group(1)) if re.search(r"tuned (\d+)", stt) else 0
        d_lo = r.num("0x%08X" % B["dac"])
        t_hi = r.nxt("rf0 Transmitting")
        d_hi = r.num("0x%08X" % B["dac"])
        t_nc = r.nxt("rf0 Transmitting")
        mx = mu if hz > 300000000 else mv
        old_lo = ((0 + 0 - 0x80) & 0xFFFF)
        old_lo = ((old_lo << 4) & 0xFFF) if a.radio == "d890" else ((old_lo & 0xFFF) << 4)
        res["f5"] = "carrier" in stt and d_lo == 0 and t_hi == "True" and d_hi == (mx << 4) and mx > 0 and \
            t_nc == "False" and "No RF cal" in r.text
        print("f5: TX %.4f MHz; table below reference -> DAC 0x%X (stock u16 math: 0x%X); above the band maximum -> "
              "DAC 0x%X (band max 0x%02X << 4); calibration invalid -> TX %s, popup %s" %
              (hz / 1e6, d_lo, old_lo, d_hi, mx, t_nc, "No RF cal" in r.text))

    # ---- F7: HW_NOTX build
    if want("f7"):
        if not os.environ.get("CFW_SNAP"):
            subprocess.run(["make", "-C", B["cfw"], "-j8", "EMU=1", "HW_NOTX=1"], check=True, capture_output=True)
        r = Run(a, B, "preflash_f7_" + a.radio, ["pttdown", "0.8|rf0 Transmitting", "0.2|shot", "pttup"],
                elf=os.path.join(B["cfw"], B["notx"]))
        t0 = r.nxt("rf0 Transmitting")
        res["f7"] = t0 == "False" and "TX inhibited" in r.text and (B["tree"] + " EMU+NOTX") in r.text
        print("f7: HW_NOTX build: PTT -> TX %s, popup %s, build string %s" %
              (t0, "TX inhibited" in r.text, (B["tree"] + " EMU+NOTX") in r.text))

    # ---- F9: HSE failure
    if want("f9"):
        r = Run(a, B, "preflash_f9_" + a.radio, [RD(S["g_clk_err"]), "pttdown", "0.8|rf0 Transmitting", "0.2|shot",
                                                  "pttup"], extra=["sysbus WriteDoubleWord 0x%08X 1" % B["hse_knob"]])
        clk = r.num("0x%08X" % S["g_clk_err"]) & 0xFF
        t0 = r.nxt("rf0 Transmitting")
        res["f9"] = clk == 1 and "CLK ERR" in r.text and t0 == "False"
        print("f9: HSE dead -> clk_err %d, 'CLK ERR' shown %s, PTT -> TX %s" % (clk, "CLK ERR" in r.text, t0))

    # ---- F8: build string on the boot screen (any run above saw the boot screen)
    if want("f8"):
        r = Run(a, B, "preflash_f8_" + a.radio, ["0|version"])
        res["f8"] = re.search(r"%s EMU \d{6}" % B["tree"], r.text) is not None
        print("f8: boot screen build string %s" % (re.search(r"%s EMU \d{6}" % B["tree"], r.text) or "missing"))

    for k, v in res.items():
        print("%s %s" % ("PASS" if v else "FAIL", k))
    ok = bool(res) and all(res.values())
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
