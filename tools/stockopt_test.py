#!/usr/bin/env python3
"""Stock CPS common sections in the cfw (app/stockopt.c, app/stockrun.c, app/ui_stock.c, drivers/bcast.c), emulator,
either radio. A test codeplug = cfw_cps/examples/synth.d8cp + made-up stock values for every section:

  run boot   Power-on Interface Character + text, Startup Sound, Default Startup Channel (zone 0 member 2), GPS on +
             Time Zone (clock set from GPS), GPS Roaming entry around the emulator's GPS position -> zone Site Bravo,
             Menu Exit Time 10 s
  run chan   channel columns: PTT Prohibit (ch 2), Reverse (ch 3), Correct Frequency +500 Hz (ch 4), Busy Lock (ch 1,
             carrier on), hot keys: long 2 = digital call to contact "Talk 1", long 3 = menu Call Log
  run vfo    Step 25K, Auto Repeater A positive with the offset list (VFO 146.940 -> TX 147.540), Options page
  run roam   Roaming Zone "Roam All": carrier on Bravo Roam (446.075) -> Menu > Radio extras > Roaming > Roam now
  run bcast  FM radio memories -> receiver IC tuned / off; D890UV: AM Zone -> AM memory -> IC in AM at the 2.925 MHz IF,
             RF chip 1 at air band + 2.925 MHz
  run aprs   APRS TOCALL / call-SSID / path / symbol / comment / fixed position / TX frequency: beacon decoded
  run tot    TOT 30 s + TOT Predict: pre-alert after 20 s of TX

  python3 cfw/tools/stockopt_test.py [--radio d890|d168] [--elf ELF] [--only RUN,RUN]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402
import cal_crc_test as CC  # noqa: E402

sys.path.insert(0, U.CPS)
from cps import codeplug as C, stock as ST  # noqa: E402

RUNS = ["boot", "chan", "vfo", "roam", "bcast", "aprs", "tot"]


def make_codeplug(model, path):
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    ST.use(model)
    o = lambda k, v: ST.set_stock_value(cp, "OptionalSetting", k, str(v))  # noqa: E731
    o("StartDspSet", 1); o("Start_Char", "CFW STOCK"); o("Start_Char2", "TEST LINE 2"); o("Boot_Sound", 1)
    o("StartChUse", 1); o("StartZone1", 0); o("StartCurChan1", 2)
    o("Gps", 1); o("TmZone", 25)                                 # UTC+08:00
    o("MenuLowPower", 1)                                         # 10 s
    o("Step", 7)                                                 # 25K (V1.05 table: 2.5K first)
    o("AutoRepeater", 1); o("VhfAutoRep", 1)
    o("TOT", 1); o("TotPreEn", 1)                                # TOT 30 s
    cp.stock["AutoRepeaterOffsetFrequencys"] = [["1", "0.60000"], ["2", "5.00000"]]
    # hot keys: 2 = digital group call to "Talk 1", 3 = menu Call Log (others: stock default rows)
    T = ST.TABLES["HotKey_HotKey"]
    cols = T["columns"]
    rows = [list(r) for r in T["default_rows"]]
    r = rows[1]
    for k, v in (("Mode", "Call"), ("Call Type", "Digital"), ("Call Object", "Talk 1"), ("Digi Call Type", "Group Call"),
                 ("Content", "Off")):
        r[cols.index(k)] = v
    rows[2][cols.index("Menu")] = "Call Log"
    cp.stock["HotKey_HotKey"] = rows
    # GPS roaming: emulator GPS 32.7157 N 117.1611 W, 2 km -> zone 3 (Site Bravo)
    G = ST.TABLES["GPSRoaming"]
    g = [list(x) for x in G["default_rows"]]
    gc = G["columns"]
    for k, v in (("OnOff", "1"), ("Zone", "3"), ("Latitude Degree", "32"), ("North or South", "0"),
                 ("Longtitude Degree", "117"), ("East or West", "1"), ("Latitude Minute", "42.94"),
                 ("Longtitude Minute", "09.67"), ("Radius(Meter)", "2000")):
        g[0][gc.index(k)] = v
    cp.stock["GPSRoaming"] = g
    # broadcast memories
    if "Name" in ST.TABLES["FM"]["columns"]:
        cp.stock["FM"] = [["1", "88.100", "Add", "CFW FM ONE"], ["2", "101.500", "Add", "CFW FM TWO"], ["101", "98.000", "Del", "VFO"]]
    else:
        cp.stock["FM"] = [["1", "88.100", "Add"], ["2", "101.500", "Add"], ["101", "98.000", "Del"]]
    if "AMAir" in ST.TABLES:
        cp.stock["AMAir"] = [["1", "118.1000", "TOWER"], ["2", "121.5000", "GUARD"], ["257", "119.0000", "VFO"]]
        cp.stock["AMZone"] = [["1", "Airport", "TOWER|GUARD", "TOWER", "TOWER"]]
    # APRS
    a = lambda k, v: ST.set_stock_value(cp, "APRS", k, str(v))  # noqa: E731
    acols = ST.TABLES["APRS"]["columns"]
    def acol(i, v):
        ST.set_stock_value(cp, "APRS", acols[i], str(v))
    acol(3, 1); acol(4, 33); acol(5, 45); acol(6, 0); acol(7, 0); acol(8, 118); acol(9, 15); acol(10, 0); acol(11, 1)
    acol(48, "APCFWT"); acol(49, 0); acol(50, "XX0APR"); acol(51, 7); acol(52, "\\"); acol(53, "k")
    acol(54, "WIDE2-1"); acol(55, "stock aprs"); acol(56, "144.39000")
    # channel columns (stock Channel extras live in the channel object's .stock dict)
    cp.channels[1].stock = dict(cp.channels[1].stock or {}, **{"PTT Prohibit": "On"})
    cp.channels[2].tx_hz = 147150000
    cp.channels[2].stock = dict(cp.channels[2].stock or {}, **{"Reverse": "On"})
    cp.channels[3].stock = dict(cp.channels[3].stock or {}, **{"Correct Frequency[Hz]": "500"})
    C.save(cp, path)
    return cp


def ui(radio, elf, cpf, name, steps, boot=4.0, gps=False):
    """gps False: the emulator's GPS sends nothing (GPS roaming would move the radio to zone Site Bravo)"""
    r = subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", radio, "--elf", elf,
                        "--codeplug", cpf, "--boot", str(boot)] + ([] if gps else ["--extra", "gps Enabled false"]) + steps,
                       capture_output=True, text=True)
    out = os.path.join(U.RADIOS[radio]["emu"], "out", name)
    tp = os.path.join(out, "then_out.txt")
    vals = CC.monitor_values(tp) if os.path.exists(tp) else []
    tx = os.path.join(out, "text.txt")
    text = open(tx, errors="replace").read() if os.path.exists(tx) else ""
    return vals, text, out


