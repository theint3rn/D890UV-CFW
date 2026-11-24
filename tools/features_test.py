#!/usr/bin/env python3
"""Emulator test of the settings that act on the radio (app/spk.c) + the encryption gate UI, either radio.

  python3 cfw/tools/features_test.py [--radio d890|d168] [--only beep,volume,vox,psave,enc]

On the FM built-in channel 2M CALL (146.520, no codeplug), settings poked into rset's value table:
  beep    Key beep on: a key press plays ~1 kHz on the MCU audio PWM (captured); Key beep off: no beep
  volume  1 kHz FM tone streamed into the receiver; decoded audio level with Volume limit 8 vs 2 (expect ~1/4)
  vox     VOX 1, the radio's detector reports voice (D890UV UART4 module status, D168UV PD3 low) -> TX keyed;
          no voice -> released after 1.5 s (hardware-path details: hwpaths_test.py)
  psave   Power save 1:1, squelch 1, idle -> receiver switched off/on (rf0); a carrier keeps it on
  enc     (cfw codeplug, ui_test) Settings > Encryption: warning screen, Menu = on; saved in the codeplug journal
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))
RS = dict(SQL=0, TOT=1, VOX=2, BEEP=3, MAXVOL=4, PSAVE=7)


def reads(path):
    vals = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        l = b.strip().splitlines()
        if l and ("ReadDoubleWord" in l[0] or "ReadByte" in l[0]) and len(l) > 1:
            vals.append(int(l[1].strip(), 16))
    return vals


def tone_file(path, hz=1000.0, secs=2.0, dev=2500.0):
    t = np.arange(int(secs * 48000)) / 48000.0
    np.clip(2048 + 0.25 * dev * np.sin(2 * np.pi * hz * t), 0, 4095).astype("<u2").tofile(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--only", default="beep,volume,vox,psave,enc")
    a = ap.parse_args()
    only = a.only.split(",")
    emu = os.path.join(SRC, "emu") if a.radio == "d890" else os.path.join(D168, "emu")
    sys.path.insert(0, emu)
    import txrx_loop as T
    T.ELF = T.ELF.replace("/build/", "/build_emu/")      # EMU build: MCU-side volume gain (measurable decoded PCM)
    first = T.first_builtin() if a.radio == "d890" else 1
    voice_on = ('bt Send "03 00 00 00 01 00 00 00 00"' if a.radio == "d890" else 'board Set "D3" false')
    voice_off = ('bt Send "03 00 00 00 00 00 00 00 00"' if a.radio == "d890" else 'board Set "D3" true')
    S = {k: v[0] for k, v in T.nm(T.ELF).items()}
    sv, dbg = S["s_val"], S["g_spk_dbg"]
    res = {}
    name = "feat_%s" % a.radio
    d = os.path.join(T.OUT, name)
    os.makedirs(d, exist_ok=True)

    def setv(k, v):
        return "0|sysbus WriteByte 0x%08X %d" % (sv + RS[k], v)

    def rdbg(i):
        return "0|sysbus ReadDoubleWord 0x%08X" % (dbg + 4 * i)

    steps = T.keys(first) + ["1.0|shot"]
    marks = []
    if "beep" in only:
        cap = os.path.join(d, "beep.adc16")
        steps += [setv("BEEP", 1), "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap, rdbg(0),
                  "0|lcd Press 13 150", "0.2|shot", "0|pwm CaptureStop", "0|lcd Press 14 150", "0.3|version", rdbg(0),
                  setv("BEEP", 0), "0|lcd Press 13 150", "0.3|version", "0|lcd Press 14 150", "0.3|version", rdbg(0)]
        marks.append(("beep", 3))
    if "volume" in only:
        tf = os.path.join(d, "tone1k.adc16")
        tone_file(tf)
        raw = os.path.join(d, "vol_pcm.raw")
        if os.path.exists(raw):
            os.remove(raw)
        steps += [setv("MAXVOL", 8), "0|pcm Attach 0x%08X 0x%08X 8192 @%s" % (S["g_emu_pcm"], S["g_emu_pcm_wr"], raw),
                  "0|analog Stream @%s 48000" % tf, "2.2|version", setv("MAXVOL", 2), "0|analog Stream @%s 48000" % tf,
                  "2.2|version", "0|pcm Detach"]
    if "vox" in only:
        steps += [setv("VOX", 1), "0.3|version", rdbg(1), "0|" + voice_on, "0.6|shot", "0|rf0 Status", rdbg(1),
                  "0|" + voice_off, "0.5|version", "0|rf0 Status", "1.6|version", "0|rf0 Status", rdbg(2),
                  setv("VOX", 0)]
        marks.append(("vox", 3))
    if "psave" in only:
        steps += [setv("SQL", 1), setv("PSAVE", 1), "6.5|version", rdbg(3), "0|rf0 SignalHere -70", "1.0|version",
                  rdbg(3), "3.0|version", rdbg(3), "0|rf0 Status", "0|rf0 SignalOff", setv("PSAVE", 0), setv("SQL", 0)]
        marks.append(("psave", 3))
    T.run(name, steps, seconds=6.0)
    out = os.path.join(d, "then_out.txt")
    v = reads(out)
    txt = open(out).read()
    k = 0
    if "beep" in only:
        b0, b1, b2 = v[k:k + 3]; k += 3
        x = np.fromfile(cap, dtype="<u2").astype(float) if os.path.exists(cap) else np.zeros(0)
        f, snr, _ = T.tone_measure(x[:48000], 48000.0) if len(x) > 1000 else (None, None, 0)
        res["beep"] = b1 == b0 + 2 and b2 == b1 and f is not None and abs(f - 1000) < 30   # Menu + Exit, beep on
        print("beep: played %d for 2 keys (on) then %d more (off); PWM tone %s Hz (%.2f s captured)" %
              (b1 - b0, b2 - b1, "%.0f" % f if f else "-", len(x) / 48000.0))
    if "volume" in only:
        pcm = np.fromfile(raw, dtype="<i2").astype(float) if os.path.exists(raw) else np.zeros(0)
        n = len(pcm)
        if n > 30000:
            seg = 16000
            r1 = np.sqrt(np.mean(pcm[int(0.2 * 8000):int(0.2 * 8000) + seg // 2] ** 2))
            r2 = np.sqrt(np.mean(pcm[n - seg // 2 - 1600:n - 1600] ** 2))
            ratio = r2 / r1 if r1 else 0
            res["volume"] = 0.18 < ratio < 0.32
            print("volume: decoded rms %.0f (limit 8) -> %.0f (limit 2), ratio %.2f (expect 0.25)" % (r1, r2, ratio))
        else:
            res["volume"] = False
            print("volume: no PCM captured (%d samples)" % n)
    if "vox" in only:
        st = re.findall(r"rf0 Status\s*\n(.*)", txt)
        v0, v1, rel = v[k:k + 3]; k += 3
        tx_on = len(st) > 0 and "TX" in st[0].upper() and "TX FALSE" not in st[0].upper()
        res["vox"] = v1 == v0 + 1 and rel >= 1
        print("vox: key-ups %d, releases %d; rf0 while loud: %s" % (v1 - v0, rel, st[0].strip() if st else "?"))
    if "psave" in only:
        p0, p1, p2 = v[k:k + 3]; k += 3
        res["psave"] = p0 > 3 and p2 - p1 <= 1
        print("psave: %d receiver-off windows in 6.5 s idle; with a carrier: %d more in 3 s" % (p0, p2 - p1))
    if "enc" in only:
        r = subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), "enc_%s" % a.radio, "--radio", a.radio, "--elf", T.ELF,
                            "keys:14,14,13,10", "key:15", "key:15", "key:13", "key:13", "0.8|shot", "key:14",
                            "1.5|shot", "0|sysbus ReadByte 0x%08X" % S["s_rs_enc"]], capture_output=True, text=True)
        eo = os.path.join(emu, "out", "enc_%s" % a.radio)
        ev = reads(os.path.join(eo, "then_out.txt"))
        tl = open(os.path.join(eo, "text.txt")).read() if os.path.exists(os.path.join(eo, "text.txt")) else ""
        res["enc"] = bool(ev) and ev[-1] == 1 and "ILLEGAL on amateur" in tl and "SAVED" in tl
        print("enc: warning shown %s, enabled %s, saved %s (montage %s)" % ("ILLEGAL on amateur" in tl,
              bool(ev) and ev[-1] == 1, "SAVED" in tl, os.path.join(eo, "montage.png")))
    ok = all(res.values())
    for kk, vv in res.items():
        print("%s %s" % ("PASS" if vv else "FAIL", kk))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
