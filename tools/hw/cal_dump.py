#!/usr/bin/env python3
"""Read the RF / TX calibration block out of a radio that still runs the STOCK firmware, over the stock PC link
(programming cable on USART1, or the D890UV USB port), before the custom firmware is flashed. READ-ONLY.

  python3 tools/hw/cal_dump.py --radio d890 --port /dev/ttyACM0        # D890UV USB (CDC)
  python3 tools/hw/cal_dump.py --radio d168 --port /dev/ttyUSB0        # programming cable
  python3 tools/hw/cal_dump.py --radio d890 --port tcp::3481           # emulator USART1 socket

Protocol = the stock PC link (also what the stock CPS uses): "PROGRAM" -> "QX" 06, 02 -> 'I' ident ... 06,
'R' a3 a2 a1 a0 len -> 'W' a3 a2 a1 a0 len data sum 06, "END". This tool sends ONLY those four frames: the send
path refuses anything else ('W' writes, 'U'pdate, 'F'actory, ...), see ALLOWED / send().

Stock 'R' handler (D890 FUN_0812efdc, D168 FUN_08090000 range, case 0xb): reads the NAND at any address, no
limits, but when the 256 KB (D890) / 128 KB (D168) block of the address has no valid marker it silently reads the
same offset one block higher. So reading copy A returns exactly the copy the firmware uses (A if valid, else B).
Reading copy B returns B if B is valid, else the block after it. The tool reads both, checks the markers and
reports which copy is in use.

Output (default ./cal_backups/): cal_<radio>_<tag>.bin = the copy in use (whole NAND block, as served for copy A),
cal_<radio>_<tag>_B.bin = what the radio serves for copy B, and cal_<radio>_<tag>.json = markers, the copy in use,
CRC-32 (compare with the cfw DEBUG=1 RTT boot line), and the decoded APC / tune values (tools/hw/calmap.py).
"""
import argparse
import datetime
import hashlib
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import calmap  # noqa: E402

ACK = 0x06
ALLOWED = (b"PROGRAM", b"\x02", b"END")        # plus 'R' + 4 address bytes + 1 length byte (6 bytes total)


class CalError(Exception):
    pass


def check_frame(frame):
    """The only gate to the radio. Read-only frames pass; everything else raises (nothing is sent)."""
    if frame in ALLOWED:
        return
    if len(frame) == 6 and frame[:1] == b"R" and 0 < frame[5] <= 0xFF:
        return
    raise CalError("refusing to send %r: cal_dump only reads (PROGRAM / 02 / R / END)" % frame[:8])


class StockLink:
    def __init__(self, port, log=print, chunk=0x80):
        if not 0 < chunk <= 0xFF:
            raise CalError("chunk 1..255")
        self.p, self.log, self.chunk = port, log, chunk
        self.ident = None

    def send(self, frame):
        check_frame(frame)
        self.p.write(frame)

    def enter(self):
        self.p.drain()
        for _ in range(3):
            self.send(b"PROGRAM")
            if self.p.read(3, 2.0) == b"QX\x06":
                break
            self.p.drain()
        else:
            raise CalError("no answer to PROGRAM: radio off, wrong port, or not the stock firmware?")
        self.send(b"\x02")
        r = b""
        end = time.time() + 2.0
        while time.time() < end and not (r[:1] == b"I" and r[-1:] == b"\x06" and len(r) >= 8):
            r += self.p.read(1, 0.3)
        if r[:1] != b"I":
            raise CalError("bad identify reply %s" % r.hex())
        self.ident = r[1:-1]
        if b"CFW" in self.ident:
            raise CalError("radio runs the custom firmware (%r): its link has no NAND access. Dump the calibration "
                           "with the stock firmware, or use the cfw RTT boot line" % self.ident)
        return self.ident

    def read(self, addr, n):
        frame = b"R" + struct.pack(">IB", addr, n)
        for _ in range(3):
            self.send(frame)
            r = self.p.read(8 + n)
            if len(r) == 8 + n and r[:1] == b"W" and r[1:6] == frame[1:] and r[-1] == ACK and \
                    sum(r[1:6 + n]) & 0xFF == r[6 + n]:
                return r[6:6 + n]
            self.p.drain()
        raise CalError("read 0x%08X+%d failed" % (addr, n))

    def read_range(self, addr, size, what=""):
        out = bytearray()
        t0 = time.time()
        while len(out) < size:
            n = min(self.chunk, size - len(out))
            out += self.read(addr + len(out), n)
            if len(out) % 0x8000 == 0 or len(out) == size:
                self.log("  %s 0x%08X: %d / %d KB (%.0f s)" % (what, addr, len(out) // 1024, size // 1024,
                                                                time.time() - t0))
        return bytes(out)

    def leave(self):
        try:
            self.send(b"END")
            self.p.read(1, 1.0)
        except Exception:
            pass


