#!/usr/bin/env python3
"""Emulator test of cal_dump.py against the STOCK firmware (D890UV V1.05 / D168UV V1.07) in Renode.

The stock PC link is USB CDC on both radios (D890: RX ring 0x2001BF18 fed by the CDC callback FUN_0801b770, replies
through FUN_0801b7b8; D168: USB packet poll FUN_080a3b9c into 0x2000D6A8, replies through FUN_0808c63c). The
emulators have no USB device model, so this test adds a small USB-CDC shim with CPU hooks (no emulator files
changed): a TCP server on localhost hands each request to a hook through a file, the hook puts it where the CDC
driver would (D890: RX ring + head; D168: the packet buffer, return value = length), and the reply hook appends the
firmware's reply bytes to a file the shim streams back. The stock command parser, the 'R' handler and the NAND
reads are the firmware's own.

Copy A = the emulator's synthetic calibration block (D890 emu/images/rfcal_synthetic.pages, D168
D168UV/emu/images/synth.pages), copy B = A with changed values; both laid over the stock NAND image.
Scenarios: 1) A and B valid -> A in use; 2) A marked invalid -> the radio serves B for A; 3) B invalid too -> none.
Each dump must match the expected bytes byte for byte (span + markers; --full: the whole 256 / 128 KB block A).

  python3 tools/hw/emu_cal_test.py --radio d890 [--full] [--keep]
  python3 tools/hw/emu_cal_test.py --radio d168
"""
import argparse
import importlib.util
import json
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
PROJECTS = os.path.dirname(os.path.dirname(SRC))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(SRC, "cfw_cps"))
import calmap  # noqa: E402
import cal_dump  # noqa: E402
from cps.link import TcpPort  # noqa: E402

EMU = {"d890": os.path.join(SRC, "emu"), "d168": os.path.join(PROJECTS, "D168UV", "emu")}
PAGE = {"d890": 0x1000, "d168": 0x800}
CAL_SRC = {"d890": os.path.join(SRC, "emu", "images", "rfcal_synthetic.pages"),
           "d168": os.path.join(PROJECTS, "D168UV", "emu", "images", "synth.pages")}

RECV_RING = """import System
if System.IO.File.Exists(INPATH):
    d = System.IO.File.ReadAllBytes(INPATH)
    System.IO.File.Delete(INPATH)
    h = int(self.Bus.ReadWord(0x200042E6))
    for b in d:
        self.Bus.WriteByte(0x2001BF18 + h, b)
        h = (h + 1) % 0x800
    self.Bus.WriteByte(0x2001C71D, 1)
    self.Bus.WriteWord(0x200042E6, h)
"""
RECV_PACKET = """import System
from Antmicro.Renode.Peripherals.CPU import RegisterValue
n = 0
if System.IO.File.Exists(INPATH):
    d = System.IO.File.ReadAllBytes(INPATH)
    System.IO.File.Delete(INPATH)
    buf = int(self.GetRegister(1).RawValue)
    for i in range(len(d)):
        self.Bus.WriteByte(buf + i, d[i])
    n = len(d)
self.SetRegister(0, RegisterValue.Create(n, 32))
self.PC = self.GetRegister(14)
"""
SEND = """import System
from Antmicro.Renode.Peripherals.CPU import RegisterValue
buf = BUF if BUF else int(self.GetRegister(0).RawValue)
n = int(self.GetRegister(LENREG).RawValue) & 0xFFFF
d = System.Array.CreateInstance(System.Byte, n)
for i in range(n):
    d[i] = self.Bus.ReadByte(buf + i)
fs = System.IO.File.Open(OUTPATH, System.IO.FileMode.Append)
fs.Write(d, 0, n)
fs.Close()
self.SetRegister(0, RegisterValue.Create(1, 32))
self.PC = self.GetRegister(14)
"""
# (hook address, script, BUF, LENREG)
HOOKS = {"d890": [(0x0812D63C, RECV_RING, 0, 0), (0x0801B7B8, SEND, 0, 1)],
         "d168": [(0x080A3B9C, RECV_PACKET, 0, 0), (0x0808C63C, SEND, 0x2000D6A8, 0)]}


