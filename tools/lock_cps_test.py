#!/usr/bin/env python3
"""The CPS link is the way back from a forgotten power-on password (emulator).

  python3 cfw/tools/lock_cps_test.py [--radio d890|d168] [--elf ELF]       (normal build, or build_hw_save_pw)

Boots with a codeplug that has Password = On, 1234 -> password screen; reads the radio over the emulated programming UART
from there, writes the same codeplug without the password -> the radio opens at once, and stays open after a power cycle."""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ui_test as U  # noqa: E402

sys.path.insert(0, U.CPS)
sys.path.insert(0, os.path.join(U.CPS, "tools"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="d890", choices=["d890", "d168"])
    ap.add_argument("--elf")
    ap.add_argument("--monitor", type=int, default=1391)
    ap.add_argument("--uart", type=int, default=3491)
    a = ap.parse_args()
    os.environ["EMU_NO_ANYTONE"] = "1"
    d168 = a.radio == "d168"
    if d168 and "D168UV" not in sys.argv:
        sys.argv.append("D168UV")                  # emu_roundtrip picks the D168 emulator's ctl.py by this
    import emu_roundtrip as ER
    from cps import codeplug as C, ops, stock as ST
    R = U.RADIOS[a.radio]
    elf = os.path.abspath(a.elf) if a.elf else R["elf"]
    out = os.path.join(R["emu"], "out", "lockcps")
    os.makedirs(out, exist_ok=True)
    cp = C.load(os.path.join(U.CPS, "examples", "synth.d8cp"))
    if cp.model != R["model"]:
        ST.convert_model(cp, R["model"])
    ST.use(R["model"])
    ST.set_stock_value(cp, "OptionalSetting", "Password", "1")
    ST.set_stock_value(cp, "OptionalSetting", "Password_Char", "1234")
    tmp = tempfile.mkdtemp(prefix="lockcps_")
    C.save(cp, os.path.join(tmp, "a.d8cp"))
    binf = U.codeplug_bin(os.path.join(tmp, "a.d8cp"), R["model"], out)
    load = "sysbus LoadBinary @%s 0x%08X" % (binf, R["base"])
    cmd = [sys.executable, os.path.join(R["emu"], "run.py")] + R["args"] + ["--elf", elf, "--seconds", "4", "--name", "lockcps",
           "--serve", str(a.monitor), "--extra", load]
    cmd += ["--uart-socket", "usart1:%d" % a.uart] if d168 else ["--extra", 'emulation CreateServerSocketTerminal %d "progterm" false' % a.uart,
                                                                 "--extra", "connector Connect sysbus.usart1 progterm"]
    subprocess.check_call(cmd, cwd=R["emu"])
    if not ER.wait_port(a.monitor + 1, 30) and ER.wait_port(a.monitor, 30):
        subprocess.Popen([sys.executable, os.path.join(R["emu"], "ctl.py"), "--port", str(a.monitor), "broker"], cwd=R["emu"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True)
    ER.wait_port(a.monitor + 1)
    txt, res = os.path.join(out, "text.txt"), []

    def step(name, ok, detail=""):
        res.append(bool(ok))
        print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    def screen(secs=2.0):
        if os.path.exists(txt):
            os.remove(txt)
        time.sleep(secs)
        return open(txt).read() if os.path.exists(txt) else ""
    hook = ("cpu AddHook 0x%08X \"import System; p = self.GetRegister(2).RawValue; "
            "s = ''.join([chr(self.Bus.ReadByte(p + i)) for i in range(40)]).split(chr(0))[0]; "
            "System.IO.File.AppendAllText('%s', s + chr(10))\"") % (ER.sym(elf, "gfx_text") & ~1, txt)
    try:
        ER.talk(a.monitor, [hook, "start"])
        port = "tcp:127.0.0.1:%d" % a.uart
        t = ""
        for _ in range(10):
            t = screen()
            if "Power-on password" in t:
                break
        step("password screen", "Power-on password" in t)
        _, raw, inf = ops.read_radio(port, log=lambda s: None, timeout=10)
        step("CPS reads the radio from the password screen", inf["valid"], "%d bytes" % len(raw))
        cp2 = C.decode(raw)
        ST.set_stock_value(cp2, "OptionalSetting", "Password", "0")
        data = ops.write_radio(port, cp2, log=lambda s: None, timeout=10)
        step("CPS writes a codeplug without the password", data is not None)
        t = ""
        for _ in range(6):
            t = screen()
            if "Simplex 1" in t or "SIMPLEX" in t:
                break
        step("radio opens without a power cycle", ("Simplex 1" in t or "SIMPLEX" in t) and "Power-on password" not in t)
        on = ['cpu VectorTableOffset 0x%08X' % ER.sym(elf, "g_vectors"), 'cpu SP `sysbus ReadDoubleWord 0x%08X`' % ER.sym(elf, "g_vectors"),
              'cpu PC `sysbus ReadDoubleWord 0x%08X`' % (ER.sym(elf, "g_vectors") + 4), 'board Set "A0" false'] if d168 else ["board PowerOn"]
        ER.talk(a.monitor, ["pause", "board PowerOff", 'emulation RunFor "00:00:02.000000"', "machine Reset"] + on + ["start"])
        t = ""
        for _ in range(20):
            t = screen(1.5)
            if "Simplex 1" in t or "SIMPLEX" in t:
                break
        step("still open after a power cycle", ("Simplex 1" in t or "SIMPLEX" in t) and "Power-on password" not in t)
    except Exception as e:                       # noqa: BLE001
        step("exception", False, repr(e))
    finally:
        try:
            ER.talk(a.monitor, ["quit"], timeout=20)
        except Exception:                        # noqa: BLE001
            pass
    print("RESULT:", "PASS" if len(res) == 5 and all(res) else "FAIL", "(%d/5)" % sum(res))
    return 0 if len(res) == 5 and all(res) else 1


if __name__ == "__main__":
    sys.exit(main())
