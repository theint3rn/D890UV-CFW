#!/usr/bin/env python3
"""RF register + GPIO states of the stock firmware against the cfw, in the emulator (both radios).

  python3 cfw/tools/rf_compare.py trace --radio d890|d168 --fw stock|cfw [--elf ELF] --out DIR    (stock: your own AnyTone files)
  python3 cfw/tools/rf_compare.py table --out DIR > cfw/docs/RF_COMPARE_RAW.md

Every RF register write is logged (hook on the write routine: stock D890UV FUN_080e2f64, D168UV FUN_0807bc60, cfw rf_write) with
a marker per radio state; GPIO MODER / ODR (D890UV) or CRL / CRH / ODR (D168UV) are read at each marker. Registers whose top
nibble selects a sub-register (IDX) are kept per sub-register. Stock D890UV states come from copies of the NAND image in which
every channel is the wanted one and settings byte +0x21 is the broadcast mode; nothing of them is kept in the repository."""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

IDX = (0x40, 0x49, 0x52, 0x53, 0x2E)
# Hz, digital, 25 kHz
CH = {"vhf_fm": (146520000, 0, 0), "uhf_fm": (446000000, 0, 0), "vhf_fmw": (146520000, 0, 1), "uhf_fmw": (446000000, 0, 1),
      "vhf_dmr": (145510000, 1, 0), "uhf_dmr": (441000000, 1, 0)}