# ------------------------------------------------------------------------------------------------ pages files
def read_pages(path, page):
    d = open(path, "rb").read()
    out = {}
    for o in range(0, len(d) - 3, 4 + page):
        out[struct.unpack_from("<I", d, o)[0]] = d[o + 4:o + 4 + page]
    return out


def block_from_pages(pages, base, size, page):
    blk = bytearray(b"\xff" * size)
    for a, p in pages.items():
        if base <= a < base + size:
            blk[a - base:a - base + page] = p
    return blk


def write_pages(path, items, page):
    """items: [(base, block bytes)] -> pages file with every non-blank page."""
    with open(path, "wb") as f:
        for base, blk in items:
            for o in range(0, len(blk), page):
                p = bytes(blk[o:o + page])
                if p != b"\xff" * page:
                    f.write(struct.pack("<I", base + o) + p)


def blank_page_items(base, blk, offs, page):
    """Pages (full contents) around the given offsets, for a runtime overlay that may blank a marker."""
    return [(base + o, bytes(blk[o:o + page])) for o in sorted({x & ~(page - 1) for x in offs})]


def write_page_list(path, pages):
    with open(path, "wb") as f:
        for a, p in pages:
            f.write(struct.pack("<I", a) + p)


# ------------------------------------------------------------------------------------------------ USB-CDC shim
class Shim(threading.Thread):
    """localhost TCP <-> files read/written by the CPU hooks."""
    def __init__(self, port, inpath, outpath):
        super().__init__(daemon=True)
        self.inpath, self.outpath = inpath, outpath
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.stop = False

    def run(self):
        while not self.stop:
            r, _, _ = select.select([self.srv], [], [], 0.2)
            if not r:
                continue
            c, _ = self.srv.accept()
            self.serve(c)

    def serve(self, c):
        pending = b""
        pos = os.path.getsize(self.outpath) if os.path.exists(self.outpath) else 0
        while not self.stop:
            r, _, _ = select.select([c], [], [], 0.005)
            if r:
                d = c.recv(4096)
                if not d:
                    break
                pending += d
                continue                                   # collect the whole frame (one USB packet)
            if pending and not os.path.exists(self.inpath):
                tmp = self.inpath + ".tmp"
                with open(tmp, "wb") as f:
                    f.write(pending)
                os.replace(tmp, self.inpath)
                pending = b""
            if os.path.exists(self.outpath) and os.path.getsize(self.outpath) > pos:
                with open(self.outpath, "rb") as f:
                    f.seek(pos)
                    d = f.read()
                pos += len(d)
                c.sendall(d)
        c.close()


