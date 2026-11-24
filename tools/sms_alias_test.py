#!/usr/bin/env python3
"""Two-radio DMR SMS + talker alias test in the emulator (either radio), using the txrx_loop.py helpers.

  python3 cfw/tools/sms_alias_test.py [--radio d890|d168]

Radio A (ID 9990201, call XX0AAA = its talker alias) on the DMR simplex built-in channel: Menu > SMS > New message,
target = the channel's TG (prefilled), text "HI 73" typed with the multi-tap keys, Menu = send; then 2 s of PTT voice
(test tone). Its TX audio PWM is captured. Radio B (9990202) gets the capture through analog Stream, then opens
Menu > SMS > Inbox. PASS = B's screen shows the message text from 9990201 and the talker alias XX0AAA during the call."""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
D168 = os.path.abspath(os.path.join(SRC, "..", "..", "D168UV"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--msms", action="store_true", help="A sends the SMS as M-SMS (Motorola-style TMS over IP)")
    a = ap.parse_args()
    emu = os.path.join(SRC, "emu") if a.radio == "d890" else os.path.join(D168, "emu")
    sys.path.insert(0, emu)
    import txrx_loop as T
    import numpy as np
    first = T.first_builtin() if a.radio == "d890" else 1
    ptt = "A10" if a.radio == "d890" else "D0"
    ch = first + T.SIMPLEX["dmr"]
    syms = T.nm(T.ELF)
    tag = "_msms" if a.msms else ""
    nA, nB = "sms%s_%s_A" % (tag, a.radio), "sms%s_%s_B" % (tag, a.radio)
    dA, dB = os.path.join(T.OUT, nA), os.path.join(T.OUT, nB)
    os.makedirs(dA, exist_ok=True)
    cap = os.path.join(dA, "tx_ptt.adc16")
    tone = syms["s_tone_hz"][0]
    tap = []                                     # "HI 73": H = 4 x2, I = 4 x3, space = 0, 7 = 7 x5, 3 = 3 x4
    for key, n in ((4, 2), (4, 3), (0, 1), (7, 5), (3, 4)):
        for _ in range(n):
            tap += ["0|lcd Press %d 100" % (key + 1), "0.2|version"]
        tap.append("1.3|version")
    fmt = ["0|sysbus WriteByte 0x%08X 1" % syms["s_rs_msms"][0]] if a.msms else []
    steps = T.keys(ch) + T.ident_steps(syms, "A") + fmt + ["0.3|shot", "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap,
             "0|lcd Press 13 150", "0.4|shot", "0|lcd Press 7 150", "0.4|shot", "0|lcd Press 2 150", "0.4|shot",
             "0|lcd Press 13 150", "0.4|shot"] + tap + ["0|shot", "0|lcd Press 13 150", "0.3|shot", "1.5|shot",
             "0|sysbus WriteWord 0x%08X 440" % tone, '0|board Set "%s" false' % ptt, "1.0|shot", "1.5|shot",
             '0|board Set "%s" true' % ptt, "1.0|shot", "0|pwm CaptureStop"]
    print("[A] SMS + voice on channel %d" % ch, flush=True)
    T.run(nA, steps)
    jp = os.path.join(dA, "tx_ptt.json")
    if not os.path.exists(jp):
        print("FAIL: no TX capture"); return 1
    import json
    meta = json.load(open(jp))
    tx = np.fromfile(os.path.join(dA, meta["file"]), dtype="<u2").astype(float)
    fs = meta["rate"] or 48000.0
    pad = np.full(int(0.3 * fs), 2048.0)
    os.makedirs(dB, exist_ok=True)
    rx_in = os.path.join(dB, "rx_in.adc16")
    np.clip(np.round(np.concatenate([pad, tx, pad])), 0, 4095).astype("<u2").tofile(rx_in)
    dur = (len(tx) + 2 * len(pad)) / fs
    tlog = os.path.join(dB, "text.log")
    if os.path.exists(tlog):
        os.remove(tlog)
    print("[B] %.1f s of A's TX into analog Stream" % dur, flush=True)
    steps = T.keys(ch) + T.ident_steps(syms, "B") + ["0|rf0 SignalHere -70", T.text_hook(syms, tlog),
             "0|analog Stream @%s %d" % (rx_in, int(fs)), "%.3f|shot" % (dur * 0.75), "%.3f|shot" % (dur * 0.25 + 1.0),
             "0|lcd Press 13 150", "0.4|shot", "0|lcd Press 7 150", "0.4|shot", "0|lcd Press 3 150", "0.4|shot",
             "0|lcd Press 13 150", "0.6|shot"]
    T.run(nB, steps)
    text = open(tlog).read() if os.path.exists(tlog) else ""
    sms = "HI 73" in text and "FROM 9990201" in text
    alias = "XX0AAA" in text and "TA XX0AAA" in text
    shots = sorted(f for f in os.listdir(dB) if re.match(r"lcd_\d+\.png$", f))
    shotsA = sorted(f for f in os.listdir(dA) if re.match(r"lcd_\d+\.png$", f))
    import subprocess
    mont = os.path.join(T.OUT, "sms_alias%s_%s.png" % (tag, a.radio))
    pick = [os.path.join(dA, f) for f in shotsA[-12:-3:2]] + [os.path.join(dB, f) for f in shots[-6:]]
    subprocess.call([sys.executable, os.path.join(emu, "montage.py"), mont] + ["%d=%s" % (i + 1, p) for i, p in enumerate(pick)])
    print("SMS (%s format) received on B:" % ("M-SMS" if a.msms else "cfw"), "PASS" if sms else "FAIL")
    print("talker alias XX0AAA shown on B:", "PASS" if alias else "FAIL")
    print("montage:", mont)
    return 0 if sms and alias else 1


if __name__ == "__main__":
    sys.exit(main())
