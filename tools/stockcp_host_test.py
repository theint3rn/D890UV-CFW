#!/usr/bin/env python3
"""Stock CPS DMR / NXDN sections in the firmware (app/stockcp.c + app/cfwcp.c compiled for the PC), both radio models:
codeplugs made with cfw_cps carry a Radio ID List, the basic privacy / ARC4 / AES / NX key tables, channel stock
columns that pick them, NX Setting, NX Talk Group and the DMR / NX Digital Contact Lists (section 16); the firmware
has to see the same keys, IDs, names and flags.

  python3 cfw/tools/stockcp_host_test.py          (also part of `make test-libs`)"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CFW = os.path.dirname(HERE)
CPS = os.path.join(CFW, "..", "cfw_cps")
sys.path.insert(0, CPS)
from cps import codeplug as C, stock as ST  # noqa: E402

AES128 = "00112233445566778899AABBCCDDEEFF"
AES256 = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"


def build(model, tmp):
    exe = os.path.join(tmp, "stockcp_%s" % model)
    subprocess.check_call(["gcc", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-int-to-pointer-cast",
                           "-include", "tools/cfwcp_host.h", "-DCFWCP_MODEL_ID=%du" % C.MODELS[model]["id"],
                           "-Iapp", "-Ihal", "-Idrivers", "-Iinclude", "-Idsp", "-Ivoc",
                           "tools/stockcp_host_test.c", "app/cfwcp.c", "app/stockcp.c", "-o", exe], cwd=CFW)
    return exe


def codeplug(model):
    cp = C.load(os.path.join(CPS, "examples", "synth.d8cp"))
    if cp.model != model:
        ST.convert_model(cp, model)
    ST.use(model)
    d890 = model == "D890UV"
    cp.stock["RadioIDList"] = [["1", "9990001", "N0CALL"], ["2", "9991592", "ALT ID"]]
    cp.stock["AESEncryptionCode"] = [["5", "1", AES128], ["6", "2", AES256], ["7", "3", "XYZ"]]
    cp.stock["ARC4EncryptionCode"] = [["2", "0102030405"], ["3", "01"]]
    cp.stock["DMRDigitalContactList"] = [["1", "9990011", "XX0AAA", "Alice Example", "", "", "", "", "Private Call", ""],
                                         ["2", "9990012", "XX0ZZZ", "Bob", "", "", "", "", "Private Call", ""]]
    if d890:
        bp = ST.default_rows("EncryptionCode")
        bp[2][1] = "4660"                                  # row 3: key 0x1234, Encryption ID 65535 -> key id 3
        bp[9][1], bp[9][2] = "1", "200"                    # row 10: key 1, id 200
        cp.stock["EncryptionCode"] = bp
        nx = ST.default_rows("NXEncryptionCode")
        nx[3][0], nx[3][1] = "1", "12345"                  # row 4 valid, code 12345, key id 4
        cp.stock["NXEncryptionCode"] = nx
        ST.set_stock_value(cp, "NXSetting", "OwnId", "4321")
        ST.set_stock_value(cp, "NXSetting", "NxdnMoni", "1")
        cp.stock["NXTalkGroup"] = [["65000", "WW", "", "", "", "", "", "Group Call", "1", ""],
                                   ["100", "", "Local NX", "", "", "", "", "Group Call", "0", ""]]
        cp.stock["NXDigitalContactList"] = [["1001", "NX1CALL", "Ann", "Bee", "", "", "", "", "", ""]]
    if not d890:                                           # .rdt-only dialog (table 72): No., hex key, ID; order = list order
        cp.stock["EncryptionCodeRdt"] = [["10", "0001", "200"], ["3", "1234", "65535"], ["12", "ABCD", ""], ["33", "5555", "1"]]
    cp.keys = [C.Key("own", "DMR_AES256", 9, 0, AES256)]
    chans = [("DMR", {"Radio ID": "ALT ID", "Digital Encryption": "3", "AES Digital Encryption": "Normal Encryption"}),
             ("DMR", {"Digital Encryption": "5", "AES Digital Encryption": "Enhanced Encryption"}),
             ("DMR", {"ARC4": "2", "Digital Encryption": "3"}),
             ("NXDN" if d890 else "DMR", {"NxdnEncry": "4"} if d890 else {"Digital Encryption": "6",
                                                                          "AES Digital Encryption": "Enhanced Encryption"}),
             ("DMR", {"Digital Encryption": "3"}),           # + section 11 key 0: the cfw key wins
             ("DMR", {"Digital Encryption": "7", "AES Digital Encryption": "Enhanced Encryption"}),   # bad AES key: none
             ("DMR", {})]
    cols = ST.TABLES["Channel"]["columns"]
    for i, (mode, st) in enumerate(chans):
        c = C.Channel(name="ST%d" % i, rx_hz=451000000 + 12500 * i, tx_hz=451000000 + 12500 * i, mode=mode,
                      **({"ran": 1, "tg": 100} if mode == "NXDN" else {"cc": 1, "slot": 1, "tg": 91}))
        c.stock = {k: v for k, v in st.items() if k in cols}
        if i == 4:
            c.key = 0
        cp.channels[i] = c
    return cp


def main():
    tmp = tempfile.mkdtemp(prefix="stockcp_host_")
    fails = 0
    for model in ("D890UV", "D168UV"):
        d890 = model == "D890UV"
        exe = build(model, tmp)
        cp = codeplug(model)
        p = os.path.join(tmp, model + ".bin")
        open(p, "wb").write(C.encode(cp, for_radio=True))
        args = [str(i) for i in range(7)] + ["u:1:9990011", "u:1:9990019", "u:5:1001", "g:5:65000", "g:5:100", "g:1:91"]
        out = subprocess.run([exe, p] + args, capture_output=True, text=True).stdout
        lines = out.splitlines()
        keys = {l.split()[3].strip(): l for l in lines if l.startswith("key ")}       # by name ("BP", "AES", ...)
        kl = [l.split() for l in lines if l.startswith("key ")]
        idx = {"%s %s" % (k[2], k[3]): int(k[1]) for k in kl}                      # "BP 3" -> index
        chk = {int(l.split()[1]): l.split() for l in lines if l.startswith("ch ")}

        def key_of(ch):
            return int(chk[ch][7])

        def want(cond, what):
            nonlocal fails
            print("%s %s %s" % ("PASS" if cond else "FAIL", model, what))
            fails += not cond
        want("own_keys 1" in out, "section 12 key first")
        want(idx.get("ARC4 2") is not None and "kind 1 id 2 len 5 0102030405" in out, "ARC4 key id 2 (40 bit)")
        want("ARC4 3" not in idx, "ARC4 key with a bad length left out")
        want(idx.get("AES 5") is not None and ("kind 2 id 5 len 16 " + AES128) in out, "AES-128 key id 5")
        want(idx.get("AES 6") is not None and ("kind 3 id 6 len 32 " + AES256) in out, "AES-256 key id 6")
        want("AES 7" not in idx, "AES key with bad hex left out")
        want(int(chk[0][5]) == 9991592, "channel 0 radio ID from the Radio ID List (ALT ID)")
        want(int(chk[6][5]) == 0, "channel without Radio ID: none")
        want(key_of(1) == idx.get("AES 5"), "channel 1 Enhanced + Digital Encryption 5 -> AES key id 5")
        want(key_of(2) == idx.get("ARC4 2"), "channel 2 ARC4 2 wins over Digital Encryption")
        want(key_of(4) == 0, "channel 4: section 11 key wins")
        want(key_of(5) == -1, "channel 5: AES id 7 has no valid key -> none")
        want(key_of(6) == -1, "channel 6: no encryption")
        want("user 1 9990011 XX0AAA Alice Exampl" in out, "DMR user database name (12-char name)")
        want("user 1 9990019 -" in out, "unknown ID: no name")
        want("group 1 91 " in out and "group 1 91 -" not in out, "DMR TG 91 name from the cfw contacts")
        want(key_of(0) == idx.get("BP 3") and idx.get("BP 3") is not None, "channel 0 Normal + Digital Encryption 3 -> Encryption Code No. 3")
        want("kind 0 id 3 len 2 1234" in out and "kind 0 id 200 len 2 0001" in out, "Encryption Code: key 0x1234 id 3 (65535 = the No.), key 1 id 200")
        want("5555" not in out, "Encryption Code No. 33: out of range, left out")
        if d890:
            want(idx.get("BP 3") is not None and "kind 0 id 3 len 2 1234" in out, "basic privacy row 3: key 0x1234, id = row")
            want("kind 0 id 200 len 2 0001" in out, "basic privacy row 10: Encryption ID 200")
            want(len([k for k in kl if k[2] == "BP"]) == 2, "unset basic privacy rows (65535) left out")
            want(key_of(0) == idx.get("BP 3"), "channel 0 Normal + Digital Encryption 3 -> basic privacy row 3")
            want("kind 7 id 4 len 2 3039" in out, "NX Encryption Code row 4 -> scrambler code 12345")
            want(key_of(3) == idx.get("NX SCR"), "NXDN channel NxdnEncry 4 -> NX row 4")
            want("nx_own 4321" in out and "any_ran 1" in out, "NX Setting OwnId 4321, Moni on + RAN Any")
            want("group 5 65000 WW forbid 1" in out, "NX Talk Group 65000 'WW', TxForbid")
            want("group 5 100 Local NX forbid 0" in out, "NX Talk Group 100 name from FIRST_NAME")
            want("user 5 1001 NX1CALL Ann Bee" in out, "NX user database name")
        else:
            want(not any(k[2] == "NX" for k in kl), "no NX tables on the D168UV")
            want("kind 0 id 12 len 2 ABCD" in out, "Encryption Code No. 12 without an ID: key id = the No.")
            want(len([k for k in kl if k[2] == "BP"]) == 3, "only the listed Encryption Code rows become keys")
            want(key_of(3) == idx.get("AES 6"), "D168 channel Enhanced + Digital Encryption 6 -> AES-256 id 6")
            want("nx_own 0" in out and "any_ran 0" in out, "no NX Setting on the D168UV")
        if fails:
            print(out)
    print("RESULT:", "PASS" if not fails else "FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
