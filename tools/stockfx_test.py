#!/usr/bin/env python3
"""Stock CPS settings at run time, part 2 (app/stockfx.c, stockopt.c, scan.c, drivers/bt*.c), emulator, either radio,
made-up codeplugs from cfw_cps/examples/synth.d8cp. Runs (each its own codeplug / emulator):

  tone     Squelch Mode: CTCSS 100.0 opens on its tone, not on 103.5; DCS 023 opens on 023, not on 025 / inverted;
           stored "Carrier" ignores the tone; FM discriminator streams made here
  noaa     Menu > Radio extras > NOAA weather: WX2 tuned, 1050 Hz for 4 s -> NOAA ALERT
  tbst     Up key while PTT: TBST tone (1750 Hz) in the transmitted audio
  misc     Ana idle tone, AM/FM Function at power on, FM Monitor pause / resume, clock value (UTC+08:00)
  scan     VFO scan type SE stops on a signal; Auto Scan starts the channel's list
  lock     power-on password: PTT refused, wrong then right password
  bt       Bluetooth On: module commands (probe, gains)
  absent   plain synth codeplug: Menu Exit 5 s, channel colour Orange, Auto Repeater off
  rpt2     Auto Repeater2 range wins; AM offset negative (D890UV); private hot key; Quick Call; Options edit -> journal
  aprs     APRS Auto TX Interval 10 s sends by itself; Analog APRS PTT Mode End
  emerg    Alarm key: FM alarm (2 TX / RX cycles, then over), cancel by key; DMR alarm on the assigned channel and back;
           Friends List row -> private call target
  emgrx    A's DMR alarm (emergency bit, Unicode alias on the D890UV) streamed into B: Receive Alarm + Alias First show
           EMERGENCY and the alias; C (stock defaults: Receive Alarm off, alias display Off) shows neither
  ring     two radios, DMR: hot key sends a Pre-defined SMS + private call -> text received; MsgRing / CallRing beep only when on
  alone    Work Alone channel: warning, response, then emergency
  roam     Exclude channel from roaming refuses; Auto Roaming after 1 minute

  python3 cfw/tools/stockfx_test.py [--radio d890|d168] [--elf ELF] [--only RUN,RUN]"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402
import stockopt_test as SO  # noqa: E402
from stockopt_test import C, ST, EXTRAS, chan, tuned, check  # noqa: E402

RUNS = ["tone", "noaa", "tbst", "misc", "scan", "lock", "bt", "absent", "rpt2", "aprs", "emerg", "emgrx", "ring", "alone", "roam"]
FS = 48000


def build(model, path, fn):
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    ST.use(model)
    opt = lambda k, v: ST.set_stock_value(cp, "OptionalSetting", k, str(v))  # noqa: E731
    opt("MenuLowPower", 11)                                    # 60 s, so the menu stays open (absent = 5 s)
    fn(cp, opt)
    C.save(cp, path)
    return path


def wav(path, hz):
    """discriminator stream: deviation in Hz -> 12-bit ADC counts (0.25 count / Hz, as the emulator model)"""
    np.clip(2048 + 0.25 * np.asarray(hz), 0, 4095).astype("<u2").tofile(path)
    return path


def ctcss(f, sec, dev=600.0):
    t = np.arange(int(sec * FS)) / FS
    return dev * np.sin(2 * np.pi * f * t) + 400 * np.sin(2 * np.pi * 900 * t)


def dcs(code, sec, inv=False, dev=600.0):
    data = 0x800 | int(str(code), 8)
    r = data << 11
    for i in range(22, 10, -1):
        if r >> i & 1:
            r ^= 0xC75 << (i - 11)
    w = r << 12 | data
    n = int(sec * FS)
    bit = (np.arange(n) * 134.4 / FS).astype(int) % 23
    v = np.array([1.0 if w >> b & 1 else -1.0 for b in range(23)])[bit]
    k = np.ones(120) / 120
    return dev * np.convolve(-v if inv else v, k, "same")


def ui(radio, elf, cpf, name, steps, **kw):
    return SO.ui(radio, elf, cpf, name, steps, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--only", default=",".join(RUNS))
    a = ap.parse_args()
    os.environ["EMU_NO_ANYTONE"] = "1"
    R = U.RADIOS[a.radio]
    elf, only, d890 = a.elf or R["elf"], a.only.split(","), a.radio == "d890"
    sym = {}
    for ln in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = ln.split()
        if len(p) == 3:
            sym.setdefault(p[2], int(p[0], 16))
    tmp = tempfile.mkdtemp(prefix="stockfx_")
    res = []
    dw = lambda s, i=0: "0|sysbus ReadDoubleWord 0x%08X" % (sym[s] + 4 * i)  # noqa: E731
    by = lambda s: "0|sysbus ReadByte 0x%08X" % sym[s]                        # noqa: E731
    nums = lambda v, k: [int(x.strip(), 16) for c, x in v if k in c]          # noqa: E731
    n = lambda s: "sfx_%s_%s" % (s, a.radio)                                  # noqa: E731
    cpp = lambda s: os.path.join(tmp, s + ".d8cp")                            # noqa: E731
    out = os.path.join(R["emu"], "out")

    if "tone" in only:
        def f(cp, o):
            cp.channels[1].rx_tone = "100.0"
            cp.channels[2].rx_tone = "D023N"
            cp.channels[3].rx_tone = "100.0"
            cp.channels[3].stock = {"Squelch Mode": "Carrier"}
            o("SQL1", 1)
        build(R["model"], cpp("tone"), f)
        st = {}
        for k, x in (("c100", ctcss(100.0, 4)), ("c103", ctcss(103.5, 4)), ("d023", dcs(23, 4)), ("d025", dcs(25, 4)),
                     ("d023i", dcs(23, 4, inv=True))):
            st[k] = wav(os.path.join(out, "sfx_%s.adc16" % k), x)
        steps = []
        plan = [(2, "c100", 146535000), (2, "c103", 146535000), (3, "d023", 146550000), (3, "d025", 146550000),
                (3, "d023i", 146550000), (4, "c103", 146565000)]
        for ch, k, hz in plan:
            steps += chan(ch) + ["0|rf0 Signal %d -50" % hz, "0.2|analog Stream @%s 48000" % st[k], "wait:2.5",
                                 by("g_dspb_fm_mute"), "0|analog StreamStop", "0|rf0 SignalOff", "wait:0.8"]
        v, t, _ = ui(a.radio, elf, cpp("tone"), n("tone"), steps)
        got = [x & 1 for x in nums(v, "ReadByte")]
        for (ch, k, _), g, want in zip(plan, got + [9] * 6, [0, 1, 0, 1, 1, 0]):
            check(res, "tone squelch ch %d, %s: %s" % (ch, k, "muted" if want else "open"), g == want, "mute=%s" % g)

    if "noaa" in only:
        build(R["model"], cpp("noaa"), lambda cp, o: o("WxAlarmSign", 1))
        t_ = np.arange(5 * FS) / FS
        s = wav(os.path.join(out, "sfx_1050.adc16"), 2500 * np.sin(2 * np.pi * 1050 * t_))
        row = 6 if d890 else 5
        v, t, _ = ui(a.radio, elf, cpp("noaa"), n("noaa"), EXTRAS + ["key:16x%d" % row, "key:13", "key:16", "key:13",
                     "wait:0.5", "0|rf0 Status", "0|analog Stream @%s 48000" % s, "wait:4.5", dw("g_stockfx_dbg", 2),
                     "key:16x6", "key:13", "wait:0.3", "0|rf0 Status"])
        check(res, "NOAA WX2 162.400 tuned, left again", tuned(v)[:1] == [162400000] and tuned(v)[1:2] != [162400000], str(tuned(v)))
        check(res, "NOAA 1050 Hz alert", nums(v, "ReadDouble") == [1] and "NOAA ALERT" in t, str(nums(v, "ReadDouble")))

    if "tbst" in only:
        build(R["model"], cpp("tbst"), lambda cp, o: o("TBST", 2))
        cap = os.path.join(out, n("tbst"), "tx.adc16")
        os.makedirs(os.path.dirname(cap), exist_ok=True)
        v, t, d = ui(a.radio, elf, cpp("tbst"), n("tbst"), chan(2) + ["0|pwm AutoCapture false", "pttdown", "wait:0.4",
                     "0|pwm Capture @%s" % cap, "key:15", "wait:0.6", "0|pwm CaptureStop", "pttup", "wait:0.3",
                     dw("g_stockfx_dbg", 1)])
        import json
        hz = 0
        jp = cap.replace(".adc16", ".json")
        if os.path.exists(jp):
            m = json.load(open(jp))
            x = np.fromfile(os.path.join(os.path.dirname(cap), m["file"]), dtype="<u2").astype(float)
            x = x - x.mean()
            if len(x) > 4096:
                hz = np.argmax(np.abs(np.fft.rfft(x * np.hanning(len(x))))) * (m["rate"] or FS) / len(x)
        check(res, "TBST 1750 Hz", nums(v, "ReadDouble") == [1] and abs(hz - 1750) < 15, "count %s, %.0f Hz" % (nums(v, "ReadDouble"), hz))

    if "misc" in only:
        def f(cp, o):
            o("AnaSqOnVoice", 1); o("FM_En", 1); o("Work_FMCH", 1); o("FmMon", 1); o("Gps", 1); o("TmZone", 25); o("SQL1", 1)
        build(R["model"], cpp("misc"), f)
        st = "0|i2c KtStatus" if d890 else "0|i2c LastFmWrite"
        v, t, _ = ui(a.radio, elf, cpp("misc"), n("misc"), ["wait:1", st] + ["0|rf0 Signal 146520000 -50", "wait:2", st,
                     "0|rf0 SignalOff", "wait:3", st, dw("g_stockfx_dbg", 0), dw("g_stockfx_dbg", 7),
                     dw("g_stockrun_clock", 0), dw("g_stockrun_clock", 1)], gps=True)
        b = [x for c, x in v if ("Kt" in c or "LastFm" in c)]
        on = (lambda x: x.startswith("power on")) if d890 else (lambda x: x.startswith("C0"))
        x = nums(v, "ReadDouble") + [0] * 4
        check(res, "FM radio on at power on (AM/FM Function = FM)", len(b) == 3 and on(b[0]), str(b[:1]))
        check(res, "FM Monitor: busy channel pauses, then resumes", len(b) == 3 and not on(b[1]) and on(b[2]) and x[1] == 1, str(b[1:]))
        check(res, "Ana idle tone", x[0] >= 1, str(x[:1]))
        check(res, "GPS clock = UTC + 8 h", x[2] != 0 and x[3] - x[2] == 28800, "%d" % (x[3] - x[2]))

    if "scan" in only:
        def f(cp, o):
            o("ScanType", 2); o("VfoScanFreq2", "146"); o("VfoScanFreq3", "147"); o("Step", 7); o("SQL1", 1)
            cp.channels[41].stock = {"Auto Scan": "1"}
        build(R["model"], cpp("scan"), f)
        vk = "key:20" if d890 else "pf2"
        steps = [vk] + ["key:%d" % (int(c) + 1) for c in "14600000"] + ["wait:0.3", "0|rf0 Signal 146100000 -50", "key:13",
                 "key:16x6", "key:13", "key:13", "wait:3", "0|rf0 Status", "0|rf0 SignalOff", "key:14", "key:14", vk,
                 "wait:0.5"] + chan(42) + ["wait:2.5", dw("g_stockfx_dbg", 5)]
        v, t, _ = ui(a.radio, elf, cpp("scan"), n("scan"), steps)
        check(res, "VFO scan (SE) stops at 146.100", tuned(v)[:1] == [146100000], str(tuned(v)))
        check(res, "Auto Scan starts", nums(v, "ReadDouble") == [1], str(nums(v, "ReadDouble")))

    if "lock" in only:
        build(R["model"], cpp("lock"), lambda cp, o: (o("Password", 1), o("Password_Char", "1234")))
        v, t, _ = ui(a.radio, elf, cpp("lock"), n("lock"), ["shot", "pttdown", "wait:0.5", "0|rf0 Status", "pttup",
                     "keys:2,2,2,13", "wait:0.3", by("s_lock"), "keys:2,3,4,5,13", "wait:0.5", by("s_lock")])
        check(res, "password screen, PTT refused, wrong 111, right 1234", "PASSWORD" in t and nums(v, "ReadByte") == [1, 2]
              and "TX TEST" not in t, str(nums(v, "ReadByte")))

    if "bt" in only:
        build(R["model"], cpp("bt"), lambda cp, o: (o("BlueToothOn", 1), o("BhtMicGain", 4), o("BhtSpkGain", 1)))
        row = 7 if d890 else 6
        env = "D890_BT_LOG" if d890 else "D168_OUT_DIR"
        v, t, d = ui(a.radio, elf, cpp("bt"), n("bt"), ["wait:6"] + EXTRAS + ["key:16x%d" % row, "key:13", "wait:0.3"])
        log = ""
        for fn in ("bluetooth.log", "bt.log"):
            p = os.path.join(d, fn)
            if os.path.exists(p):
                log += open(p, errors="replace").read()
        txt = log + " ".join(bytes.fromhex(h).decode("latin1") for h in re.findall(r"(?:[0-9A-Fa-f]{2}){8,}", log.replace(" ", "").replace("-", "")) if len(h) % 2 == 0)
        mic, spk = ("MICGAIN=13", "SPKGAIN=16")if d890 else ("MICGAIN=22", "SPKGAIN=16")
        check(res, "Bluetooth: probe + gains sent", all(s in txt for s in ("VER?", mic, spk)), txt[-300:].replace("\n", " | ") if mic not in txt else "")
        if not d890:
            check(res, "Bluetooth page shows the module", "module found" in t and "Version JL_V1.0.7" in t)

    if "absent" in only:
        cpf = os.path.join(U.CPS, "examples", "synth.d8cp")
        v, t, _ = ui(a.radio, elf, cpf, n("absent"), ["key:13", "wait:3", "wait:4", "shot"])
        last = t.strip().split("\n")[-14:]
        check(res, "absent Menu Exit = 5 s", not any("SELECT" in x for x in last))

    if "rpt2" in only:
        def f(cp, o):
            o("AutoRepeater", 2); o("VhfAutoRep2", 2); o("AutoRepMinFreqV2", 146); o("AutoRepMaxFreqV2", 147); o("Step", 7)
            cp.stock["AutoRepeaterOffsetFrequencys"] = [["1", "0.60000"], ["2", "1.00000"]]
            if d890:
                o("AmOffset", 1)
                cp.stock["AMAir"] = [["1", "118.1000", "TOWER"]]
            T = ST.TABLES["HotKey_HotKey"]; cols = T["columns"]; rows = [list(r) for r in T["default_rows"]]
            for k, x in (("Mode", "Call"), ("Call Type", "Digital"), ("Call Object", "Talk 1")):
                rows[1][cols.index(k)] = x
            rows[1][6] = "Private Call"
            for k, x in (("Mode", "Call"), ("Call Type", "Analog"), ("Call Object", "1")):
                rows[2][cols.index(k)] = x
            cp.stock["HotKey_HotKey"] = rows
            cp.stock["HotKey_QuickCall"] = [["1", "DTMF", "1"], ["2", "Off", "Off"], ["3", "Off", "Off"], ["4", "Off", "Off"]]
            cp.stock["DTMFEncode"] = [["1", "1234"]]
        build(R["model"], cpp("rpt2"), f)
        vk = "key:20" if d890 else "pf2"
        steps = [vk] + ["key:%d" % (int(c) + 1) for c in "14694000"] + ["wait:0.4", "pttdown", "wait:0.5", "0|rf0 Status",
                 "pttup", "wait:0.5", vk, "wait:0.3"] + chan(2) + ["long:4", "wait:2", dw("g_sig_dbg", 0)] + chan(82) + [
                 "long:3", "wait:0.5"]
        if d890:
            steps += EXTRAS + ["key:16", "key:13", "key:13", "key:16", "key:13", "wait:0.5", "0|rf1 Status", "key:14", "key:14",
                               "key:14", "key:14", "wait:0.3"]
        opt_row = 4 if d890 else 3
        steps += ["key:14", "key:14"] + EXTRAS + ["key:16x%d" % opt_row, "key:13", "key:13", "key:16", "key:13", "key:14", "wait:1", "shot"]
        v, t, _ = ui(a.radio, elf, cpp("rpt2"), n("rpt2"), steps)
        tu = tuned(v)
        check(res, "Auto Repeater2: 146.940 TX 145.940 (entry 2, negative)", tu[:1] == [145940000], str(tu[:1]))
        check(res, "hot key Quick Call starts a DTMF burst", nums(v, "ReadDouble")[:1] == [1], str(nums(v, "ReadDouble")))
        check(res, "hot key private call", "PRIVATE Talk 1" in t)
        if d890:
            check(res, "AM Offset negative: RF chip 1 at 118.100 - 2.925", tu[1:2] == [115175000], str(tu[1:2]))
        check(res, "Options edit saved to the codeplug", "SAVED" in t and "to the codeplug" in t)

    if "aprs" in only:
        def f(cp, o):
            ac = ST.TABLES["APRS"]["columns"]
            for i, x in ((1, 10), (3, 1), (4, 33), (5, 45), (8, 118), (9, 15), (11, 1)):
                ST.set_stock_value(cp, "APRS", ac[i], str(x))
            cp.channels[2].stock = {"APRS Report Type": "Analog", "Analog APRS PTT Mode": "End Of Transmission"}
        build(R["model"], cpp("aprs"), f)
        v, t, _ = ui(a.radio, elf, cpp("aprs"), n("aprs"), chan(2) + ["wait:13", dw("g_stockrun_dbg", 3)] + chan(3) + [
                     "pttdown", "wait:0.5", "pttup", "wait:7", dw("g_stockfx_dbg", 6)])
        x = nums(v, "ReadDouble") + [0, 0]
        check(res, "APRS Auto TX Interval 10 s", x[0] >= 1, str(x))
        check(res, "Analog APRS PTT Mode End: one beacon after PTT", x[1] == 1, str(x))

    if "emerg" in only:
        hz = []
        def f(cp, o):
            al = lambda k, v: ST.set_stock_value(cp, "AlarmSetting", k, str(v))  # noqa: E731
            for k, v in (("AlarmAEmergency", 3), ("AlarmATime", 4), ("AlarmATxTime", 2), ("AlarmARxTime", 2),
                         ("AlarmAEniSend", 1), ("AlarmACycle", 2), ("AlarmDEmergency", 2), ("AlarmDTxTime", 4),
                         ("AlarmDRxTime", 2), ("AlarmDChannel", 83), ("AlarmDId", 1234567), ("AlarmDCallType", 0)):
                al(k, v)
            cp.settings.keys = ["alarm"] + list(cp.settings.keys)[1:]
            cp.stock["FriendsList"] = [["1", "1234567"], ["2", "7654321"]]
            hz.extend([cp.channels[81].rx_hz, cp.channels[82].tx_hz])
        build(R["model"], cpp("emerg"), f)
        p1 = "key:19" if d890 else "pf1"
        d = [dw("g_emerg_dbg", i) for i in (1, 2, 3, 4, 7)]
        steps = chan(2) + [p1, "wait:1", "wait:11"] + d + [p1, "wait:1", "key:14", "wait:0.3"] + d
        steps += chan(82) + [p1, "wait:2", "0|rf0 Status", "key:14", "wait:1.5", "0|rf0 Status"] + d
        steps += EXTRAS + ["key:15x2", "key:13", "key:16", "key:13", "wait:0.3", "shot"]
        v, t, _ = ui(a.radio, elf, cpp("emerg"), n("emerg"), steps)
        x = nums(v, "ReadDouble") + [0] * 15
        check(res, "FM alarm: 2 TX cycles, ended by itself", x[:5] == [1, 2, 2, 1, 0], str(x[:5]))
        check(res, "FM alarm cancelled by a key", x[5:10][0] == 2 and x[9] == 1, str(x[5:10]))
        check(res, "DMR alarm keyed, cancelled", x[10] == 3 and x[11] == x[6] + 1 and x[14] == 2, str(x[10:15]))
        tu = tuned(v)
        check(res, "DMR alarm on the assigned channel, back after", tu[:2] == [hz[1], hz[0]], "%s want %s" % (tu, hz[::-1]))
        check(res, "Friends List -> private call target", "CALL FRIEND" in t and "private 7654321" in t)

    if "emgrx" in only:
        def mk(rx):
            def f(cp, o):
                al = lambda t_, k, v: ST.set_stock_value(cp, t_, k, str(v))  # noqa: E731
                cp.channels[0] = C.Channel(name="EMG DMR", rx_hz=451000000, tx_hz=451000000, mode="DMR", cc=1, slot=1, tg=99)
                cp.channels[0].stock = {"Send Talker Alias DMR/NX" if d890 else "Send Talker Aias": "1"}
                cp.settings.start_channel = 0
                cp.settings.radio_name = "CFWALIAS"
                cp.settings.dmr_id = 9990201 if rx is None else 9990202
                cp.settings.keys = ["alarm"] + list(cp.settings.keys)[1:]
                if rx is None:
                    for k, v in (("AlarmDEmergency", 2), ("AlarmDTxTime", 6), ("AlarmDRxTime", 9), ("AlarmDEniSend", 1),
                                 ("AlarmDCycle", 1), ("AlarmDId", 99), ("AlarmDCallType", 1)):
                        al("AlarmSetting", k, v)
                    if d890:
                        al("TalkAlias", "AliasFormat", 2)
                elif rx:
                    al("AlarmSetting", "AlarmDReceive", 1)
                    al("TalkAlias", "AliasPrio", 2)
            return f
        for k, x in (("eA", None), ("eB", True), ("eC", False)):
            build(R["model"], cpp(k), mk(x))
        cap = os.path.join(out, n("emgrxA"), "tx.adc16")
        os.makedirs(os.path.dirname(cap), exist_ok=True)
        if os.path.exists(cap):
            os.remove(cap)
        ui(a.radio, elf, cpp("eA"), n("emgrxA"), ["keys:14,14", "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap,
           "key:19" if d890 else "pf1", "wait:8", "0|pwm CaptureStop"])
        x = np.fromfile(cap, dtype="<u2").astype(float) if os.path.exists(cap) else np.zeros(0)
        check(res, "A: emergency call captured", len(x) > 3 * FS, "%.1f s" % (len(x) / FS))
        if len(x) > FS:
            pad = np.full(int(0.3 * FS), 2048.0)
            s_in = os.path.join(out, "sfx_emg_%s.adc16" % a.radio)
            np.clip(np.round(np.concatenate([pad, x, pad])), 0, 4095).astype("<u2").tofile(s_in)
            dur = (len(x) + 2 * len(pad)) / FS
            got = {}
            for k in ("eB", "eC"):
                v, t, _ = ui(a.radio, elf, cpp(k), n("emgrx" + k[1]), ["keys:14,14", "0|rf0 SignalHere -70",
                             "0|analog Stream @%s 48000" % s_in] + ["%.2f|shot" % (dur / 4)] * 4 + [dw("g_emerg_dbg", 6)])
                got[k] = (nums(v, "ReadDouble")[:1], "EMERGENCY" in t and "from 9990201" in t, "TA CFWALIAS" in t, "9990201" in t)
            check(res, "B: EMERGENCY from 9990201 shown (once)", got["eB"][0] == [1] and got["eB"][1], str(got["eB"]))
            check(res, "B: talker alias shown (Alias First)", got["eB"][2], str(got["eB"]))
            check(res, "C: call heard, no emergency note, no alias (stock defaults)",
                  got["eC"][0] == [0] and not got["eC"][1] and not got["eC"][2] and got["eC"][3], str(got["eC"]))

    if "ring" in only:
        def mk(tx, on):
            def f(cp, o):
                cp.channels[0] = C.Channel(name="RING DMR", rx_hz=451000000, tx_hz=451000000, mode="DMR", cc=1, slot=1, tg=99)
                cp.settings.start_channel = 0
                cp.settings.dmr_id = 9990201 if tx else 9990202
                cp.contacts[4].name, cp.contacts[4].id, cp.contacts[4].kind = "UNIT B", 9990202, "private"
                T = ST.TABLES["HotKey_HotKey"]
                rows = [list(r) for r in T["default_rows"]]
                for k, v_ in (("Mode", "Call"), ("Call Type", "Digital"), ("Call Object", "UNIT B"),
                              ("Digi Call Type", "Private Call"), ("Content", "On my way")):
                    rows[1][T["columns"].index(k)] = v_
                cp.stock["HotKey_HotKey"] = rows
                if on:
                    o("MsgRing", 1); o("CallRing", 1)
            return f
        for k, x in (("rA", (True, False)), ("rB", (False, True)), ("rC", (False, False))):
            build(R["model"], cpp(k), mk(*x))
        cap = os.path.join(out, n("ringA"), "tx.adc16")
        os.makedirs(os.path.dirname(cap), exist_ok=True)
        if os.path.exists(cap):
            os.remove(cap)
        v, t, _ = ui(a.radio, elf, cpp("rA"), n("ringA"), ["keys:14,14", "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap,
                     "long:3", "wait:4", "pttdown", "wait:2.5", "pttup", "wait:1", "0|pwm CaptureStop"])
        check(res, "A: hot key = private SMS from the Pre-defined list", "PRIVATE UNIT B" in t and "SMS sent" in t)
        x = np.fromfile(cap, dtype="<u2").astype(float) if os.path.exists(cap) else np.zeros(0)
        if len(x) > FS:
            pad = np.full(int(0.3 * FS), 2048.0)
            s_in = os.path.join(out, "sfx_ring_%s.adc16" % a.radio)
            np.clip(np.round(np.concatenate([pad, x, pad])), 0, 4095).astype("<u2").tofile(s_in)
            dur = (len(x) + 2 * len(pad)) / FS
            got = {}
            for k in ("rB", "rC"):
                v, t, _ = ui(a.radio, elf, cpp(k), n("ring" + k[1]), ["keys:14,14", "0|rf0 SignalHere -70",
                             "0|analog Stream @%s 48000" % s_in] + ["%.2f|shot" % (dur / 4)] * 4 +
                             ["wait:1.5", dw("g_stockrun_dbg", 0), "key:13", "key:7", "key:3", "key:13", "wait:0.5"])
                got[k] = (nums(v, "ReadDouble")[:1], "On my way" in t, "9990201" in t)
            check(res, "B: message text received", got["rB"][1] and got["rB"][2], str(got["rB"]))
            check(res, "B: SMS ring + call ring", got["rB"][0] == [2], str(got["rB"]))
            check(res, "C: same traffic, no ring (stock defaults)", got["rC"][0] == [0] and got["rC"][1], str(got["rC"]))
        else:
            check(res, "A: traffic captured", False)

    if "alone" in only:
        def f(cp, o):
            ST.set_stock_value(cp, "AlarmSetting", "AlarmWWarning", "5")
            cp.channels[1].stock = {"Work Alone": "On"}
        build(R["model"], cpp("alone"), f)
        d = [dw("g_emerg_dbg", i) for i in (5, 1)]
        v, t, _ = ui(a.radio, elf, cpp("alone"), n("alone"), chan(2) + ["wait:50", "wait:13"] + d + ["key:14", "wait:8"] + d +
                     ["wait:58"] + d)
        x = nums(v, "ReadDouble") + [0] * 6
        check(res, "Work Alone: warning after 1 min, key = response", x[:4] == [1, 0, 1, 0], str(x))
        check(res, "Work Alone: no response -> emergency", x[4:6] == [2, 1] and "EMERGENCY" in t, str(x))

    if "roam" in only:
        def f(cp, o):
            o("TimeRoamOn", 1); o("WanderPerod", 0)
            cp.channels[82].stock = {"Exclude channel from roaming": "1"}
        build(R["model"], cpp("roam"), f)
        row = "key:16x2" if d890 else "key:16"
        v, t, _ = ui(a.radio, elf, cpp("roam"), n("roam"), chan(83) + EXTRAS + [row, "key:13", "key:16", "key:13", "wait:0.5",
                     "key:14", "key:14", "key:14", "wait:0.3"] + chan(82) + ["0|rf0 Signal 446075000 -70", "wait:63",
                     "0|rf0 Status", dw("g_stockrun_dbg", 4)])
        check(res, "Exclude channel from roaming refuses", "NO ROAMING" in t)
        check(res, "Auto Roaming after 1 minute", tuned(v)[-1:] == [446075000] and nums(v, "ReadDouble") == [1], "%s %s" % (tuned(v), nums(v, "ReadDouble")))

    ok = all(res) and len(res) > 0
    print("RESULT:", "PASS" if ok else "FAIL", "(%d/%d)" % (sum(res), len(res)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
