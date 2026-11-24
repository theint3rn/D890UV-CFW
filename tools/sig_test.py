#!/usr/bin/env python3
"""Analog signalling test (emulator, either radio): DTMF, 2-tone, 5-tone, MDC1200 / QDC1200, PTT ID, alert tone.

  python3 cfw/tools/sig_test.py [--radio d890|d168] [--elf ELF] [--stock-mdc] [--rx-only] [--defaults]

A cfw codeplug (cfw_cps/examples/synth.d8cp converted to the radio, + the stock "Analog" lists below) is preloaded
into internal flash. Channels (codeplug 0..3, FM): 0 DTMF (DTMF ID 1), 1 MDC1200/QDC1200 entry 1 with PTT ID
Start & End, 2 5Tone (5Tone ID 1, ZVEI1), 3 2Tone decode of pair 1.

TX run: the firmware sends each encode list entry (test hook g_dbg_sig_send) and a PTT with PTT ID (+ a DTMF key
pressed while PTT is held), the TX audio PWM (PB9) is captured per send and decoded HERE by an independent decoder
(FFT DTMF / tone tracking / MDC1200 FSK + CRC); then the alert tone 1 on the speaker (PWM capture, 1 kHz pattern).
RX run: generated streams (8 counts per Hz deviation at 48 kHz, as the FM discriminator) + the TX run's captures
(cfw TX -> cfw RX) are played into the ADC on each channel; the firmware's heard list must hold every one, with
names from the address books and "to me" + alert for calls to this radio.
The DTMF / 2Tone / 5Tone Settings and 5Tone information codes (section 10 tables 65-68) are set to non-default values
(SET below); every timing is measured on the captures. --defaults: none set, TX only, stock default timings.
--stock-mdc (D890 only): the cfw's MDC1200 capture is also played into the stock V1.05 firmware, whose decoder must
report the same op / arg / unit (emu/streams/test_stock_mdc.py set-up).
"""
import argparse, json, math, os, re, struct, subprocess, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
CPS = os.path.join(SRC, "cfw_cps")
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))
FS = 48000
RADIOS = {
    "d890": dict(emu=os.path.join(SRC, "emu"), elf=os.path.join(SRC, "cfw", "build_emu_an", "cfw.elf"), base=0x08180000,
                 model="D890UV", ptt="A10", args=[], mdc="MDC1200"),
    "d168": dict(emu=os.path.join(D168, "emu"), elf=os.path.join(D168, "cfw", "build_emu_an", "cfw168.elf"),
                 base=0x080A0000, model="D168UV", ptt="D0", args=["--cfw"], mdc="QDC1200"),
}
OWN, OTHER = 0x1234, 0xBEEF
ZVEI1 = [2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200]


# non-default dialog values (columns of cfw_cps/schema/rdt_dialogs.json); times: DTMF 200 ms, first digit 400 ms, pretime
# 800 ms, after code 300 ms, D pause 1 s; 2-tone 0.4 + gap 0.2 + 0.8 s, long 1.2 s; 5-tone pretime 500 ms
SET = {
    "DTMFSetting": {"DtmfTxTime": "2", "DtmfSelfId": "123", "DtmfInterval": "14", "DtmfGroup": "15", "DtmfDecResp": "1",
                    "DtmfFirstDigit": "40", "DtmfPretime": "80", "DtmfTimeLapse": "30", "DtmfDCodePause": "1",
                    "DtmfBot": "77", "DtmfEot": "88"},
    "T2Setting": {"T2First": "4", "T2Second": "8", "T2Long": "12", "T2Gap": "2"},
    "T5Setting": {"T5SelfId": "12234", "T5DecStd": "0", "T5DecResp": "1", "T5DecTime": "3", "T5DispAny": "0",
                  "T5Pretime": "50", "T5TimeLapse": "30", "T5PttPause": "10", "T5BotId": "135", "T5BotStd": "6",
                  "T5BotTime": "100", "T5EotId": "246", "T5EotStd": "0", "T5EotTime": "70"},
}


