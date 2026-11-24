#!/usr/bin/env python3
"""APRS beacon test (emulator, either radio): FM built-in channel 2M CALL (146.520), Menu > GPS / APRS (GPS fix from
the emulator's NMEA model), Menu = beacon. The TX audio PWM is captured and decoded here: Bell 202 AFSK -> NRZI ->
HDLC (flags, bit stuffing) -> AX.25 UI frame with a valid FCS.

  python3 cfw/tools/aprs_test.py [--radio d890|d168]
"""
import argparse, json, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))


def fcs(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return ~c & 0xFFFF


def demod(x, fs=48000.0):
    """AFSK 1200: mark 1200 / space 2200 tone energy per bit -> NRZI bits -> frames"""
    x = x - np.mean(x)
    spb = int(fs / 1200)
    t = np.arange(spb) / fs
    best = []
    for off in range(0, spb, 4):                 # bit timing search
        bits, prev = [], None
        for i in range(off, len(x) - spb, spb):
            seg = x[i:i + spb]
            m = abs(np.dot(seg, np.exp(-2j * np.pi * 1200 * t)))
            s = abs(np.dot(seg, np.exp(-2j * np.pi * 2200 * t)))
            tone = m > s
            if prev is not None:
                bits.append(1 if tone == prev else 0)
            prev = tone
        frames = hdlc(bits)
        if frames:
            return frames
        best = frames
    return best


def hdlc(bits):
    frames, cur, ones, inside = [], [], 0, False
    for b in bits:
        if b:
            ones += 1
            cur.append(1)
            continue
        if ones == 6:                            # flag 01111110
            data = cur[:-6]
            if inside and len(data) >= 8 * 18:
                by = bytes(sum(data[i * 8 + k] << k for k in range(8)) for i in range(len(data) // 8))
                if fcs(by[:-2]) == (by[-2] | by[-1] << 8):
                    frames.append(by[:-2])
            cur, inside = [], True
        elif ones == 5:                          # stuffed zero
            pass
        else:
            cur.append(0)
        ones = 0
    return frames


def ax25(fr):
    def call(a):
        c = "".join(chr(x >> 1) for x in a[:6]).strip()
        ssid = (a[6] >> 1) & 15
        return c + ("-%d" % ssid if ssid else "")
    addrs = [call(fr[i:i + 7]) for i in range(0, 21, 7)]
    return "%s>%s,%s:%s" % (addrs[1], addrs[0], addrs[2], fr[23:].decode("latin1"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf", help="cfw ELF (default: the radio's cfw/build)")
    a = ap.parse_args()
    emu = os.path.join(SRC, "emu") if a.radio == "d890" else os.path.join(D168, "emu")
    sys.path.insert(0, emu)
    import txrx_loop as T
    if a.elf:
        T.ELF = os.path.abspath(a.elf)
    first = T.first_builtin() if a.radio == "d890" else 1
    name = "aprs_%s" % a.radio
    d = os.path.join(T.OUT, name)
    os.makedirs(d, exist_ok=True)
    cap = os.path.join(d, "tx_aprs.adc16")
    syms = T.nm(T.ELF)
    steps = T.keys(first) + T.ident_steps(syms, "A") + ["2.0|shot", "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap,
             "0|lcd Press 13 150", "0.4|shot"] + ["0|lcd Press 15 150", "0.3|version"] * 4 + [   # Up x4: last rows are GPS, INFO, SAVE, APRS heard
             "0.2|shot", "0|lcd Press 13 150", "0.6|shot", "0|lcd Press 13 150", "0.3|shot", "1.5|shot",
             "0|pwm CaptureStop", "0|rf0 Status"]
    T.run(name, steps, seconds=6.0)
    jp = os.path.join(d, "tx_aprs.json")
    if not os.path.exists(jp):
        print("FAIL: no TX capture")
        return 1
    meta = json.load(open(jp))
    x = np.fromfile(os.path.join(d, meta["file"]), dtype="<u2").astype(float)
    print("captured %.2f s" % (len(x) / (meta["rate"] or 48000)))
    frames = demod(x, meta["rate"] or 48000.0)
    for f in frames:
        print("decoded:", ax25(f))
    ok = any(ax25(f).startswith("XX0AAA>APZCFW,WIDE1-1:!") for f in frames)
    shots = sorted(f for f in os.listdir(d) if f.startswith("lcd_") and f.endswith(".png"))
    import subprocess
    subprocess.call([sys.executable, os.path.join(emu, "montage.py"), os.path.join(T.OUT, "aprs_%s.png" % a.radio)]
                    + ["%d=%s" % (i + 1, os.path.join(d, f)) for i, f in enumerate(shots[-5:])])
    print("APRS beacon decoded:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
