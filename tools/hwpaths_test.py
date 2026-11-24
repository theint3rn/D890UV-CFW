#!/usr/bin/env python3
"""Stock hardware paths of the v0.5 items, on either radio's emulator (codeplug with GPS on, man down on (wait 2 s),
VOX 1, Vol Level 2, key beep on):

  gps     the stock GPS set-up lines go out on USART2 ($CFGSYS + $CFGMSG list of that radio)
  volume  limit applied where stock applies it: D890UV I2C pot 0x2E = table[2][knob step 8] = 0x0D;
          D168UV baseband frames "00 69 1F 40" + "40 08 0C"
  voxcfg  VOX 1 configured: D890UV UART4 frame cmd 0x12 byte 12 = 1; D168UV RF reg 0x12 = 0x2020, reg 0x13 = 0x0031
  vox     detector on (D890UV: module status cmd 3 byte 3 = 1; D168UV: PD3 low) -> TX keyed; off -> released
  mandown `i2c Fall` -> after 1.5 s + 2 s "Alarming / MAN DOWN" popup + alarm; `i2c Upright` -> cancelled
  route   D890UV: FM receive = amplifier PC12 on, PC7 low, RF reg 0x52 switch 2 on; knob at 0 -> amplifier off and a
          key press plays no beep (stock FUN_080c32e8 / FUN_080252e0). D168UV: skipped (analog knob).

  python3 cfw/tools/hwpaths_test.py [--radio d890|d168]"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402
sys.path.insert(0, U.CPS)
from cps import codeplug as C, stock as ST  # noqa: E402


def blocks(path):
    """then_out.txt -> list of (command, output)"""
    res = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        l = b.strip().splitlines()
        res.append((l[0].strip() if l else "", "\n".join(l[1:]).strip()))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    a = ap.parse_args()
    R = U.RADIOS[a.radio]
    d890 = a.radio == "d890"
    name = "hwpaths_%s" % a.radio
    out = os.path.join(R["emu"], "out", name)
    os.makedirs(out, exist_ok=True)
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != R["model"]:
        ST.convert_model(cp, R["model"])
    for k, v in (("Gps", 1), ("FailAlarm", 1), ("ManDownWait", 2), ("VOX", 1), ("MaxVol", 2), ("Beep", 1)):
        ST.set_stock_value(cp, "OptionalSetting", k, v)
    cp.settings.start_channel = 0
    cpf = os.path.join(out, "hwpaths.d8cp")
    C.save(cp, cpf)
    nm = subprocess.check_output(["arm-none-eabi-nm", R["elf"]], text=True)
    S = {l.split()[-1]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    md, spk = S["g_mandown_dbg"], S["g_spk_dbg"]
    on = ('0|bt Send "03 00 00 00 01 00 00 00 00"' if d890 else '0|board Set "D3" false')
    off = ('0|bt Send "03 00 00 00 00 00 00 00 00"' if d890 else '0|board Set "D3" true')
    steps = ["1.5|shot"]
    steps += ["0|i2c Pot"] if d890 else []
    steps += [] if d890 else ["0|rf0 Peek 0x12", "0|rf0 Peek 0x13"]
    steps += [on, "0.6|rf0 Transmitting", off, "0.5|rf0 Transmitting", "1.6|rf0 Transmitting"]
    steps += ["0|i2c Fall", "4.5|shot", "0|sysbus ReadDoubleWord 0x%08X" % (md + 4), "0|i2c Upright", "0.6|shot",
              "0|sysbus ReadDoubleWord 0x%08X" % md]
    if d890:
        steps += ["0|sysbus ReadDoubleWord 0x42020814", "0|rf0 Peek 0x52",               # GPIOC ODR, reg 0x52
                  "0|analog Set 2 0", "0.3|sysbus ReadDoubleWord 0x42020814",
                  "0|sysbus ReadDoubleWord 0x%08X" % spk, "0|lcd Press 13 150", "0.4|version", "0|lcd Press 14 150",
                  "0.4|sysbus ReadDoubleWord 0x%08X" % spk]
    env = dict(os.environ)
    r = subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", a.radio, "--codeplug", cpf]
                       + steps, capture_output=True, text=True, env=env)
    to = os.path.join(out, "then_out.txt")
    if not os.path.exists(to):
        print(r.stdout[-2000:], r.stderr[-2000:])
        print("RESULT: FAIL (no output)")
        return 1
    B = blocks(to)
    pos = [0]

    def nxt(cmd):
        """output of the next then-command containing cmd"""
        while pos[0] < len(B):
            c, o = B[pos[0]]
            pos[0] += 1
            if cmd in c:
                return o
        return ""

    def num(cmd):
        m = re.findall(r"0x[0-9A-Fa-f]+", nxt(cmd))
        return int(m[-1], 16) if m else -1

    def rd(path):
        p = os.path.join(out, path)
        return open(p, errors="replace").read() if os.path.exists(p) else ""

    res = {}
    # gps
    u2 = rd("usart2.txt")
    want_on = ["0", "4", "5"] if d890 else ["0", "4"]
    msgs = re.findall(r"\$CFGMSG,0,(\d),(\d)", u2)
    res["gps"] = "$CFGSYS,h00000001" in u2 and len(msgs) >= 10 and all((m[1] == "1") == (m[0] in want_on) for m in msgs[:10])
    print("gps: CFGSYS %s, CFGMSG %s" % ("$CFGSYS,h00000001" in u2, " ".join("%s=%s" % m for m in msgs[:10])))
    # volume + vox config
    if d890:
        pot = num("i2c Pot")
        res["volume"] = pot == 0x0D
        f12 = re.findall(r"MCU> A5 5A 12 0E((?: [0-9A-F]{2}){14}) 06", rd("bluetooth.log"))
        res["voxcfg"] = any(x.split()[0] == "01" and x.split()[12] == "01" for x in f12)
        print("volume: pot 0x%02X (expect 0x0D = table[2][8])" % (pot & 0xFF))
        print("voxcfg: UART4 cmd 0x12 frames %s" % sorted(set(x.strip() for x in f12)))
    else:
        bb = rd("baseband.log")
        res["volume"] = "MCU> 84 A9 61 00 03 00 69 1F 40" in bb and "MCU> 84 A9 61 00 02 40 08 0C" in bb
        r12, r13 = num("rf0 Peek 0x12"), num("rf0 Peek 0x13")
        res["voxcfg"] = r12 == 0x2020 and r13 == 0x0031
        print("volume: baseband frames 00 69 1F 40 %s, 40 08 0C %s" % ("00 69 1F 40" in bb, "40 08 0C" in bb))
        print("voxcfg: RF reg 0x12 = 0x%04X, 0x13 = 0x%04X (expect 0x2020 / 0x0031)" % (r12, r13))
    # vox
    t1, t2, t3 = nxt("rf0 Transmitting"), nxt("rf0 Transmitting"), nxt("rf0 Transmitting")
    res["vox"] = t1 == "True" and t2 == "True" and t3 == "False"
    print("vox: transmitting with voice %s, 0.5 s after %s, 2.1 s after %s" % (t1, t2, t3))
    # man down
    alarms = num("0x%08X" % (md + 4))
    state = num("0x%08X" % md)
    txt = rd("text.txt")
    res["mandown"] = alarms == 1 and "Alarming" in txt and "MAN DOWN" in txt and "cancelled" in txt and state == 1
    print("mandown: alarms %d, popup %s, cancelled %s, state after upright %d" %
          (alarms, "Alarming" in txt, "cancelled" in txt, state))
    if d890:
        odr, r52 = num("0x42020814"), num("rf0 Peek 0x52")
        odr0 = num("0x42020814")
        b0, b1 = num("0x%08X" % spk), num("0x%08X" % spk)
        res["route"] = (odr >> 12) & 1 == 1 and (odr >> 7) & 1 == 0 and (r52 & 0xF001) == 0x2001 and \
            (odr0 >> 12) & 1 == 0 and b1 == b0
        print("route: FM RX PC12 %d PC7 %d reg52 0x%04X; knob 0 -> PC12 %d, beeps %d" %
              ((odr >> 12) & 1, (odr >> 7) & 1, r52, (odr0 >> 12) & 1, b1 - b0))
    print("montage:", os.path.join(out, "montage.png"))
    for kk, vv in res.items():
        print("%s %s" % ("PASS" if vv else "FAIL", kk))
    ok = all(res.values())
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
