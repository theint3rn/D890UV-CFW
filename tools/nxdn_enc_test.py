#!/usr/bin/env python3
"""NXDN encryption end to end in the emulator (two radios = TX capture of one, streamed into the other), either radio.
For each rate (NXDN48 EHR / NXDN96 EFR) and cipher (scramble, DES-OFB, AES-256-OFB): codeplug = cfw_cps/examples/synth.d8cp with two NXDN
channels put first, 1 "NXENC 451" 451.0000 MHz (not an amateur band) and 2 "NXENC HAM" 439.9000 MHz, both with the
cipher's key, Settings > Encryption on.

  python3 cfw/tools/nxdn_enc_test.py [--radio d890|d168] [--rates 48,96] [--ciphers SCR,DES,AES]

A: channel 1, PTT 2 s with the 440 Hz test tone -> TX captured; g_radio.enc_tx must be 1 (encrypted).
B: same codeplug, A's TX streamed into the receiver -> the 440 Hz tone is decoded (key known).
C: same codeplug with Encryption off -> keys not used: the call shows ENC and is muted (no tone).
H: channel 2 (amateur band, once per rate), PTT -> sent in clear (enc_tx 2, "CLEAR TX" on the screen).
Writes <emu>/out/nxdn_enc_<radio>/report.md. Run one at a time (machine load causes false fails).
"""
import argparse
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
CPS = os.path.join(SRC, "cfw_cps")
sys.path.insert(0, HERE)
sys.path.insert(0, CPS)
import ui_test as U  # noqa: E402
from enc_test import reads  # noqa: E402
from cps import codeplug as C, stock as ST  # noqa: E402

KEYS = {"SCR": ("NXDN_SCR", 1, "2A5B"),
        "DES": ("NXDN_DES", 2, "ABCDEF0123456789"),
        "AES": ("NXDN_AES", 3, "ABCDEF0123456789CDEF0123456789ABEF0123456789ABCD0123456789ABCDEF")}


