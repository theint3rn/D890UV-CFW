#!/usr/bin/env python3
"""Encryption (privacy keys) end to end in the emulator, either radio. Codeplug = cfw_cps/examples/synth.d8cp with two DMR simplex channels
put first: 1 "ENC 451" 451.0000 MHz (not an amateur band) and 2 "ENC HAM" 439.9000 MHz, both with key 1 (DMR AES-256,
the cfw privacy scheme), and Settings > Encryption on (codeplug flag).

  python3 cfw/tools/enc_test.py [--radio d890|d168]

A: channel 1, PTT 2 s with the 440 Hz test tone -> TX captured; g_radio.enc_tx must be 1 (encrypted).
B: same codeplug, A's TX streamed into its receiver -> the 440 Hz tone is decoded (key known).
C: same codeplug but Encryption off -> keys not used: the call is seen as encrypted and muted (no tone).
A2: channel 2 (amateur band), PTT -> sent in clear (enc_tx 2, "CLEAR TX" on the screen).
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
CPS = os.path.join(SRC, "cfw_cps")
sys.path.insert(0, HERE)
sys.path.insert(0, CPS)
import ui_test as U  # noqa: E402
from cps import codeplug as C, stock as ST  # noqa: E402


def reads(path):
    vals = []
    for b in re.split(r"(?m)^> ", open(path).read())[1:]:
        l = b.strip().splitlines()
        if l and ("ReadByte" in l[0] or "ReadDoubleWord" in l[0]) and len(l) > 1:
            vals.append(int(l[1].strip(), 16))
    return vals


def make_cp(model, enc, out):
    cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    cp.keys = [C.Key("TEST AES", "DMR_AES256", 1, 0x24, "00112233445566778899AABBCCDDEEFF" * 2)]
    for i, (nm, hz) in enumerate((("ENC 451", 451000000), ("ENC HAM", 439900000))):
        c = C.Channel(name=nm, rx_hz=hz, tx_hz=hz, mode="DMR", cc=1, slot=1, tg=99)
        c.key = 0
        cp.channels[i] = c
    cp.settings.encryption = enc
    cp.settings.dmr_id = 9990201
    cp.settings.start_channel = 0
    p = os.path.join(out, "enc_%s_%d.d8cp" % (model, enc))
    C.save(cp, p)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    a = ap.parse_args()
    R = U.RADIOS[a.radio]
    sys.path.insert(0, R["emu"])
    import txrx_loop as T
    elf = R["elf"]
    S = {k: v[0] for k, v in T.nm(elf).items()}
    out = os.path.join(R["emu"], "out", "enc_e2e_%s" % a.radio)
    os.makedirs(out, exist_ok=True)
    cp_on, cp_off = make_cp(R["model"], True, out), make_cp(R["model"], False, out)
    ptt = R["side"]["ptt"]
    enc_tx = S["g_radio"]          # radio_t: offset of enc_tx found below
    off = None
    for l in subprocess.check_output(["gdb-multiarch", "-batch", "-ex", "print (int)&((radio_t*)0)->enc_tx", elf],
                                     text=True, stderr=subprocess.DEVNULL).splitlines():
        m = re.search(r"= (\d+)", l)
        if m:
            off = int(m.group(1))
    if off is None:
        print("no debug info for radio_t.enc_tx")
        return 1
    cap = os.path.join(out, "tx_a.adc16")

    def ui(name, cpf, steps):
        subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", a.radio, "--elf", elf,
                        "--codeplug", cpf] + steps, capture_output=True, text=True)
        return os.path.join(R["emu"], "out", name)

    # A: encrypted TX on channel 1
    dA = ui("encA_%s" % a.radio, cp_on, ["keys:14,14", "0|sysbus WriteWord 0x%08X 440" % S["s_tone_hz"],
            "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap, '0|board Set "%s" false' % ptt, "0.5|shot",
            "0|sysbus ReadByte 0x%08X" % (enc_tx + off), "2.0|shot", '0|board Set "%s" true' % ptt, "1.0|shot",
            "0|pwm CaptureStop"])
    va = reads(os.path.join(dA, "then_out.txt"))
    a_enc = va[0] if va else -1
    if not os.path.exists(cap) or os.path.getsize(cap) < 1000:
        print("FAIL: no TX capture from A")
        return 1
    x = np.fromfile(cap, dtype="<u2").astype(float)
    pad = np.full(int(0.3 * 48000), 2048.0)
    rx_in = os.path.join(out, "rx_in.adc16")
    np.clip(np.round(np.concatenate([pad, x, pad])), 0, 4095).astype("<u2").tofile(rx_in)
    dur = (len(x) + 2 * len(pad)) / 48000.0

    def rx(name, cpf):
        raw = os.path.join(out, name + "_pcm.raw")
        if os.path.exists(raw):
            os.remove(raw)
        d = ui(name, cpf, ["keys:14,14", "0|rf0 SignalHere -70",
                           "0|pcm Attach 0x%08X 0x%08X 8192 @%s" % (S["g_emu_pcm"], S["g_emu_pcm_wr"], raw),
                           "0|analog Stream @%s 48000" % rx_in, "%.2f|shot" % (dur * 0.7), "%.2f|shot" % (dur * 0.3 + 0.5),
                           "0|pcm Detach"])
        pcm = np.fromfile(raw, dtype="<i2").astype(float) if os.path.exists(raw) else np.zeros(0)
        f, snr, rms = T.tone_measure(pcm, 8000.0) if len(pcm) > 2000 else (None, None, 0.0)
        txt = open(os.path.join(d, "text.txt")).read() if os.path.exists(os.path.join(d, "text.txt")) else ""
        return f, snr, rms, txt, len(pcm)

    fb, sb, rb, tb, nb = rx("encB_%s" % a.radio, cp_on)
    fc, sc, rc, tc, nc = rx("encC_%s" % a.radio, cp_off)
    dA2 = ui("encA2_%s" % a.radio, cp_on, ["keys:14,14", "0|lcd Press 15 150", "0.6|shot",
             '0|board Set "%s" false' % ptt, "0.6|shot", "0|sysbus ReadByte 0x%08X" % (enc_tx + off),
             '0|board Set "%s" true' % ptt, "1.0|shot"])
    va2 = reads(os.path.join(dA2, "then_out.txt"))
    t2 = open(os.path.join(dA2, "text.txt")).read() if os.path.exists(os.path.join(dA2, "text.txt")) else ""
    okA = a_enc == 1
    okB = fb is not None and abs(fb - 440) < 10 and (sb or 0) > 10
    okC = (fc is None or abs(fc - 440) > 10 or (sc or 0) < 3) and "ENC" in tc
    okA2 = bool(va2) and va2[-1] == 2 and "CLEAR TX" in t2
    print("A  channel 1 (451.000) TX: enc_tx=%d -> %s" % (a_enc, "PASS" if okA else "FAIL"))
    print("B  key + Encryption on: tone %s Hz snr %s (pcm %d) -> %s" % ("%.1f" % fb if fb else "-",
          "%.1f" % sb if sb is not None else "-", nb, "PASS" if okB else "FAIL"))
    print("C  Encryption off: tone %s Hz snr %s, ENC shown %s -> %s" % ("%.1f" % fc if fc else "-",
          "%.1f" % sc if sc is not None else "-", "ENC" in tc, "PASS" if okC else "FAIL"))
    print("A2 channel 2 (439.900, amateur): enc_tx=%s, CLEAR TX shown %s -> %s" % (va2[-1] if va2 else "-",
          "CLEAR TX" in t2, "PASS" if okA2 else "FAIL"))
    ok = okA and okB and okC and okA2
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