# ------------------------------------------------------------------ codeplug
def make_codeplug(R, out, defaults=False):
    sys.path.insert(0, CPS)
    from cps import codeplug as C, stock as ST
    cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
    if cp.model != R["model"]:
        ST.convert_model(cp, R["model"])
    ST.use(R["model"])
    d168 = R["model"] == "D168UV"
    cp.stock["AnalogAddressBook"] = [["1", "12345", "Base DTMF"], ["2", "*0#9", "Gate"]]
    cp.stock["DTMFEncode"] = [["1", "12345"], ["2", "*0#9"]]
    cp.stock["5ToneEncode"] = [["1", "12234", "ZVEI1", "70", "Base 5T"], ["2", "54321", "CCIR1", "100", "Car"]]
    cp.stock["2ToneEncode"] = [["1", "1000", "1500", "QC pair"], ["2", "900", "", "Long"]]
    cp.stock["DTMFEncode"].append(["3", "1D2"])
    if not defaults:
        for tb, vals in SET.items():
            row = ST.default_rows(tb)[0]
            cols = ST.SCHEMA["tables"][tb]["columns"]
            for k, v in vals.items():
                row[cols.index(k)] = v
            cp.stock[tb] = [row]
        cp.stock["T5InfoCodes"] = [["1", "0", "1", "777"]]
    if d168:
        cp.stock["QDC1200EnCodeTable"] = [["1", "Private Call", "1234", "", "PTT ID", "Off", "My ID"],
                                          ["2", "Private Call", "BEEF", "", "Call Alert", "On", "Unit B"]]
        cp.stock["QDC1200AddressBook"] = [["1", "Private Call", "BEEF", "", "Call Alert", "Off", "Unit B"]]
        rec = ST.default_rows("QDC1200DECodeTable")
        rec[0][1] = "1234"
        cp.stock["QDC1200DECodeTable"] = rec
        sig = "QDC1200"
    else:
        cp.stock["MDC1200Encode"] = [["1", "Private Call", "PTT ID", "Off", "", "1234", "My ID"],
                                     ["2", "Private Call", "Call Alert", "On", "", "BEEF", "Unit B"]]
        cp.stock["MDC1200AddressBook"] = [["1", "Private Call", "Call Alert", "Off", "", "BEEF", "Unit B"]]
        sig = "MDC1200"
    chans = [{"Optional Signal": "DTMF", "DTMF ID": "1", "PTT ID": "Start & End"},
             {"Optional Signal": sig, "Rpga_Mdc": "1", "PTT ID": "Start & End"},
             {"Optional Signal": "5Tone", "5Tone ID": "1", "PTT ID": "Start & End"},
             {"Optional Signal": "2Tone", "2Tone ID": "1", "2TONE Decode": "1", "PTT ID": "Off"}]
    for i, st in enumerate(chans):
        cp.channels[i].mode = "FM"
        cp.channels[i].stock = dict(st)
    data = C.encode(cp, for_radio=True)
    p = os.path.join(out, "codeplug.bin")
    open(p, "wb").write(data)
    return p


def nm(elf):
    out = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", elf], text=True).splitlines():
        f = l.split()
        if len(f) == 4:
            out.setdefault(f[3], (int(f[0], 16), int(f[1], 16)))
        elif len(f) == 3:
            out.setdefault(f[2], (int(f[0], 16), 0))
    return out