def make_cp(model, rate, cipher, enc, out):
    cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    kind, kid, hx = KEYS[cipher]
    cp.keys = [C.Key("NX " + cipher, kind, kid, 0, hx)]
    for i, (nm, hz) in enumerate((("NXENC 451", 451000000), ("NXENC HAM", 439900000))):
        c = C.Channel(name=nm, rx_hz=hz, tx_hz=hz, mode="NXDN", ran=1, tg=99)
        c.nxdn_rate = 9600 if rate == 96 else 4800
        c.key = 0
        cp.channels[i] = c
    cp.settings.encryption = enc
    cp.settings.dmr_id = 9990201
    cp.settings.start_channel = 0
    p = os.path.join(out, "nxenc_%s_%d_%s_%d.d8cp" % (model, rate, cipher, enc))
    C.save(cp, p)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--rates", default="48,96")
    ap.add_argument("--ciphers", default="SCR,DES,AES")
    a = ap.parse_args()
    R = U.RADIOS[a.radio]
    sys.path.insert(0, R["emu"])
    import txrx_loop as T
    elf = R["elf"]
    S = {k: v[0] for k, v in T.nm(elf).items()}
    out = os.path.join(R["emu"], "out", "nxdn_enc_%s" % a.radio)
    os.makedirs(out, exist_ok=True)
    ptt = R["side"]["ptt"]
    off = None
    for l in subprocess.check_output(["gdb-multiarch", "-batch", "-ex", "print (int)&((radio_t*)0)->enc_tx", elf],
                                     text=True, stderr=subprocess.DEVNULL).splitlines():
        if "= " in l:
            off = int(l.split("= ")[1])
    if off is None:
        print("no debug info for radio_t.enc_tx")
        return 1
    enc_tx = S["g_radio"] + off

    def ui(name, cpf, steps):
        subprocess.run([sys.executable, os.path.join(HERE, "ui_test.py"), name, "--radio", a.radio, "--elf", elf,
                        "--codeplug", cpf] + steps, capture_output=True, text=True)
        return os.path.join(R["emu"], "out", name)

    def text(d):
        p = os.path.join(d, "text.txt")
        return open(p).read() if os.path.exists(p) else ""

    def rx(name, cpf, rx_in, dur):
        raw = os.path.join(out, name + "_pcm.raw")
        if os.path.exists(raw):
            os.remove(raw)
        d = ui(name, cpf, ["keys:14,14", "0|rf0 SignalHere -70",
                           "0|pcm Attach 0x%08X 0x%08X 8192 @%s" % (S["g_emu_pcm"], S["g_emu_pcm_wr"], raw),
                           "0|analog Stream @%s 48000" % rx_in, "%.2f|shot" % (dur * 0.7), "%.2f|shot" % (dur * 0.3 + 0.5),
                           "0|pcm Detach"])
        pcm = np.fromfile(raw, dtype="<i2").astype(float) if os.path.exists(raw) else np.zeros(0)
        f, snr, rms = T.tone_measure(pcm, 8000.0) if len(pcm) > 2000 else (None, None, 0.0)
        return f, snr, text(d), len(pcm)

    rows, ok_all = [], True
    for rate in [int(x) for x in a.rates.split(",")]:
        for ci in a.ciphers.split(","):
            tag = "%s_%d_%s" % (a.radio, rate, ci)
            cp_on, cp_off = make_cp(R["model"], rate, ci, True, out), make_cp(R["model"], rate, ci, False, out)
            cap = os.path.join(out, "tx_%s.adc16" % tag)
            if os.path.exists(cap):
                os.remove(cap)
            dA = ui("nxencA_" + tag, cp_on, ["keys:14,14", "0|sysbus WriteWord 0x%08X 440" % S["s_tone_hz"],
                    "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap, '0|board Set "%s" false' % ptt, "0.5|shot",
                    "0|sysbus ReadByte 0x%08X" % enc_tx, "2.0|shot", '0|board Set "%s" true' % ptt, "1.0|shot",
                    "0|pwm CaptureStop"])
            va = reads(os.path.join(dA, "then_out.txt"))
            a_enc = va[0] if va else -1
            if not os.path.exists(cap) or os.path.getsize(cap) < 1000:
                rows.append((rate, ci, "A no TX capture", False))
                ok_all = False
                continue
            x = np.fromfile(cap, dtype="<u2").astype(float)
            pad = np.full(int(0.3 * 48000), 2048.0)
            rx_in = os.path.join(out, "rx_in_%s.adc16" % tag)
            np.clip(np.round(np.concatenate([pad, x, pad])), 0, 4095).astype("<u2").tofile(rx_in)
            dur = (len(x) + 2 * len(pad)) / 48000.0
            fb, sb, tb, nb = rx("nxencB_" + tag, cp_on, rx_in, dur)
            fc, sc, tc, nc = rx("nxencC_" + tag, cp_off, rx_in, dur)
            okA = a_enc == 1
            okB = fb is not None and abs(fb - 440) < 10 and (sb or 0) > 10
            okC = (fc is None or abs(fc - 440) > 10 or (sc or 0) < 3) and "ENC" in tc
            fm = lambda f, s: "%s Hz / %s dB" % ("%.1f" % f if f else "-", "%.1f" % s if s is not None else "-")
            rows.append((rate, ci, "A enc_tx=%d" % a_enc, okA))
            rows.append((rate, ci, "B keyed RX: %s (pcm %d)" % (fm(fb, sb), nb), okB))
            rows.append((rate, ci, "C Encryption off: %s, ENC shown %s" % (fm(fc, sc), "ENC" in tc), okC))
            ok_all &= okA and okB and okC
            for r in rows[-3:]:
                print("NXDN%d %-3s %-60s %s" % (r[0], r[1], r[2], "PASS" if r[3] else "FAIL"), flush=True)
        tag = "%s_%d" % (a.radio, rate)
        dH = ui("nxencH_" + tag, make_cp(R["model"], rate, a.ciphers.split(",")[-1], True, out),
                ["keys:14,14", "0|lcd Press 15 150", "0.6|shot", '0|board Set "%s" false' % ptt, "0.6|shot",
                 "0|sysbus ReadByte 0x%08X" % enc_tx, '0|board Set "%s" true' % ptt, "1.0|shot"])
        vh = reads(os.path.join(dH, "then_out.txt"))
        okH = bool(vh) and vh[-1] == 2 and "CLEAR TX" in text(dH)
        rows.append((rate, "-", "H amateur band: enc_tx=%s, CLEAR TX shown %s" % (vh[-1] if vh else "-",
                     "CLEAR TX" in text(dH)), okH))
        ok_all &= okH
        print("NXDN%d %-3s %-60s %s" % (rate, "-", rows[-1][2], "PASS" if okH else "FAIL"), flush=True)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# NXDN encryption emulator test (%s, %s)\n\n| rate | cipher | check | result |\n|---|---|---|---|\n"
                % (a.radio, os.path.basename(elf)))
        for r in rows:
            f.write("| NXDN%d | %s | %s | %s |\n" % (r[0], r[1], r[2], "PASS" if r[3] else "FAIL"))
        f.write("\nRESULT: %s\n" % ("PASS" if ok_all else "FAIL"))
    print("RESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
