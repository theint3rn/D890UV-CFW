#!/usr/bin/env python3
"""Stock CPS DMR / NXDN sections end to end in the emulator (two radios = TX capture of one, streamed into the other),
either radio. The codeplug (cfw_cps/examples/synth.d8cp + test channels first) takes its keys only from the stock tables
(Encryption Code = basic privacy, ARC4 / AES Encryption Code, NX Encryption Code), picked by the stock channel columns,
the source ID from the Radio ID List ("ALT ID" 9991592 on the channel) or NX Setting OwnId (4321), and the caller name
from the DMR / NX Digital Contact List (codeplug section 16). Settings > Encryption on.

  python3 cfw/tools/stock_sections_test.py [--radio d890|d168] [--elf ELF] [--cases BP,ARC4,AES128,AES256,NXSCR]

Per case:
A: PTT 2 s with the 440 Hz test tone -> TX captured; g_radio.enc_tx must be 1 (encrypted with the stock key).
B: same codeplug, A's TX into the receiver -> 440 Hz decoded, and the caller's user-database entry is on the screen
   (DMR "XX0AAA Alice": only when the call carried the channel's Radio ID; NXDN "NX1CALL Ann Bee" for OwnId 4321).
C: the same key table entry with other key bytes -> no 440 Hz (cfw radios only interoperate with the same key).
Writes <emu>/out/stock_sections_<radio>/report.md. Run one at a time (machine load causes false fails).
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

AES128 = "00112233445566778899AABBCCDDEEFF"
AES256 = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"
# case -> (channel mode, channel stock columns, D890 only)
CASES = {"BP": ("DMR", {"Digital Encryption": "3", "AES Digital Encryption": "Normal Encryption"}, False),
         "ARC4": ("DMR", {"ARC4": "2"}, False),
         "AES128": ("DMR", {"Digital Encryption": "5", "AES Digital Encryption": "Enhanced Encryption"}, False),
         "AES256": ("DMR", {"Digital Encryption": "6", "AES Digital Encryption": "Enhanced Encryption"}, False),
         "NXSCR": ("NXDN", {"NxdnEncry": "4"}, True)}


def flip(hx):
    return "".join("%X" % (int(c, 16) ^ 5) for c in hx)


def make_cp(model, case, wrong, out):
    cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    ST.use(model)
    f = flip if wrong else (lambda x: x)
    cp.keys = []
    cp.stock["RadioIDList"] = [["1", "9990201", "MAIN"], ["2", "9991592", "ALT ID"]]
    cp.stock["AESEncryptionCode"] = [["5", "1", f(AES128)], ["6", "2", f(AES256)]]
    cp.stock["ARC4EncryptionCode"] = [["2", f("0102030405")]]
    cp.stock["DMRDigitalContactList"] = [["1", "9991592", "XX0AAA", "Alice", "", "", "", "", "Private Call", ""],
                                         ["2", "9990201", "MAINID", "Main", "", "", "", "", "Private Call", ""]]
    if model == "D890UV":
        bp = ST.default_rows("EncryptionCode")
        bp[2][1] = "4661" if wrong else "4660"
        cp.stock["EncryptionCode"] = bp
        nx = ST.default_rows("NXEncryptionCode")
        nx[3][0], nx[3][1] = "1", "12346" if wrong else "12345"
        cp.stock["NXEncryptionCode"] = nx
        ST.set_stock_value(cp, "NXSetting", "OwnId", "4321")
        cp.stock["NXDigitalContactList"] = [["4321", "NX1CALL", "Ann", "Bee", "", "", "", "", "", ""]]
    else:                                                  # .rdt-only Encryption Code dialog: No., hex key, ID
        cp.stock["EncryptionCodeRdt"] = [["3", "1235" if wrong else "1234", "65535"]]
    mode, st, _ = CASES[case]
    cols = ST.TABLES["Channel"]["columns"]
    c = C.Channel(name="STOCK " + case, rx_hz=451000000, tx_hz=451000000, mode=mode,
                  **({"ran": 1, "tg": 100} if mode == "NXDN" else {"cc": 1, "slot": 1, "tg": 99}))
    c.stock = dict({"Radio ID": "ALT ID"} if mode == "DMR" else {}, **st)
    c.stock = {k: v for k, v in c.stock.items() if k in cols}
    cp.channels[0] = c
    cp.settings.encryption = True
    cp.settings.dmr_id = 9990201
    cp.settings.nxdn_unit_id = 0
    cp.settings.start_channel = 0
    p = os.path.join(out, "stock_%s_%s_%d.d8cp" % (model, case, wrong))
    C.save(cp, p)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--cases", default="BP,ARC4,AES128,AES256,NXSCR")
    a = ap.parse_args()
    R = U.RADIOS[a.radio]
    sys.path.insert(0, R["emu"])
    import txrx_loop as T
    elf = os.path.abspath(a.elf) if a.elf else R["elf"]
    S = {k: v[0] for k, v in T.nm(elf).items()}
    out = os.path.join(R["emu"], "out", "stock_sections_%s" % a.radio)
    os.makedirs(out, exist_ok=True)
    ptt = R["side"]["ptt"]
    off = None
    for l in subprocess.check_output(["gdb-multiarch", "-batch", "-ex", "print (int)&((radio_t*)0)->enc_tx", elf],
                                     text=True, stderr=subprocess.DEVNULL).splitlines():
        if "= " in l:
            off = int(l.split("= ")[1])
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
    for case in a.cases.split(","):
        if CASES[case][2] and R["model"] != "D890UV":
            rows.append((case, "n/a on the %s (no such stock table)" % R["model"], True))
            continue
        tag = "%s_%s" % (a.radio, case)
        cp_ok, cp_bad = make_cp(R["model"], case, False, out), make_cp(R["model"], case, True, out)
        cap = os.path.join(out, "tx_%s.adc16" % tag)
        if os.path.exists(cap):
            os.remove(cap)
        dA = ui("stA_" + tag, cp_ok, ["keys:14,14", "0|sysbus WriteWord 0x%08X 440" % S["s_tone_hz"],
                "0|pwm AutoCapture false", "0|pwm Capture @%s" % cap, '0|board Set "%s" false' % ptt, "0.5|shot",
                "0|sysbus ReadByte 0x%08X" % enc_tx, "2.0|shot", '0|board Set "%s" true' % ptt, "1.0|shot",
                "0|pwm CaptureStop"])
        va = reads(os.path.join(dA, "then_out.txt"))
        a_enc = va[0] if va else -1
        if not os.path.exists(cap) or os.path.getsize(cap) < 1000:
            rows.append((case, "A no TX capture", False))
            ok_all = False
            continue
        x = np.fromfile(cap, dtype="<u2").astype(float)
        pad = np.full(int(0.3 * 48000), 2048.0)
        rx_in = os.path.join(out, "rx_in_%s.adc16" % tag)
        np.clip(np.round(np.concatenate([pad, x, pad])), 0, 4095).astype("<u2").tofile(rx_in)
        dur = (len(x) + 2 * len(pad)) / 48000.0
        fb, sb, tb, nb = rx("stB_" + tag, cp_ok, rx_in, dur)
        fc, sc, tc, nc = rx("stC_" + tag, cp_bad, rx_in, dur)
        name = "NX1CALL Ann Bee" if CASES[case][0] == "NXDN" else "XX0AAA Alice"
        okA = a_enc == 1
        okB = fb is not None and abs(fb - 440) < 10 and (sb or 0) > 10
        okN = name in tb
        okC = fc is None or abs(fc - 440) > 10 or (sc or 0) < 3
        fm = lambda f, s: "%s Hz / %s dB" % ("%.1f" % f if f else "-", "%.1f" % s if s is not None else "-")
        rows += [(case, "A enc_tx=%d (stock key)" % a_enc, okA),
                 (case, "B same key: %s (pcm %d)" % (fm(fb, sb), nb), okB),
                 (case, "B caller name '%s' on screen: %s" % (name, okN), okN),
                 (case, "C other key bytes: %s" % fm(fc, sc), okC)]
        ok_all &= okA and okB and okN and okC
        for r in rows[-4:]:
            print("%-7s %-62s %s" % (r[0], r[1], "PASS" if r[2] else "FAIL"), flush=True)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# Stock DMR / NXDN sections emulator test (%s, %s)\n\n| case | check | result |\n|---|---|---|\n"
                % (a.radio, os.path.basename(elf)))
        for r in rows:
            f.write("| %s | %s | %s |\n" % (r[0], r[1], "PASS" if r[2] else "FAIL"))
        f.write("\nRESULT: %s\n" % ("PASS" if ok_all else "FAIL"))
    print("RESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