def bcd(hz):
    return bytes.fromhex("%08d" % (hz // 10))


def nand_image(src, dst, hz, dmr, bc, wide):
    """D890UV stock NAND copy: every channel record = hz simplex, analog / digital; broadcast mode bc (0 off, 1 FM, 2 AM)"""
    d = open(src, "rb").read()
    out = bytearray()
    for k in range(0, len(d), 4100):
        a = struct.unpack_from("<I", d, k)[0]
        pg = bytearray(d[k + 4:k + 4100])
        if 0x01000000 <= a < 0x02000000 and (a - 0x01000000) % 0x80000 < 0x4000:
            for r in range(0, 4096, 128):
                if pg[r:r + 4] != b"\xff\xff\xff\xff" and pg[r] != 0:
                    pg[r:r + 4] = bcd(hz)
                    pg[r + 4:r + 8] = bytes(4)
                    pg[r + 8] = (pg[r + 8] & 0x2C) | dmr | wide << 4     # simplex
                    if not dmr:
                        pg[r + 9] = 0                             # no tones
        if a in (0x03500000, 0x03540000):
            pg[0x21] = bc
        out += struct.pack("<I", a) + pg
    open(dst, "wb").write(out)


def gpio_reads(radio):
    if radio == "d890":
        return ["sysbus ReadDoubleWord 0x%08X" % (0x42020000 + 0x400 * p + o) for p in range(5) for o in (0x00, 0x14)]
    return ["sysbus ReadDoubleWord 0x%08X" % (0x40010800 + 0x400 * p + o) for p in range(5) for o in (0x00, 0x04, 0x0C)]


def runner(radio, fw, name, log, nand, elf, boot, steps, extra=()):
    R = U.RADIOS[radio]
    if os.path.exists(log):
        os.remove(log)
    if fw == "stock":
        adr = 0x080e2f64 if radio == "d890" else 0x0807bc60
        ra, rb, rc = (("0", "1", "2") if radio == "d890" else ("0", "1", None))
        cmd = [sys.executable, os.path.join(R["emu"], "run.py")] + (["--orig"] if radio == "d890" else [])
        if nand:
            cmd += ["--nand", nand]
    else:
        nm = subprocess.check_output(["arm-none-eabi-nm", elf], text=True)
        fn = "write_reg" if radio == "d168" else "rf_write"      # D168UV: rf_write is inlined around write_reg(reg, value)
        adr = next(int(x.split()[0], 16) for x in nm.splitlines() if x.split()[-1] == fn) & ~1
        ra, rb, rc = ("0", "1", None) if radio == "d168" else ("1", "2", "0")
        cmd = [sys.executable, os.path.join(R["emu"], "run.py")] + R["args"] + ["--elf", elf]
    hook = ("cpu AddHook 0x%08X \"import System; r=self.GetRegister(%s).RawValue&0x7F; v=self.GetRegister(%s).RawValue&0xFFFF; c=%s; "
            "System.IO.File.AppendAllText('%s', '%%d %%02X %%04X' %% (c,r,v) + chr(10))\"") % (
            adr, ra, rb, "self.GetRegister(%s).RawValue&0xFF" % rc if rc else "0", log)
    cmd += ["--seconds", str(boot), "--name", name, "--timeout", "2400", "--extra", hook]
    for e in extra:
        cmd += ["--extra", e]
    for s in steps:
        cmd += ["--then", s]
    subprocess.run(cmd, cwd=R["emu"], capture_output=True, text=True)
    return os.path.join(R["emu"], "out", name)


def states(radio, log, outdir, hz=0):
    """-> {marker: (regs {(chip, reg, idx): value}, gpio [..])}"""
    then = os.path.join(outdir, "then_out.txt")
    g = [int(m, 16) for m in re.findall(r"(?m)^> sysbus ReadDoubleWord 0x[0-9A-Fa-f]+\s*\n\s*(0x[0-9A-Fa-f]+)", open(then).read())] if os.path.exists(then) else []
    per = len(gpio_reads(radio))
    regs, out, k = {}, {}, 0
    if radio == "d168" and hz and "_stock_" in os.path.basename(log):
        return states_a(radio, log, g, per, hz)
    for ln in open(log):
        if ln.startswith("#"):
            out[ln[1:].strip()] = (dict(regs), g[k * per:(k + 1) * per])
            k += 1
            continue
        c, r, v = ln.split()
        c, r, v = int(c), int(r, 16), int(v, 16)
        regs[(c, r, v >> 12 if r in IDX else -1)] = v
    return out


def states_a(radio, log, g, per, hz):
    """stock D168UV alternates between VFO A and B: keep the writes of the blocks (56 = 0 ... next 56 = 0) tuned to A only"""
    cur, regs, out, k, seen = [], {}, {}, 0, False      # seen: the receiver was on once (before: power-on set-up, kept)

    def flush():
        f = {r: v for c, r, v in cur if r in (0x68, 0x69)}
        if not seen or len(f) < 2 or abs((f[0x69] << 16 | f[0x68]) - hz) < 100000:
            for c, r, v in cur:
                regs[(c, r, v >> 12 if r in IDX else -1)] = v
    for ln in open(log):
        if ln.startswith("#"):
            flush()
            cur = []
            out[ln[1:].strip()] = (dict(regs), g[k * per:(k + 1) * per])
            k += 1
            continue
        c, r, v = ln.split()
        c, r, v = int(c), int(r, 16), int(v, 16)
        if r == 0x56 and v == 0:
            flush()
            cur = []
        seen |= r == 0x56 and v == 1
        cur.append((c, r, v))
    return out


def mark(radio, log, t):
    return ["0|python \"import System; System.IO.File.AppendAllText('%s', '# %s' + chr(10))\"" % (log, t)] + ["0|" + x for x in gpio_reads(radio)]


def trace(a):
    import pickle
    radio, fw = a.radio, a.fw
    R = U.RADIOS[radio]
    elf = a.elf or R["elf"]
    os.makedirs(a.out, exist_ok=True)
    allst = {}
    ptt = 'board Set "%s" ' % R["side"]["ptt"]
    vol = ["0|analog Set 2 2500"] if radio == "d890" else []
    jobs = [("vhf_fm", 0), ("uhf_fm", 0), ("vhf_fmw", 0), ("uhf_fmw", 0), ("vhf_dmr", 0), ("uhf_dmr", 0)]
    if radio == "d890":
        jobs += [("vhf_fm", 1), ("uhf_fm", 1), ("vhf_fm", 2), ("uhf_fm", 2)]
    else:
        jobs += [("uhf_dmr", 1)]
    for key, bc in jobs:
        hz, dmr, wide = CH[key]
        tag = key + ("" if not bc else "_bc%d" % bc)
        if a.only and tag not in a.only.split(","):
            continue
        log = os.path.join(a.out, "%s_%s_%s.txt" % (radio, fw, tag))
        M = lambda t: mark(radio, log, t)                                      # noqa: E731
        st = vol + ["0.5|version"]
        nand = None
        pre, boot, extra = [], 4.0, []
        if fw == "stock":
            boot = 14.0
            if radio == "d890":
                nand = os.path.join(a.out, "nand_%s.pages" % tag)
                nand_image(os.path.join(R["emu"], "images", "extflash.pages"), nand, hz, dmr, bc, wide)
                extra = ["nand LoadPages @%s" % os.path.join(R["emu"], "images", "rfcal_synthetic.pages")]   # as the cfw runs
            else:
                press = lambda *c: [x for k in c for x in ("0|lcd Press %d 150" % k, "0.8|version")]     # noqa: E731
                if not bc:                             # VFO A: the frequency, then Menu > Settings > Chan Set > CH Type = A-Analog
                    pre = press(16, 15, *[int(x) + 1 for x in "%08d" % (hz // 10)])
                    if not dmr:
                        pre += press(13, 16, 16, 16, 16, 16, 16, 13, 16, 13, 16, 16, 13, 15, 13) + ["2|version"] + press(14, 14, 14)
                else:
                    extra = ["nand Poke 0x2500021 1"]  # FM function on
                pre += ["1|version"]
        else:
            pre, extra = cfw_pre(radio, key, bc, a.out, R)
        st += pre + ["0.5|shot"] + M(tag + " rx")
        if not bc and not dmr:
            st += ["0|rf0 SignalHere -30", "2|shot"] + M(tag + " rx signal") + ["0|rf0 SignalOff", "1.5|version"] + M(tag + " rx closed")
        if not bc and not dmr:
            st += ["0|" + ptt + "false", "1|shot"] + M(tag + " tx") + ["0|" + ptt + "true", "2|version"] + M(tag + " rx after tx")
        if fw == "cfw":                                # boot state: the same key state before anything
            pass
        out = runner(radio, fw, "rfcmp_%s_%s_%s" % (radio, fw, tag), log, nand, elf, boot, st, extra)
        if os.path.exists(log):
            allst.update(states(radio, log, out, hz if not bc else 0))
        if nand:
            os.remove(nand)
        print(tag, "states", [k for k in allst if k.startswith(tag)], flush=True)
    p = os.path.join(a.out, "%s_%s.pkl" % (radio, fw))
    old = pickle.load(open(p, "rb")) if a.only and os.path.exists(p) else {}
    old.update(allst)
    pickle.dump(old, open(p, "wb"))


def cfw_pre(radio, key, bc, out, R):
    """cfw: a one-channel codeplug for the state -> (steps, extra lines)"""
    sys.path.insert(0, U.CPS)
    from cps import codeplug as C, stock as ST
    hz, dmr, wide = CH[key]
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != R["model"]:
        ST.convert_model(cp, R["model"])
    ST.use(R["model"])
    ST.set_stock_value(cp, "OptionalSetting", "MenuLowPower", "11")
    if bc:
        ST.set_stock_value(cp, "OptionalSetting", "FM_En", str(bc))
    c = cp.channels[81] if dmr else cp.channels[0]
    c.rx_hz = c.tx_hz = hz
    cp.channels[0] = c
    c.scanlist = None
    c.wide = bool(wide)
    cp.settings.start_channel = 0
    p = os.path.join(out, "cfw_%s.d8cp" % key)
    C.save(cp, p)
    b = U.codeplug_bin(p, R["model"], out)
    b2 = os.path.join(out, "cfw_%s_%d.bin" % (key, bc))
    os.replace(b, b2)
    return (["0.8|version"], ["gps Enabled false", "sysbus LoadBinary @%s 0x%08X" % (b2, R["base"])])


def table(a):
    import pickle
    for radio in ("d890", "d168"):
        try:
            s = pickle.load(open(os.path.join(a.out, "%s_stock.pkl" % radio), "rb"))
            c = pickle.load(open(os.path.join(a.out, "%s_cfw.pkl" % radio), "rb"))
        except OSError:
            continue
        print("# %s\n" % radio)
        for st in s:
            if st not in c:
                print("## %s: no cfw state\n" % st)
                continue
            sr, sg = s[st]
            cr, cg = c[st]
            rows = []
            for k in sorted(set(sr) | set(cr)):
                if sr.get(k) != cr.get(k) and k[1] not in (0x68, 0x69):
                    rows.append("chip %d %02X%s: stock %s cfw %s" % (k[0], k[1], "[%X]" % k[2] if k[2] >= 0 else "",
                                "%04X" % sr[k] if k in sr else "-", "%04X" % cr[k] if k in cr else "-"))
            print("## %s (%d stock / %d cfw registers, %d differ)" % (st, len(sr), len(cr), len(rows)))
            print("\n".join("- " + r for r in rows))
            names = gpio_names(radio)
            for (p, o), x, y in zip(names, sg, cg):
                if x != y:
                    print("- GPIO%s %s: stock %08X cfw %08X (xor %08X)" % (p, o, x, y, x ^ y))
            print()


def gpio_names(radio):
    if radio == "d890":
        return [("ABCDE"[p], o) for p in range(5) for o in ("MODER", "ODR")]
    return [("ABCDE"[p], o) for p in range(5) for o in ("CRL", "CRH", "ODR")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["trace", "table"])
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--fw", default="cfw", choices=["stock", "cfw"])
    ap.add_argument("--elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only")
    a = ap.parse_args()
    if a.cmd == "table":
        return table(a)
    trace(a)


if __name__ == "__main__":
    main()