def dump(link, radio, full=True, log=print):
    """Read both copies. Returns (blk_a, blk_b, report dict)."""
    c = calmap.CAL[radio]
    size = c["block"] if full else c["crc_span"]

    def copy(base, what):
        data = bytearray(b"\xff" * c["block"])
        data[:size] = link.read_range(base, size, what)
        if not full:                                                  # markers only
            data[c["bad_off"]:c["bad_off"] + 4] = link.read(base + c["bad_off"], 4)
            data[c["ok_off"]:c["ok_off"] + 4] = link.read(base + c["ok_off"], 4)
        return bytes(data)

    a = copy(c["a"], "copy A")
    b = copy(c["b"], "copy B")
    ma, mb = calmap.markers(radio, a), calmap.markers(radio, b)
    same = a[:size] == b[:size]
    if not ma["valid"]:
        in_use, note = None, "copy A as served has no valid marker: NO valid copy (stock would use raw B; cfw: no TX)"
    elif not same:
        in_use, note = "A", "copy A valid (A and B differ, so A is not redirected)"
    else:
        in_use, note = "A or B", ("served copy valid; A and B read identical (either A valid with an identical B, or "
                                  "A invalid and the radio redirected to B): same bytes either way")
    rep = dict(radio=c["name"], ident=link.ident.decode("latin-1") if link.ident else None,
               nand_a="0x%08X" % c["a"], nand_b="0x%08X" % c["b"], block_size=c["block"], read_size=size,
               copy_a_as_served=ma, copy_b_as_served=mb, a_equals_b=same, copy_in_use=in_use, note=note,
               crc32_in_use="%08x" % calmap.crc(radio, a), crc_span=c["crc_span"],
               sha256_in_use=hashlib.sha256(a[:size]).hexdigest(),
               decoded=calmap.decode(radio, a) if ma["valid"] else None,
               areas=calmap.areas(radio, a) if ma["valid"] else None,
               related_blocks=related(link, radio, a if ma["valid"] else None),
               created=datetime.datetime.now().isoformat(timespec="seconds"))
    return a, b, rep


def related(link, radio, in_use=None):
    """Other NAND blocks the stock code keeps near the calibration: markers, and for a partial calibration copy its
    bytes and whether they equal the copy in use (no calibration is taken from them)."""
    c = calmap.CAL[radio]
    out = []
    for a, b, role, n in calmap.RELATED[radio]:
        st = dict(role=role)
        for name, base in (("first", a), ("second", b)):
            if base is None:
                continue
            blk = bytearray(b"\xff" * c["block"])
            blk[c["bad_off"]:c["bad_off"] + 4] = link.read(base + c["bad_off"], 4)
            blk[c["ok_off"]:c["ok_off"] + 4] = link.read(base + c["ok_off"], 4)
            st[name] = dict(addr="0x%08X" % base, **calmap.markers(radio, blk))
        if n:
            data = link.read_range(a, n, "related")
            st["data"] = data.hex()
            st["equals_in_use_prefix"] = in_use is not None and data == bytes(in_use[:n])
        out.append(st)
    return out


def save(outdir, radio, tag, a, b, rep):
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.join(outdir, "cal_%s_%s" % (radio, tag))
    for path, data in ((stem + ".bin", a), (stem + "_B.bin", b)):
        if os.path.exists(path):
            raise CalError("%s exists: use another --tag" % path)
        with open(path, "wb") as f:
            f.write(data)
    with open(stem + ".json", "w") as f:
        json.dump(rep, f, indent=1)
    return stem