def run(R, name, elf, cpbin, steps, boot=4.0):
    out = os.path.join(R["emu"], "out", name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith("lcd_") or f == "then_out.txt":
            os.remove(os.path.join(out, f))
    cmd = [sys.executable, os.path.join(R["emu"], "run.py")] + R["args"] + [
        "--elf", elf, "--seconds", str(boot), "--name", name, "--timeout", "2400",
        "--extra", "sysbus LoadBinary @%s 0x%08X" % (cpbin, R["base"])]
    for s in steps:
        cmd += ["--then", s]
    subprocess.run(cmd, cwd=R["emu"], capture_output=True, text=True)
    t = os.path.join(out, "then_out.txt")
    return out, open(t).read() if os.path.exists(t) else ""


# ------------------------------------------------------------------ host decoders (independent of the firmware)
def frames_peak(x, fs, win):
    """per frame: (freq of the strongest bin, its share of the frame energy)"""
    n = int(fs * win)
    w = np.hanning(n)
    out = []
    for i in range(0, len(x) - n, n):
        s = x[i:i + n] - np.mean(x[i:i + n])
        e = np.sum(s * s) + 1e-9
        X = np.abs(np.fft.rfft(s * w, 4 * n)) ** 2
        f = np.fft.rfftfreq(4 * n, 1 / fs)
        out.append((f, X, e))
    return out


def host_dtmf(x, fs=FS):
    rows, cols, keys = [697, 770, 852, 941], [1209, 1336, 1477, 1633], "123A456B789C*0#D"
    n = int(fs * 0.02)
    t = np.arange(n) / fs
    seq, prev, run_ = "", None, 0
    for i in range(0, len(x) - n, n):
        s = x[i:i + n] - np.mean(x[i:i + n])
        e = np.sum(s * s) + 1e-9
        pr = [abs(np.dot(s, np.exp(-2j * np.pi * f * t))) ** 2 * 2 / n for f in rows]
        pc = [abs(np.dot(s, np.exp(-2j * np.pi * f * t))) ** 2 * 2 / n for f in cols]
        r, c = int(np.argmax(pr)), int(np.argmax(pc))
        d = keys[r * 4 + c] if pr[r] + pc[c] > 0.6 * e and min(pr[r], pc[c]) > 0.15 * e else None
        if d and d == prev:
            run_ += 1
            if run_ == 2:
                seq += d
        else:
            run_ = 1
        prev = d
    return seq


def host_env(x, fs=FS, win=0.005):
    """active segments of a capture: list of (start s, length s)"""
    n = int(fs * win)
    segs, on = [], None
    for i in range(0, len(x) - n, n):
        s = x[i:i + n]
        a = np.std(s) > 20
        if a and on is None:
            on = i
        if not a and on is not None:
            segs.append((round(on / fs, 3), round((i - on) / fs, 3)))
            on = None
    if on is not None:
        segs.append((round(on / fs, 3), round((len(x) - on) / fs, 3)))
    return segs


def near(a, b, tol=0.012):
    return len(a) == len(b) and all(abs(p - q) <= tol for p, q in zip(a, b))


def lens(env):
    return [d for _, d in env]


def gaps(env):
    return [round(env[i + 1][0] - env[i][0] - env[i][1], 3) for i in range(len(env) - 1)]


def host_tones(x, fs=FS, win=0.01):
    """list of (freq Hz, duration s) of steady single tones"""
    n = int(fs * win)
    segs, cur, cnt = [], None, 0
    for i in range(0, len(x) - n, n):
        s = x[i:i + n] - np.mean(x[i:i + n])
        if np.sqrt(np.mean(s * s)) < 20:
            f = None
        else:
            X = np.abs(np.fft.rfft(s * np.hanning(n), 16 * n))
            k = int(np.argmax(X[1:])) + 1
            f = k * fs / (16 * n)
        if f and cur and abs(f - cur) < cur * 0.02:
            cnt += 1
        else:
            if cur and cnt >= 3:
                segs.append((round(cur), cnt * win))
            cur, cnt = f, 1
    if cur and cnt >= 3:
        segs.append((round(cur), cnt * win))
    return segs


def mdc_crc(d):
    crc = 0
    for c in d:
        j = 1
        while j <= 0x80:
            bit = crc & 0x8000
            crc = (crc << 1) & 0xFFFF
            if c & j:
                bit ^= 0x8000
            if bit:
                crc ^= 0x1021
            j <<= 1
    r = sum(1 << (15 - i) for i in range(16) if crc & (1 << i))
    return r ^ 0xFFFF


def host_mdc(x, fs=FS):
    """MDC1200: per bit 1200 vs 1800 Hz energy (bit timing searched), differential -> bits, sync, de-interleave, CRC"""
    x = x - np.mean(x)
    spb = fs // 1200
    t = np.arange(spb) / fs
    e12, e18 = np.exp(-2j * np.pi * 1200 * t), np.exp(-2j * np.pi * 1800 * t)
    found = []
    sync = [int(b) for b in "{:040b}".format(0x07092A446F)]
    for off in range(0, spb, 4):
        xb = []
        for i in range(off, len(x) - spb, spb):
            s = x[i:i + spb]
            xb.append(1 if abs(np.dot(s, e18)) > abs(np.dot(s, e12)) else 0)
        bits, b = [], 0
        for v in xb:
            b ^= v
            bits.append(b)
        for i in range(len(bits) - 40 - 112):
            w = bits[i:i + 40]
            err = sum(p != q for p, q in zip(w, sync))
            if err > 2 and err < 38:
                continue
            inv = err >= 38
            lb = [v ^ inv for v in bits[i + 40:i + 152]]
            d = [0] * 14
            k = m = 0
            for a in range(14):
                for j in range(8):
                    d[a] |= lb[k] << j
                    k += 16
                    if k > 111:
                        m += 1
                        k = m
            c = mdc_crc(d[:4])
            if d[4] == c & 0xFF and d[5] == c >> 8:
                pk = (d[0], d[1], d[2] << 8 | d[3])
                if d[0] in (0x35, 0x55):             # double packet: decode the next 112 bits too
                    lb2 = [v ^ inv for v in bits[i + 152:i + 264]]
                    if len(lb2) == 112:
                        d2 = [0] * 14
                        k = m = 0
                        for a2 in range(14):
                            for j in range(8):
                                d2[a2] |= lb2[k] << j
                                k += 16
                                if k > 111:
                                    m += 1
                                    k = m
                        c2 = mdc_crc(d2[:4])
                        if d2[4] == c2 & 0xFF and d2[5] == c2 >> 8:
                            pk = pk + (d2[0], d2[1], d2[2] << 8 | d2[3])
                if pk not in found:
                    found.append(pk)
        if found:
            return found
    return found


def load_cap(path):
    if not os.path.exists(path):
        return None
    return np.fromfile(path, dtype="<u2").astype(float)


# ------------------------------------------------------------------ stream generation (FM discriminator counts)
def to_counts(dev):
    return 2048 + 0.25 * np.asarray(dev)


def g_tone(f1, f2, ms, a=1500):
    t = np.arange(int(FS * ms / 1000)) / FS
    return a * np.sin(2 * np.pi * f1 * t) + (a * np.sin(2 * np.pi * f2 * t) if f2 else 0)


def g_sil(ms):
    return np.zeros(int(FS * ms / 1000))


def g_dtmf(s):
    R, Cc, keys = [697, 770, 852, 941], [1209, 1336, 1477, 1633], "123A456B789C*0#D"
    out = [g_sil(200)]
    for d in s:
        k = keys.index(d)
        out += [g_tone(R[k // 4], Cc[k % 4], 100), g_sil(100)]
    return np.concatenate(out + [g_sil(700)])


def g_5t(s, tab=ZVEI1, ms=70):
    out, prev = [g_sil(200)], None
    for d in s:
        f = 2600 if d == prev else tab[int(d)]
        prev = None if d == prev else d
        out.append(g_tone(f, 0, ms, 2500))
    return np.concatenate(out + [g_sil(500)])


def g_2t(f1, f2):
    return np.concatenate([g_sil(200), g_tone(f1, 0, 1000, 2500), g_tone(f2, 0, 3000, 2500), g_sil(500)])


def g_mdc(op, arg, unit, ext=None):
    """ext = (msg op, msg arg, caller id): double packet (second block right after the first, no new sync)"""
    sys.path.insert(0, os.path.join(SRC, "emu", "streams"))
    import gen_mdc1200 as g
    bits = g.packet_bits(op, arg, unit)
    if ext:
        bits += [(b >> (7 - i)) & 1 for b in g.encode_packet(ext[0], ext[1], ext[2]) for i in range(8)]
    m = np.array(g.modulate(bits, FS, 2500))
    return np.concatenate([g_sil(300), m, g_sil(700)])


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--stock-mdc", action="store_true")
    ap.add_argument("--defaults", action="store_true", help="no setting dialogs in the codeplug, TX timings only")
    ap.add_argument("--rx-only", action="store_true", help="reuse the last TX run's captures")
    a = ap.parse_args()
    R = RADIOS[a.radio]
    elf = os.path.abspath(a.elf) if a.elf else R["elf"]
    S = nm(elf)
    name = "sig_%s" % a.radio
    out = os.path.join(R["emu"], "out", name)
    os.makedirs(out, exist_ok=True)
    cpbin = make_codeplug(R, out, a.defaults)
    results = []

    def check(what, ok, detail=""):
        results.append((what, ok, detail))
        print("%-44s %s  %s" % (what, "PASS" if ok else "FAIL", detail), flush=True)

    # ---------------- TX run
    send = S["g_dbg_sig_send"][0]
    caps = {}
    steps = ["0|pwm AutoCapture false", "0.5|shot"]
    plan = [("dtmf1", 1, 0, 4.2), ("dtmf2", 1, 1, 3.8), ("dtmf3", 1, 2, 4.2), ("5t1", 3, 0, 1.9), ("5t2", 3, 1, 2.1),
            ("2t1", 2, 0, 2.6), ("2t2", 2, 1, 2.2), ("mdc2", 4, 1, 0.9)]
    if a.defaults:
        plan = [("dtmf1", 1, 0, 2.4), ("5t1", 3, 0, 1.5), ("2t1", 2, 0, 2.0), ("2t2", 2, 1, 1.8)]
    for tag, kind, entry, wait in plan:
        caps[tag] = os.path.join(out, "tx_%s.adc16" % tag)
        steps += ["0|pwm Capture @%s" % caps[tag], "0|sysbus WriteWord 0x%08X %d" % (send, (kind << 8 | entry) + 1),
                  "%.2f|pwm CaptureStop" % wait, "0.3|version"]
    if a.defaults:
        run(R, name + "_def", elf, cpbin, steps)
        x = {k: load_cap(v) for k, v in caps.items()}
        e = {k: host_env(v) if v is not None else [] for k, v in x.items()}
        check("default DTMF: 200 ms first digit, then 100 / 100 ms", near(lens(e["dtmf1"]), [.2, .1, .1, .1, .1]) and
              near(gaps(e["dtmf1"]), [.1] * 4), "%s gaps %s" % (lens(e["dtmf1"]), gaps(e["dtmf1"])))
        t2 = host_tones(x["2t1"]) if x["2t1"] is not None else []
        check("default 2Tone: 0.5 s + 0.5 s", len(t2) == 2 and near([d for _, d in t2], [.5, .5], 0.025), str(t2))
        check("default 2Tone entry without 2nd tone: long 1 s", near(lens(e["2t2"]), [1.0]), str(e["2t2"]))
        check("default pretime 200 ms (DTMF, 5Tone)", all(e[k] and 0.18 <= e[k][0][0] <= 0.45 for k in ("dtmf1", "5t1")),
              str([e[k][:1] for k in ("dtmf1", "5t1")]))
        npass = sum(1 for r in results if r[1])
        print("%s defaults: %d/%d PASS" % (a.radio, npass, len(results)))
        return 0 if npass == len(results) else 1
    caps["ptt0"] = os.path.join(out, "tx_ptt0.adc16")     # channel 0: DTMF BOT / EOT
    steps += ["0|pwm Capture @%s" % caps["ptt0"], '0|board Set "%s" false' % R["ptt"], "2.8|shot",
              '0|board Set "%s" true' % R["ptt"], "2.8|shot", "0|pwm CaptureStop"]
    caps["ptt"] = os.path.join(out, "tx_ptt.adc16")       # channel 1: PTT ID start & end + DTMF key 7 while PTT held
    steps += ["0|lcd Press 15 150", "0.6|shot", "0|pwm Capture @%s" % caps["ptt"], '0|board Set "%s" false' % R["ptt"],
              "0.6|shot", "0|lcd Press 8 150", "0.9|shot", '0|board Set "%s" true' % R["ptt"], "0.9|shot",
              "0|pwm CaptureStop", "0|lcd Press 15 150", "0.6|version"]
    caps["ptt2"] = os.path.join(out, "tx_ptt2.adc16")     # channel 2: 5Tone BOT / EOT, second PTT within the pause time
    steps += ["0|pwm Capture @%s" % caps["ptt2"]]
    for _ in range(2):
        steps += ['0|board Set "%s" false' % R["ptt"], "1.6|shot", '0|board Set "%s" true' % R["ptt"], "1.8|version"]
    steps += ["0|pwm CaptureStop", "0|lcd Press 16 150", "0.6|version", "0|lcd Press 16 150", "0.6|version"]
    caps["alert"] = os.path.join(out, "tx_alert.adc16")
    steps += ["0|pwm Capture @%s" % caps["alert"], "0|sysbus WriteByte 0x%08X 1" % S["g_dbg_sig_alert"][0], "5.6|shot",
              "0|pwm CaptureStop", "0|sysbus ReadBytes 0x%08X 32" % S["g_sig_dbg"][0]]
    if not a.rx_only:
        print("TX run (%s) ..." % a.radio, flush=True)
        run(R, name + "_tx", elf, cpbin, steps)
    x = {k: load_cap(v) for k, v in caps.items()}
    got = host_dtmf(x["dtmf1"]) if x["dtmf1"] is not None else ""
    check("TX DTMF entry 1 '12345'", got == "12345", repr(got))
    got = host_dtmf(x["dtmf2"]) if x["dtmf2"] is not None else ""
    check("TX DTMF entry 2 '*0#9'", got == "*0#9", repr(got))
    e = {k: host_env(v) if v is not None else [] for k, v in x.items()}
    check("DTMF times: first 400, then 200 / 200 ms", near(lens(e["dtmf1"]), [.4, .2, .2, .2, .2]) and
          near(gaps(e["dtmf1"]), [.2] * 4), "%s gaps %s" % (lens(e["dtmf1"]), gaps(e["dtmf1"])))
    check("DTMF pretime 800 ms, 5Tone pretime 500 ms", bool(e["dtmf1"]) and 0.78 <= e["dtmf1"][0][0] <= 1.1 and
          bool(e["5t1"]) and 0.48 <= e["5t1"][0][0] <= 0.78, str([e["dtmf1"][:1], e["5t1"][:1]]))
    check("DTMF '1D2': 1 s pause for D", near(lens(e["dtmf3"]), [.4, .2]) and near(gaps(e["dtmf3"]), [1.2]),
          "%s gaps %s" % (lens(e["dtmf3"]), gaps(e["dtmf3"])))
    got = host_dtmf(x["ptt0"]) if x["ptt0"] is not None else ""
    check("TX DTMF PTT ID BOT '77' + EOT '88'", got == "7788", repr(got))
    t5 = host_tones(x["ptt2"]) if x["ptt2"] is not None else []
    want = [1124, 1275, 1446] + [1160, 1400, 1670] * 2          # BOT 135 CCIR, EOT 246 ZVEI1, 2nd PTT: EOT only
    check("TX 5Tone BOT 135 CCIR1 + EOT 246 ZVEI1, pause 10 s", len(t5) == 9 and
          all(abs(f - w) < w * 0.01 for (f, _), w in zip(t5, want)), str(t5))
    t5 = host_tones(x["5t1"]) if x["5t1"] is not None else []
    want = [ZVEI1[1], ZVEI1[2], 2600, ZVEI1[3], ZVEI1[4]]
    ok = len(t5) == 5 and all(abs(f - w) < w * 0.01 and 0.05 <= d <= 0.09 for (f, d), w in zip(t5, want))
    check("TX 5Tone ZVEI1 '12234' (E = repeat)", ok, str(t5))
    ccir = [1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860]
    t5 = host_tones(x["5t2"]) if x["5t2"] is not None else []
    want = [ccir[5], ccir[4], ccir[3], ccir[2], ccir[1]]
    ok = len(t5) == 5 and all(abs(f - w) < w * 0.01 and 0.08 <= d <= 0.12 for (f, d), w in zip(t5, want))
    check("TX 5Tone CCIR1 '54321' 100 ms", ok, str(t5))
    t2 = host_tones(x["2t1"]) if x["2t1"] is not None else []
    ok = len(t2) == 2 and abs(t2[0][0] - 1000) < 10 and abs(t2[1][0] - 1500) < 15
    check("TX 2Tone 1000 Hz 0.4 s, gap 0.2 s, 1500 Hz 0.8 s", ok and near(lens(e["2t1"]), [.4, .8]) and near(gaps(e["2t1"]), [.2]),
          "%s %s" % (t2, e["2t1"]))
    t2 = host_tones(x["2t2"]) if x["2t2"] is not None else []
    check("TX 2Tone entry without 2nd tone: long 1.2 s", len(t2) == 1 and abs(t2[0][0] - 900) < 10 and near(lens(e["2t2"]), [1.2]), str(t2))
    pk = host_mdc(x["mdc2"]) if x["mdc2"] is not None else []
    check("TX %s call alert to BEEF (35 89 BEEF + 80 15 1234)" % R["mdc"], (0x35, 0x89, OTHER, 0x80, 0x15, OWN) in pk,
          str([tuple(map(hex, p)) for p in pk]))
    pk = host_mdc(x["ptt"]) if x["ptt"] is not None else []
    check("TX PTT ID start (01 00 1234) + end (01 80 1234)", (0x01, 0x00, OWN) in pk and (0x01, 0x80, OWN) in pk,
          str([tuple(map(hex, p)) for p in pk]))
    got = host_dtmf(x["ptt"]) if x["ptt"] is not None else ""
    check("TX DTMF key '7' while PTT held", got == "7", repr(got))
    t = host_tones(x["alert"], win=0.02) if x["alert"] is not None else []
    ok = len(t) == 3 and all(abs(f - 1000) < 10 and 0.9 <= d <= 1.1 for f, d in t)
    check("alert tone 1 (1 kHz, 1 s on / 1 s off x3)", ok, str(t))

    # ---------------- RX run
    gap = g_sil(1500)
    cap_or = lambda k: (x[k] - 2048) * 4.0 if x.get(k) is not None else g_sil(10)   # noqa: E731  counts -> dev units
    streams = {
        0: np.concatenate([g_dtmf("12345"), gap, cap_or("dtmf2"), gap, g_dtmf("123*456"), gap, g_dtmf("#23"), gap, g_dtmf("124*456")]),
        1: np.concatenate([g_mdc(0x35, 0x89, OWN, (0x80, 0x15, OTHER)), gap, g_mdc(0x63, 0x85, OWN), gap, g_mdc(0x01, 0x80, OTHER), gap, cap_or("ptt"), gap, cap_or("mdc2")]),
        2: np.concatenate([g_5t("12234"), gap, cap_or("5t2"), gap, g_5t("34567"), gap, g_5t("777"), gap, cap_or("5t1")]),
        3: np.concatenate([g_2t(1000, 1500), gap, gap, cap_or("2t1"), gap]),
    }
    hb, hs = S["s_sig_heard"]
    steps = ["0|rf0 SignalHere -70", "0.3|shot"]
    for ch in range(4):
        if ch:
            steps += ["0|lcd Press 15 150", "0.6|version"]
        p = os.path.join(out, "rx_ch%d.adc16" % ch)
        np.clip(to_counts(streams[ch]), 0, 4095).astype("<u2").tofile(p)
        steps += ["0|analog Stream @%s 48000" % p, "%.2f|shot" % (len(streams[ch]) / FS + 0.6),
                  "0|sysbus ReadBytes 0x%08X %d" % (hb, hs)]     # after each channel (D168UV keeps 8 entries)
    steps += ["0|sysbus ReadBytes 0x%08X 32" % S["g_sig_dbg"][0], "0|sysbus ReadBytes 0x%08X 1" % S["s_note_seq"][0]]
    print("RX run (%s) ..." % a.radio, flush=True)
    d, txt = run(R, name + "_rx", elf, cpbin, steps)
    blocks = re.findall(r"ReadBytes 0x[0-9A-Fa-f]+ \d+\s*\n((?:.*\n)+?)(?=>|\Z)", txt)
    esz = 56                                              # sizeof(sig_heard_t)
    heard = []
    for blk in blocks[:-2]:
      raw = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\b", blk))
      for i in range(hs // esz):
        e = raw[i * esz:(i + 1) * esz]
        if len(e) < 56 or e[0] == 0:
            continue
        kind, to_me, op, arg = e[0], e[1], e[2], e[3]
        unit = e[4] | e[5] << 8
        text = e[6:31].split(b"\0")[0].decode(errors="replace")
        nm_ = e[31:48].split(b"\0")[0].decode(errors="replace")
        target = e[52] | e[53] << 8
        h = (kind, text, nm_, to_me, op, arg, unit, target, e[54], e[55])
        if h not in heard:
            heard.append(h)
    for h in heard:
        print("  heard:", h)
    dbg = struct.unpack("<8I", bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\b", blocks[-2]))[:32]) if len(blocks) > 2 else None
    nseq = int(re.findall(r"0x([0-9A-Fa-f]{2})\b", blocks[-1])[0], 16) if len(blocks) > 2 else -1
    print("  g_sig_dbg:", dbg)

    def hh(kind, text, **kw):
        for h in heard:
            if h[0] == kind and h[1] == text and all({"name": h[2], "to_me": h[3], "op": h[4], "arg": h[5], "target": h[7],
                                                       "msg_op": h[8]}[k] == v for k, v in kw.items()):
                return True
        return False
    check("RX DTMF '12345' (own ID -> call, book name)", hh(1, "12345", name="Base DTMF", to_me=1))
    check("RX DTMF '*0#9' from the cfw TX capture", hh(1, "*0#9", name="Gate"))
    check("RX %s call alert BEEF -> own 1234: call, name" % R["mdc"], hh(4, "BEEF", op=0x35, msg_op=0x80, target=OWN, to_me=1, name="Unit B"))
    check("RX %s radio check to own 1234" % R["mdc"], hh(4, "1234", op=0x63, to_me=1))
    check("RX %s PTT ID BEEF -> book name 'Unit B'" % R["mdc"], hh(4, "BEEF", name="Unit B", op=0x01))
    check("RX %s PTT ID start + end from cfw capture" % R["mdc"], hh(4, "1234", op=0x01, arg=0x00) and hh(4, "1234", op=0x01, arg=0x80))
    check("RX %s call alert 1234 -> BEEF from cfw capture" % R["mdc"], hh(4, "1234", op=0x35, target=OTHER, to_me=0))
    check("RX 5Tone ZVEI1 '12234' -> own ID call", hh(3, "12234", to_me=1))
    check("RX DTMF '123*456' (self ID 123) and '#23' (group #): calls", hh(1, "123*456", to_me=1) and hh(1, "#23", to_me=1))
    check("RX DTMF '124*456': not to me", hh(1, "124*456", to_me=0))
    check("RX 5Tone information code '777' no. 1", any(h[0] == 3 and h[1] == "777" and h[3] and h[9] == 1 for h in heard))
    nq = sum(1 for h in heard if h[0] == 3 and not h[3])
    check("RX 5Tone '34567': heard list only (Disp AnyID off)", hh(3, "34567", to_me=0) and dbg is not None and
          dbg[2] - nseq == nq, "decodes %s popups %s quiet %s" % (dbg[2] if dbg else None, nseq, nq))
    check("RX 2Tone 1000/1500 -> call", hh(2, "2TONE", to_me=1))
    check("alerts played for calls (g_sig_dbg[3] >= 3)", dbg is not None and dbg[3] >= 3, str(dbg[3] if dbg else None))

    if a.stock_mdc and a.radio == "d890" and x.get("mdc2") is not None:
        check("stock V1.05 decodes the cfw MDC1200 capture", stock_mdc(x["mdc2"], out), "")

    shots = sorted(f for f in os.listdir(d) if f.startswith("lcd_") and f.endswith(".png"))
    subprocess.call([sys.executable, os.path.join(R["emu"], "montage.py"), os.path.join(R["emu"], "out", "sig_%s.png" % a.radio)]
                    + ["%d=%s" % (i + 1, os.path.join(d, f)) for i, f in enumerate(shots[-6:])],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    npass = sum(1 for r in results if r[1])
    rep = os.path.join(R["emu"], "out", "sig_%s.json" % a.radio)
    json.dump([{"check": w, "pass": ok, "detail": dd} for w, ok, dd in results], open(rep, "w"), indent=1)
    print("%s: %d/%d PASS (report %s)" % (a.radio, npass, len(results), rep))
    return 0 if npass == len(results) else 1


def stock_mdc(cap, out):
    """the cfw capture -> stock V1.05 MDC decoder (set-up as emu/streams/test_stock_mdc.py)"""
    emu = os.path.join(SRC, "emu")
    p = os.path.join(out, "stock_in.adc16")
    pad = np.full(FS * 2 // 5, 2048.0)
    np.concatenate([pad, cap, pad, cap, pad, cap, pad]).clip(0, 4095).astype("<u2").tofile(p)   # 3x, as the stock test
    then = ["0|knob Turn 1"] + ["1|knob Turn 1"] * 5 + [
        "1|sysbus WriteByte 0x200042e2 0", "0|sysbus WriteByte 0x20013A42 0x40",
        "0|rf0 SignalHere -80", "0|sysbus WriteWord 0x2000d762 0x60", "0|sysbus WriteByte 0x2001af9b 4",
        "0|sysbus WriteDoubleWord 0x40001028 99", "0|sysbus WriteDoubleWord 0x4000102C 249",
        "0|sysbus WriteDoubleWord 0x4000100C 1", "0|sysbus WriteDoubleWord 0x40001000 0x81",
        "0|sysbus WriteDoubleWord 0xE000E104 0x20000", "0|analog Stream @%s 48000" % p,
        "1.0|sysbus ReadByte 0x2000c316", "0|sysbus ReadByte 0x2000c2f9", "0|sysbus ReadWord 0x2000c310"]
    args = [sys.executable, os.path.join(emu, "run.py"), "--orig", "--seconds", "15", "--name", "sig_stock_mdc", "--timeout", "900"]
    for t in then:
        args += ["--then", t]
    subprocess.call(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    res = open(os.path.join(emu, "out", "sig_stock_mdc", "then_out.txt")).read()
    vals = {k.lower(): int(v, 16) for k, v in re.findall(r"> sysbus Read\w+ (0x\w+)\n(0x\w+)", res)}
    got = (vals.get("0x2000c316"), vals.get("0x2000c2f9"), vals.get("0x2000c310"))
    print("  stock decoded op/arg/unit:", got)
    return got in ((0x35, 0x89, OTHER), (0x80, 0x15, OWN))


if __name__ == "__main__":
    sys.exit(main())
