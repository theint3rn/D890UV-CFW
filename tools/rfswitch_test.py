#!/usr/bin/env python3
"""D890UV: RF reg 0x52 switch outputs and the speaker route the cfw writes, against the stock V1.05 code (emulator).

  python3 cfw/tools/rfswitch_test.py [--elf ELF]

Hooks rf_write(chip, reg, value) and keeps the eight outputs of each RF chip. Checked, with the stock source of each
expectation (decompile; docs/AUDIO_PATHS.md "Audio routing", docs/rf_chip.md "Reg 0x52"):
  receive VHF / UHF (FUN_0810d490), transmit VHF / UHF (FUN_0811b308), audio switches on chip 1 only (FUN_08118df8 ->
  FUN_08019ab4), FM broadcast (FUN_08111a58) and AM air band (FUN_08019b78), chip 0 outputs 1 / 2 never written high."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stockfx_test as FX  # noqa: E402
import stockopt_test as SO  # noqa: E402
import ui_test as U  # noqa: E402

VHF_RX, UHF_RX = {0: 0, 6: 1, 7: 1, 4: 0}, {0: 1, 6: 0, 7: 0, 4: 1}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf")
    a = ap.parse_args()
    os.environ["EMU_NO_ANYTONE"] = "1"
    R = U.RADIOS["d890"]
    elf = a.elf or R["elf"]
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    name = "rfswitch_d890"
    out = os.path.join(R["emu"], "out", name)
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "rfw.txt")
    if os.path.exists(log):
        os.remove(log)
    cpf = FX.build("D890UV", os.path.join(out, "sw.d8cp"), lambda cp, o: o("SQL1", 3))
    hook = ("cpu AddHook 0x%08X \"import System; c=self.GetRegister(0).RawValue&0xFF; r=self.GetRegister(1).RawValue&0xFF; "
            "v=self.GetRegister(2).RawValue&0xFFFF; (System.IO.File.AppendAllText('%s', '%%d %%02X %%04X' %% (c,r,v) + chr(10)) "
            "if r in (0x52,0x56) else None)\"") % (sym["rf_write"] & ~1, log)
    mark = lambda t: "0|python \"import System; System.IO.File.AppendAllText('%s', '# %s' + chr(10))\"" % (log, t)  # noqa: E731
    ch = SO.chan
    st = ["0|analog Set 2 2500", "0.5|version", mark("boot"), "0|rf0 SignalHere -30", "1|version", mark("vhf signal"),
          "0|rf0 SignalOff", "1|version", mark("vhf squelched"),
          "pttdown", "0.6|version", mark("vhf tx"), "pttup", "0.8|version", mark("vhf rx again")]
    st += ch(79) + ["0.6|version", mark("uhf rx"), "pttdown", "0.6|version", mark("uhf tx"), "pttup", "0.8|version", mark("uhf rx again")]
    st += ch(82) + ["0.8|version", mark("uhf dmr rx")]
    st += ch(1) + ["0.8|version", mark("vhf rx 3")]
    st += SO.EXTRAS + ["key:13", "key:16", "key:13", "wait:0.5", mark("fm broadcast"), "key:14", "wait:0.5", "key:14", mark("fm off")]
    st += ["key:16", "key:13", "key:16", "key:13", "key:13", "wait:0.5", mark("am"), "key:14", "wait:0.5", "key:14", mark("am off")]
    subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", "d890", "--elf", elf, "--codeplug", cpf, "--extra",
                    "gps Enabled false", "--extra", hook] + st, capture_output=True, text=True)
    sw, at, seq, cur = {0: [None] * 8, 1: [None] * 8}, {}, {}, []
    hi0 = False
    for ln in open(log):
        if ln[0] == "#":
            at[ln[2:].strip()] = {c: list(x) for c, x in sw.items()}
            seq[ln[2:].strip()] = cur
            cur = []
            continue
        c, r, v = int(ln[0]), int(ln[2:4], 16), int(ln[5:9], 16)
        cur.append((c, r, v))
        if r == 0x52:
            sw[c][v >> 12] = v & 1
            hi0 |= c == 0 and v >> 12 in (1, 2) and bool(v & 1)
    res = []
    ok0 = lambda k, want: all(at[k][0][i] == v for i, v in want.items())       # noqa: E731
    aud = lambda k: (at[k][1][1], at[k][1][2])                                # noqa: E731
    C = SO.check
    C(res, "chip 0 receive 146.520 MHz", ok0("boot", VHF_RX), str(at["boot"][0]))
    C(res, "FM audio: chip 1 switch 2 on, 1 off", aud("vhf signal") == (0, 1), str(aud("vhf signal")))
    C(res, "squelch closed: both switches off", aud("vhf squelched") == (0, 0), str(aud("vhf squelched")))
    for b, want in (("vhf", {0: 0, 6: 0, 5: 0, 3: 0}), ("uhf", {0: 0, 6: 0, 5: 0, 3: 1})):
        s = seq[b + " tx"]
        w0 = [x for x in s if x[0] == 0]
        C(res, "chip 0 transmit %s: switches, reg 0x56 = 2 before switch 3" % b, ok0(b + " tx", want) and (0, 0x56, 2) in w0
          and w0.index((0, 0x56, 2)) < max([i for i, x in enumerate(w0) if x[1] == 0x52 and x[2] >> 12 == 3], default=999), str(["%02X %04X" % x[1:] for x in w0]))
        C(res, "%s transmit: audio switches off" % b, aud(b + " tx") == (0, 0), str(aud(b + " tx")))
        C(res, "chip 0 receive again %s" % b, ok0(b + " rx again", VHF_RX if b == "vhf" else UHF_RX), str(at[b + " rx again"][0]))
    C(res, "chip 0 receive 442.450 MHz", ok0("uhf rx", UHF_RX), str(at["uhf rx"][0]))
    C(res, "digital channel: chip 1 switch 1 on, 2 off", aud("uhf dmr rx") == (1, 0), str(aud("uhf dmr rx")))
    C(res, "back on VHF", ok0("vhf rx 3", VHF_RX), str(at["vhf rx 3"][0]))
    f = at["fm broadcast"]
    C(res, "FM broadcast: chip 1 pin 5 on, 4 off, audio switches off", (f[1][5], f[1][4], f[1][1], f[1][2]) == (1, 0, 0, 0), str(f[1]))
    C(res, "FM broadcast off: pin 5 off", at["fm off"][1][5] == 0, str(at["fm off"][1]))
    f = at["am"]
    C(res, "AM: chip 1 pins 5, 7, 4 on, audio switches off; chip 0 antenna switches (VHF)", [f[1][i] for i in (5, 7, 4, 1, 2)] == [1, 1, 1, 0, 0]
      and all(f[0][i] == v for i, v in {0: 0, 6: 0, 7: 0, 4: 1}.items()), str(f))
    f = at["am off"]
    C(res, "AM off: pins off, chip 0 back to VHF receive", [f[1][i] for i in (5, 7, 4)] == [0, 0, 0] and ok0("am off", VHF_RX), str(f))
    C(res, "chip 0 outputs 1 / 2 never on", not hi0)
    print("RESULT:", "PASS" if all(res) else "FAIL", "(%d/%d)" % (sum(res), len(res)))
    return 0 if all(res) else 1


if __name__ == "__main__":
    sys.exit(main())