# ------------------------------------------------------------------------------------------------ emulator
def load_ctl(radio):
    spec = importlib.util.spec_from_file_location("ctl_%s" % radio, os.path.join(EMU[radio], "ctl.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def wait_port(port, t):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


def start_emulator(radio, name, mon, extra):
    emu = EMU[radio]
    args = [sys.executable, os.path.join(emu, "run.py")] + (["--orig"] if radio == "d890" else []) + \
        ["--seconds", "12" if radio == "d890" else "8", "--name", name, "--serve", str(mon)]
    for e in extra:
        args += ["--extra", e]
    subprocess.check_call(args, cwd=emu, timeout=900)
    out = os.path.join(emu, "out", name)
    pid = int(open(os.path.join(out, "renode.pid")).read().strip())
    return pid, out


def ensure_broker(radio, mon, timeout=2700):
    """run.py --serve starts a ctl.py broker 3 s after Renode; if Renode's monitor was not up yet that broker died,
    so start one more. Never kill a waiting broker or probe the monitor port: Renode's monitor serves only its first
    client, and it answers only after the boot script's RunFor (slow with the hooks)."""
    if wait_port(mon + 1, 20):
        return True
    p = None
    end = time.time() + timeout
    while time.time() < end:
        if p is None or p.poll() is not None:                    # (re)start only when the previous one exited
            p = subprocess.Popen([sys.executable, os.path.join(EMU[radio], "ctl.py"), "--port", str(mon), "broker"],
                                 cwd=EMU[radio], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        if wait_port(mon + 1, 5):
            return True
    return False


def my_pids(mon, renode_pid):
    """Renode + its ctl.py broker (found by ps, killed by pid)."""
    pids = [renode_pid]
    ps = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    for ln in ps.splitlines():
        p = ln.split(None, 1)
        if len(p) == 2 and "ctl.py" in p[1] and "--port %d broker" % mon in p[1]:
            pids.append(int(p[0]))
    return pids


# ------------------------------------------------------------------------------------------------ test
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--radio", choices=("d890", "d168"), required=True)
    ap.add_argument("--full", action="store_true", help="scenario 1 reads the whole NAND block (slow in the emulator)")
    ap.add_argument("--keep", action="store_true", help="leave the emulators running")
    ap.add_argument("--scenario", help="comma list of 1,2,3 (default all)")
    ap.add_argument("--monitor", type=int, default=None)
    ap.add_argument("--shim-port", type=int, default=None)
    a = ap.parse_args()
    radio = a.radio
    c = calmap.CAL[radio]
    page = PAGE[radio]
    mon = a.monitor or (1481 if radio == "d890" else 1491)
    shim_port = a.shim_port or mon + 2000
    name = "caldump_test_%s" % radio
    work = os.path.join(EMU[radio], "out", name + "_shim")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    inpath, outpath = os.path.join(work, "in.bin"), os.path.join(work, "out.bin")

    # expected copies: A = the emulator's synthetic calibration block, B = A with other values
    A = block_from_pages(read_pages(CAL_SRC[radio], page), c["a"], c["block"], page)
    if calmap.markers(radio, A)["valid"] is False:
        raise SystemExit("%s has no valid calibration block A" % CAL_SRC[radio])
    B = bytearray(A)
    for i in range(c["crc_span"]):
        B[i] = (B[i] + 0x11) & 0xFF if i % 3 else B[i] ^ 0x5A
    for o in c["ram_patched"]:          # keep the loader-checked bytes: a D168 +0x1D that differs from the settings
        B[o] = A[o]                      # makes the stock save the calibration back to NAND at boot (FUN_08027ff8)
    B[c["ok_off"]:c["ok_off"] + 4] = calmap.OK_MARK
    ab = os.path.join(work, "cal_ab.pages")
    write_pages(ab, [(c["a"], A), (c["b"], B)], page)
    A_bad = bytearray(A)
    A_bad[c["bad_off"]:c["bad_off"] + 4] = calmap.BAD_MARK
    B_bad = bytearray(B)
    B_bad[c["bad_off"]:c["bad_off"] + 4] = calmap.BAD_MARK
    inv_a, inv_b = os.path.join(work, "inv_a.pages"), os.path.join(work, "inv_b.pages")
    write_page_list(inv_a, blank_page_items(c["a"], A_bad, [c["bad_off"], c["ok_off"]], page))
    write_page_list(inv_b, blank_page_items(c["b"], B_bad, [c["bad_off"], c["ok_off"]], page))

    hooks = []
    for i, (addr, code, buf, lenreg) in enumerate(HOOKS[radio]):
        path = os.path.join(work, "hook%d.py" % i)
        with open(path, "w") as f:
            f.write(code.replace("BUF", str(buf)).replace("LENREG", str(lenreg)))
        cond = "" if code is SEND else "System.IO.File.Exists(INPATH) and "    # idle calls: one file check only
        if code is RECV_PACKET:
            cond = ""                                            # must fake the USB call's return every time
        hooks.append("cpu AddHook 0x%08X \"import System; INPATH='%s'; OUTPATH='%s'; %sexecfile('%s')\""
                     % (addr, inpath, outpath, cond, path))
    for p in (ab, inpath, outpath):
        if " " in p:
            raise SystemExit("no spaces allowed in emulator paths: %s" % p)

    ctl = load_ctl(radio)
    shim = Shim(shim_port, inpath, outpath)
    shim.start()
    t0 = time.time()
    results, ok = [], True
    left = []

    def step(label, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        results.append(dict(step=label, ok=bool(cond), detail=detail))
        print("%-4s %s %s" % ("PASS" if cond else "FAIL", label, detail), flush=True)

    def dump(full=False):
        for attempt in range(80):                                # until the stock link answers (boot)
            port = TcpPort("127.0.0.1", shim_port, timeout=4.0)
            link = cal_dump.StockLink(port, log=lambda s: None, chunk=0x10)
            try:
                link.enter()
                break
            except cal_dump.CalError:
                port.close()
                time.sleep(3)
        else:
            raise SystemExit("stock firmware never answered PROGRAM")
        try:
            res = cal_dump.dump(link, radio, full=full, log=lambda s: None)
            n = len([f for f in os.listdir(work) if f.startswith("dump") and f.endswith(".json")])
            with open(os.path.join(work, "dump%d_A.bin" % n), "wb") as f:
                f.write(res[0])
            with open(os.path.join(work, "dump%d_B.bin" % n), "wb") as f:
                f.write(res[1])
            with open(os.path.join(work, "dump%d.json" % n), "w") as f:
                json.dump(res[2], f, indent=1)
            return link, res
        finally:
            link.leave()
            port.close()

    def boot(overlays):
        """Fresh stock boot per scenario: the stock firmware decides block validity at boot, so a marker changed
        while it runs is not seen (same as a real radio)."""
        for p in (inpath, outpath):
            if os.path.exists(p):
                os.remove(p)
        rpid, _ = start_emulator(radio, name, mon, ["nand LoadPages @%s" % o for o in overlays] + hooks)
        if not ensure_broker(radio, mon):
            stop(rpid)
            raise SystemExit("emulator monitor did not come up")
        ctl.talk(mon, ["start"])
        return rpid

    def stop(rpid):
        if a.keep:
            left.append(rpid)
            return
        try:
            ctl.talk(mon, ["quit"], timeout=20)
        except Exception:
            pass
        time.sleep(1)
        for p in my_pids(mon, rpid):
            try:
                os.kill(p, 15)
            except ProcessLookupError:
                pass
        time.sleep(2)

    scen = [("1", [ab]), ("2", [ab, inv_a]), ("3", [ab, inv_a, inv_b])]
    if a.scenario:
        scen = [x for x in scen if x[0] in a.scenario.split(",")]
    try:
        for sid, overlays in scen:
            rpid = boot(overlays)
            try:
                if sid == "1":
                    span = c["block"] if a.full else c["crc_span"]
                    link, (ga, gb, rep) = dump(a.full)
                    step("link up (stock PROGRAM / ident)", link.ident is not None, repr(link.ident))
                    step("1: copy in use A", rep["copy_in_use"] == "A", rep["note"])
                    step("1: A as served == emulator block A (0x%X bytes)" % span, ga[:span] == bytes(A[:span]))
                    step("1: A markers", ga[c["ok_off"]:c["ok_off"] + 4] == calmap.OK_MARK
                         and ga[c["bad_off"]:c["bad_off"] + 4] == bytes(A[c["bad_off"]:c["bad_off"] + 4]))
                    step("1: B as served == block B", gb[:c["crc_span"]] == bytes(B[:c["crc_span"]]))
                    step("1: CRC = calmap CRC of A", rep["crc32_in_use"] == "%08x" % calmap.crc(radio, A),
                         rep["crc32_in_use"])
                    step("1: decoded values = decode(A)",
                         rep["decoded"] == json.loads(json.dumps(calmap.decode(radio, A))))
                elif sid == "2":
                    _, (ga, gb, rep) = dump()
                    step("2: A invalid -> served copy is B", rep["copy_in_use"] == "A or B" and
                         ga[:c["crc_span"]] == bytes(B[:c["crc_span"]]), rep["note"])
                    step("2: CRC = CRC of B", rep["crc32_in_use"] == "%08x" % calmap.crc(radio, B),
                         rep["crc32_in_use"])
                else:
                    _, (ga, gb, rep) = dump()
                    step("3: A and B invalid -> no copy", rep["copy_in_use"] is None and rep["decoded"] is None,
                         rep["note"])
            finally:
                stop(rpid)
    finally:
        shim.stop = True
    pids = left
    with open(os.path.join(work, "result.json"), "w") as f:
        json.dump(dict(radio=radio, ok=ok, seconds=round(time.time() - t0), steps=results), f, indent=1)
    print("%s (%d s), log %s/result.json%s" % ("ALL PASS" if ok else "FAILED", time.time() - t0, work,
                                              "; emulator left running (pids %s)" % pids if a.keep else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