def chan(n):
    return ["key:%d" % (int(d) + 1) for d in str(n)] + ["key:12"]


def tuned(v):
    return [int(m.group(1)) for c, x in v if "rf" in c and "Status" in c for m in [re.search(r"tuned (\d+) Hz", x)] if m]


def check(res, name, ok, detail=""):
    res.append(ok)
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail else ""))


EXTRAS = ["key:13", "key:15x5", "key:13"]          # Menu, Up x5 (Radio extras sits 5th from the end), Select


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--only", default=",".join(RUNS))
    a = ap.parse_args()
    os.environ["EMU_NO_ANYTONE"] = "1"
    R = U.RADIOS[a.radio]
    elf = a.elf or R["elf"]
    only = a.only.split(",")
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    dbg = sym["g_stockrun_dbg"]
    tmp = tempfile.mkdtemp(prefix="stockopt_")
    cpf = os.path.join(tmp, "stock.d8cp")
    make_codeplug(R["model"], cpf)
    res = []
    rd = lambda i: "0|sysbus ReadDoubleWord 0x%08X" % (dbg + 4 * i)  # noqa: E731

    if "boot" in only:
        v, t, out = ui(a.radio, elf, cpf, "so_boot_%s" % a.radio, ["0|shot", "wait:3", rd(0), "0|gps Enabled true", "wait:12",
                                                                  rd(2), rd(5), "key:13", "wait:0.5", "wait:11", "shot"], boot=1.0)
        check(res, "power-on text", "CFW STOCK" in t and "TEST LINE 2" in t)
        nums = [int(x, 16) for c, x in v if "ReadDoubleWord" in c]
        check(res, "startup sound", len(nums) > 0 and nums[0] >= 1, "beeps %s" % (nums[:1],))
        check(res, "startup channel (zone FM Simplex, member 3)", "003 Simplex 3" in t)
        check(res, "GPS clock set (time zone)", len(nums) > 1 and nums[1] >= 1)
        check(res, "GPS roaming -> zone Site Bravo", len(nums) > 2 and nums[2] >= 1 and "Site Bra" in t)
        last = t.strip().split("\n")[-14:]
        check(res, "menu exit time (back to main after 10 s)", not any("SELECT" in x or x.startswith("MENU ") for x in last))

    if "chan" in only:
        steps = (chan(2) + ["pttdown", "wait:0.5", "pttup", "wait:0.3"] +
                 chan(3) + ["wait:0.5", "0|rf0 Status"] +
                 chan(4) + ["wait:0.5", "0|rf0 Status"] +
                 chan(1) + ["0|rf0 Signal 146520000 -60", "wait:1", "pttdown", "wait:0.5", "pttup", "0|rf0 SignalOff",
                            "wait:0.5", "wait:3", "pttdown", "0.8|rf0 Transmitting", "pttup", "wait:0.5"] +
                 ["long:3", "wait:0.5", "long:4", "wait:0.5"])
        v, t, _ = ui(a.radio, elf, cpf, "so_chan_%s" % a.radio, steps)
        tu = tuned(v)
        check(res, "PTT Prohibit", "TX PROHIBITED" in t)
        check(res, "Reverse (RX on the TX frequency)", len(tu) > 0 and tu[0] == 147150000, str(tu[:1]))
        check(res, "Correct Frequency +500 Hz", len(tu) > 1 and tu[1] == 146565500, str(tu[1:2]))
        check(res, "Busy Lock", "CHANNEL BUSY" in t)
        check(res, "Busy Lock, squelch Off, no signal: TX works",
              [x.strip() for c, x in v if "Transmitting" in c][-1:] == ["True"])
        check(res, "hot key 2 = call Talk 1", "GROUP Talk 1" in t)
        check(res, "hot key 3 = menu Call Log", "CALL LOG" in t)

    if "vfo" in only:
        vfo_key = "key:20" if a.radio == "d890" else "pf2"
        steps = ([vfo_key, "wait:0.5"] + ["key:%d" % (int(d) + 1) for d in "14694000"] +
                 ["wait:0.5", "pttdown", "wait:0.5", "0|rf0 Status", "pttup", "wait:0.5", "knob:1", "wait:0.5",
                  "0|rf0 Status"] + ["key:14"] + EXTRAS + ["key:15x6", "key:13", "wait:0.5"])
        v, t, _ = ui(a.radio, elf, cpf, "so_vfo_%s" % a.radio, steps)
        tu = tuned(v)
        check(res, "auto repeater: VFO 146.940 TX 147.540", len(tu) > 0 and tu[0] == 147540000, str(tu[:1]))
        check(res, "VFO step 25K", len(tu) > 1 and tu[1] == 146965000, str(tu[1:2]))
        check(res, "Options page shows stock values", "Freq step" in t and "25K" in t)

    if "roam" in only:
        roam_row = "key:16x2" if a.radio == "d890" else "key:16"     # Radio extras: FM radio, (AM air,) Roaming
        steps = (chan(82) + ["wait:0.5", "0|rf0 Signal 446075000 -70"] + EXTRAS + [roam_row, "key:13", "key:16",
                 "key:13", "wait:2.5", "0|rf0 Status", "shot"])
        v, t, _ = ui(a.radio, elf, cpf, "so_roam_%s" % a.radio, steps)
        tu = tuned(v)
        check(res, "roaming: strongest repeater Bravo Roam", "Bravo Roam -70dBm" in t and len(tu) > 0 and tu[-1] == 446075000,
              str(tu[-1:]))

    if "bcast" in only:
        fm_steps = EXTRAS + ["key:13", "key:16", "key:13", "wait:0.5"]
        if a.radio == "d890":
            steps = fm_steps + ["0|i2c KtStatus", "key:14", "wait:0.3", "0|i2c KtStatus", "key:14", "key:16", "key:13",
                                "key:16", "key:13", "key:13", "wait:0.5", "0|i2c KtStatus", "0|rf1 Status", "key:14",
                                "wait:0.3", "0|i2c KtStatus"]
            v, t, _ = ui(a.radio, elf, cpf, "so_bcast_%s" % a.radio, steps)
            kt = [x for c, x in v if "KtStatus" in c]
            tu = tuned(v)
            check(res, "FM radio memory CFW FM ONE -> IC FM 88.1 MHz", len(kt) > 0 and "power on, band FM, FM 88100000" in kt[0]
                  and "CFW FM ONE" in t, kt[0] if kt else "")
            check(res, "FM radio stop -> IC off", len(kt) > 1 and kt[1].startswith("power off"), kt[1] if len(kt) > 1 else "")
            check(res, "AM zone Airport -> TOWER 118.100: IC AM at 2.925 MHz IF", len(kt) > 2 and "band AM" in kt[2]
                  and "AM IF 2925000" in kt[2] and "TOWER" in t, kt[2] if len(kt) > 2 else "")
            check(res, "AM: RF chip 1 at 118.100 + 2.925 MHz", len(tu) > 0 and tu[0] == 121025000, str(tu))
            check(res, "AM stop -> IC off", len(kt) > 3 and kt[3].startswith("power off"), kt[3] if len(kt) > 3 else "")
        else:
            steps = fm_steps + ["0|i2c LastFmWrite", "key:14", "wait:0.3", "0|i2c LastFmWrite"]
            v, t, _ = ui(a.radio, elf, cpf, "so_bcast_%s" % a.radio, steps)
            fw = [x.strip() for c, x in v if "LastFmWrite" in c]
            check(res, "FM radio 88.1 MHz -> receiver regs 02..05 (volume 13)", len(fw) > 0 and fw[0] == "C0-09-3C-9A-04-02-85-AD", str(fw[:1]))
            check(res, "FM radio stop -> receiver off", len(fw) > 1 and fw[1].startswith("80-00"), str(fw[1:2]))
            check(res, "AM air not offered (D168UV)", "AM air" not in t)

    if "aprs" in only:
        import aprs_test as AT
        out = os.path.join(R["emu"], "out", "so_aprs_%s" % a.radio)
        os.makedirs(out, exist_ok=True)
        cap = os.path.join(out, "tx_aprs.adc16")
        steps = ["wait:0.5", "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap, "key:13", "key:15x4", "key:13",
                 "key:13", "wait:0.3", "0|rf0 Status", "wait:1.5", "0|pwm CaptureStop", "0|rf0 Status"]
        v, t, _ = ui(a.radio, elf, cpf, "so_aprs_%s" % a.radio, steps, gps=True)
        tu = tuned(v)
        import json
        import numpy as np
        frames = []
        jp = os.path.join(out, "tx_aprs.json")
        if os.path.exists(jp):
            meta = json.load(open(jp))
            x = np.fromfile(os.path.join(out, meta["file"]), dtype="<u2").astype(float)
            frames = [AT.ax25(f) for f in AT.demod(x, meta["rate"] or 48000.0)]
        want = "XX0APR-7>APCFWT,WIDE2-1:!3345.00N\\11815.00Wkstock aprs"
        check(res, "APRS frame from the stock APRS table", want in frames, "; ".join(frames) or "no frame")
        check(res, "APRS on the stock TX frequency 144.390, back after", len(tu) > 1 and tu[0] == 144390000 and tu[1] != 144390000,
              str(tu))

    if "tot" in only:
        v, t, _ = ui(a.radio, elf, cpf, "so_tot_%s" % a.radio, ["pttdown", "wait:21", rd(1), "pttup", "wait:0.5"])
        nums = [int(x, 16) for c, x in v if "ReadDoubleWord" in c]
        check(res, "TOT predict after 20 s of a 30 s TOT", len(nums) > 0 and nums[0] == 1 and "TOT 10 s" in t, str(nums))

    ok = all(res) and len(res) > 0
    print("RESULT:", "PASS" if ok else "FAIL", "(%d/%d)" % (sum(res), len(res)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