def summary(rep):
    L = ["%s calibration: copy in use = %s" % (rep["radio"], rep["copy_in_use"] or "NONE"), "  " + rep["note"],
         "  A markers: ok %s / invalid %s" % (rep["copy_a_as_served"]["ok_marker"],
                                              rep["copy_a_as_served"]["invalid_marker"]),
         "  B markers: ok %s / invalid %s" % (rep["copy_b_as_served"]["ok_marker"],
                                              rep["copy_b_as_served"]["invalid_marker"]),
         "  CRC-32 of the first 0x%X bytes: %s   (cfw DEBUG=1 RTT boot line must show the same)"
         % (rep["crc_span"], rep["crc32_in_use"])]
    d = rep["decoded"]
    if d:
        L.append("  UHF tune points: %s" % d["uhf_tune_points"])
        L.append("  VHF tune points: %s" % d["vhf_tune_points"])
        for band in ("uhf_apc", "vhf_apc"):
            x = d[band]
            L.append("  %s: usable %s, max %d, per level cal %s" % (band.upper(), x["usable"], x["apc_max"],
                     " ".join("%s=%d" % (k, v["cal"]) for k, v in x["levels"].items())))
        L.append("  TX possible with the cfw: %s" % d["tx_possible"])
        for band, q in d["squelch"].items():
            L.append("  squelch %-20s base %5d, reg 0x49 at ref for levels 1-5: %s%s" % (
                band, q["base"], " ".join(str(v) for v in q["reg49_at_ref"].values()),
                "" if q["plausible"] else "  (blank?)"))
        L.append("  stock sub-areas: %d (all saved in the .json 'areas'; the .bin holds the whole block)"
                 % len(rep["areas"]))
    for r in rep.get("related_blocks") or []:
        L.append("  related %s%s valid %s%s%s: %s" % (
            r["first"]["addr"], " / " + r["second"]["addr"] if "second" in r else "", r["first"]["valid"],
            " / %s" % r["second"]["valid"] if "second" in r else "",
            ", = copy in use" if r.get("equals_in_use_prefix") else (", differs from copy in use" if "data" in r
                                                                     else ""), r["role"][:50]))
    if not d:
        L.append("  NOT VALID: do not TX with the cfw on this radio; keep / restore stock (docs/CALIBRATION.md)")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--radio", choices=sorted(calmap.CAL), required=True)
    ap.add_argument("--port", required=True, help="/dev/ttyACM0 (D890 USB), /dev/ttyUSB0 (cable), COM3, tcp:HOST:PORT")
    ap.add_argument("--out", default=os.path.join(os.getcwd(), "cal_backups"))
    ap.add_argument("--tag", default=None, help="file tag, e.g. the radio's serial number (default: time)")
    ap.add_argument("--quick", action="store_true", help="read only the used part + markers (not the whole block)")
    ap.add_argument("--chunk", type=lambda s: int(s, 0), default=0x80, help="bytes per R request (1..255)")
    a = ap.parse_args(argv)
    sys.path.insert(0, os.path.join(SRC, "cfw_cps"))
    from cps.link import open_port            # serial / Windows COM / tcp; pyserial not needed
    port = open_port(a.port, timeout=3.0)
    link = StockLink(port, chunk=a.chunk)
    try:
        ident = link.enter()
        print("radio: %r" % ident)
        if calmap.CAL[a.radio]["name"].encode() not in ident:
            print("warning: ident does not name %s: check --radio" % calmap.CAL[a.radio]["name"])
        blk_a, blk_b, rep = dump(link, a.radio, full=not a.quick)
    except CalError as e:
        sys.exit("ERROR: %s" % e)
    finally:
        link.leave()
        port.close()
    tag = a.tag or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    stem = save(a.out, a.radio, tag, blk_a, blk_b, rep)
    print(summary(rep))
    print("saved %s.bin / _B.bin / .json  - copy them off this PC too" % stem)
    return 0 if rep["copy_in_use"] else 2


if __name__ == "__main__":
    sys.exit(main())
