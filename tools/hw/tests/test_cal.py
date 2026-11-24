"""Offline tests for cal_dump.py (stock PC link, read-only), calmap.py and `hw.py calram`.

FakeStockRadio speaks the stock protocol like D890 FUN_0812efdc / D168 FUN_080967cc: 'R' of an address whose NAND
block has no valid marker is served from the same offset one block higher."""
import json
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import cal_dump  # noqa: E402
import calmap  # noqa: E402
import hw  # noqa: E402
from test_hw import FakeChip, jlink, Args  # noqa: E402

RADIOS = ("d890", "d168")


def cal_block(radio, seed=0):
    c = calmap.CAL[radio]
    b = bytearray(b"\xff" * c["block"])
    for i in range(0x1C10 if radio == "d890" else 0x700):
        b[i] = (0x80 + ((i * 13 + seed) % 23)) & 0xFF
    L = calmap.LAYOUT[radio]
    b[L["uhf_cal"] + 2:L["uhf_cal"] + 6] = bytes(x + seed for x in (0x40, 0x70, 0xA0, 0xC0))
    b[L["vhf_cal"] + 2:L["vhf_cal"] + 6] = bytes(x + seed for x in (0x30, 0x60, 0x90, 0xB0))
    to, _ = L["uhf_tune"]
    b[to:to + 6] = struct.pack("<3H", 0x500 + seed, 0x800, 0xB00)
    b[c["ok_off"]:c["ok_off"] + 4] = calmap.OK_MARK
    return b


class FakeStockRadio:
    """Byte-level stock PC link over a fake port (write / read / drain / close)."""
    def __init__(self, radio, nand, ident=None):
        self.radio, self.nand = radio, nand          # nand: {block base: bytearray}
        self.block = calmap.CAL[radio]["block"]
        self.ident = ident or (b"D890UV\x00\x01V100\x00\x00" if radio == "d890" else b"D168UV\x00\x01V100\x00\x00")
        self.rx, self.sent = b"", []
        self.in_program = False

    def nand_read(self, addr, n):
        base = addr & ~(self.block - 1)
        blk = self.nand.get(base)
        if blk is None or not calmap.markers(self.radio, blk)["valid"]:       # stock redirect to +1 block
            base += self.block
        blk = self.nand.get(base, bytearray(b"\xff" * self.block))
        off = addr - (addr & ~(self.block - 1))
        return bytes(blk[off:off + n])

    # port API
    def write(self, b):
        self.sent.append(bytes(b))
        if b == b"PROGRAM":
            self.in_program = True
            self.rx += b"QX\x06"
        elif b == b"\x02" and self.in_program:
            self.rx += b"I" + self.ident + b"\x06"
        elif b[:1] == b"R" and len(b) == 6 and self.in_program:
            addr, n = struct.unpack(">IB", b[1:])
            data = self.nand_read(addr, n)
            body = b[1:] + data
            self.rx += b"W" + body + bytes([sum(body) & 0xFF, 6])
        elif b == b"END":
            self.in_program = False
            self.rx += b"\x06"
        elif b[:1] == b"W":
            raise AssertionError("FAKE RADIO: a write frame reached the radio")

    def read(self, n, timeout=None):
        out, self.rx = self.rx[:n], self.rx[n:]
        return out

    def drain(self):
        self.rx = b""

    def close(self):
        pass


def nand_with(radio, a_valid=True, b_valid=True):
    c = calmap.CAL[radio]
    A, B = cal_block(radio, 0), cal_block(radio, 5)
    if not a_valid:
        A[c["bad_off"]:c["bad_off"] + 4] = calmap.BAD_MARK
    if not b_valid:
        B[c["ok_off"]:c["ok_off"] + 4] = b"\xff" * 4
    nxt = bytearray(b"\x11" * c["block"])                    # block after B: unrelated data
    return {c["a"]: A, c["b"]: B, c["b"] + c["block"]: nxt}, A, B


@pytest.mark.parametrize("radio", RADIOS)
def test_check_frame_refuses_writes(radio):
    for f in (b"PROGRAM", b"\x02", b"END", b"R\x04\xf0\x00\x00\x10"):
        cal_dump.check_frame(f)
    for f in (b"W\x04\xf0\x00\x00\x10" + bytes(16) + b"\x00\x06", b"W", b"U", b"F", b"UPDATE", b"R\x00",
              b"R\x04\xf0\x00\x00\x00", b"QX", b"\x02\x02", b"PROGRAMX"):
        with pytest.raises(cal_dump.CalError, match="refusing"):
            cal_dump.check_frame(f)
    radio_ = FakeStockRadio(radio, {})
    link = cal_dump.StockLink(radio_)
    with pytest.raises(cal_dump.CalError):
        link.send(b"W\x00\x00\x00\x00\x01\x00\x01\x06")
    assert radio_.sent == []                                  # nothing reached the port


def test_no_write_frame_in_source():
    """One write site to the port, inside send(), right after check_frame; 'W' only appears as the reply check."""
    src = open(cal_dump.__file__).read()
    assert src.count(".write(") == src.count("f.write(") + 1
    send = src[src.index("    def send(self, frame):"):src.index("    def enter(self):")]
    assert send.index("check_frame(frame)") < send.index("self.p.write(frame)")
    assert [ln.strip() for ln in src.splitlines() if 'b"W"' in ln] == [
        'if len(r) == 8 + n and r[:1] == b"W" and r[1:6] == frame[1:] and r[-1] == ACK and \\']


@pytest.mark.parametrize("radio", RADIOS)
def test_dump_a_valid(radio):
    nand, A, B = nand_with(radio)
    r = FakeStockRadio(radio, nand)
    link = cal_dump.StockLink(r, log=lambda s: None, chunk=0x10)
    assert b"UV" in link.enter()
    a, b, rep = cal_dump.dump(link, radio, full=False, log=lambda s: None)
    c = calmap.CAL[radio]
    assert rep["copy_in_use"] == "A" and rep["copy_a_as_served"]["valid"] and not rep["a_equals_b"]
    assert a[:c["crc_span"]] == A[:c["crc_span"]] and b[:c["crc_span"]] == B[:c["crc_span"]]
    assert rep["crc32_in_use"] == "%08x" % calmap.crc(radio, A)
    assert rep["decoded"]["uhf_apc"]["levels"]["low"]["cal"] == 0x40
    assert all(f[:1] in (b"P", b"\x02", b"R", b"E") for f in r.sent)
    link.leave()
    assert r.sent[-1] == b"END"


@pytest.mark.parametrize("radio", RADIOS)
def test_dump_a_invalid_served_from_b(radio):
    nand, A, B = nand_with(radio, a_valid=False)
    link = cal_dump.StockLink(FakeStockRadio(radio, nand), log=lambda s: None)
    link.enter()
    a, b, rep = cal_dump.dump(link, radio, full=False, log=lambda s: None)
    c = calmap.CAL[radio]
    assert rep["copy_in_use"] == "A or B" and rep["a_equals_b"]
    assert a[:c["crc_span"]] == B[:c["crc_span"]]
    assert rep["crc32_in_use"] == "%08x" % calmap.crc(radio, B)


@pytest.mark.parametrize("radio", RADIOS)
def test_dump_none_valid(radio):
    nand, A, B = nand_with(radio, a_valid=False, b_valid=False)
    link = cal_dump.StockLink(FakeStockRadio(radio, nand), log=lambda s: None)
    link.enter()
    _, _, rep = cal_dump.dump(link, radio, full=False, log=lambda s: None)
    assert rep["copy_in_use"] is None and rep["decoded"] is None
    assert "NOT VALID" in cal_dump.summary(rep)


def test_dump_full_block_d168():
    nand, A, B = nand_with("d168")
    link = cal_dump.StockLink(FakeStockRadio("d168", nand), log=lambda s: None, chunk=0xFF)
    link.enter()
    a, b, rep = cal_dump.dump(link, "d168", full=True, log=lambda s: None)
    assert a == bytes(A) and b == bytes(B) and rep["read_size"] == 0x20000


def test_refuses_cfw_and_bad_chunk():
    link = cal_dump.StockLink(FakeStockRadio("d890", {}, ident=b"D890CFW\x00\x01\x00\x01\x00\x01\x80"))
    with pytest.raises(cal_dump.CalError, match="custom firmware"):
        link.enter()
    with pytest.raises(cal_dump.CalError):
        cal_dump.StockLink(FakeStockRadio("d890", {}), chunk=0)


def test_save_and_no_overwrite(tmp_path):
    nand, A, B = nand_with("d890")
    link = cal_dump.StockLink(FakeStockRadio("d890", nand), log=lambda s: None)
    link.enter()
    a, b, rep = cal_dump.dump(link, "d890", full=False, log=lambda s: None)
    stem = cal_dump.save(str(tmp_path), "d890", "SN1", a, b, rep)
    assert json.load(open(stem + ".json"))["copy_in_use"] == "A"
    assert open(stem + ".bin", "rb").read() == a
    with pytest.raises(cal_dump.CalError, match="exists"):
        cal_dump.save(str(tmp_path), "d890", "SN1", a, b, rep)


def test_decode_emulator_synthetic_blocks():
    """The emulator's made-up blocks (emu make_rfcal.py / D168 make_nand.py) decode as the cfw reads them."""
    for radio, path, page in (("d890", os.path.join(hw.SRC, "emu", "images", "rfcal_synthetic.pages"), 0x1000),
                              ("d168", os.path.join(hw.PROJECTS, "D168UV", "emu", "images", "synth.pages"), 0x800)):
        if not os.path.exists(path):
            continue
        c = calmap.CAL[radio]
        d = open(path, "rb").read()
        blk = bytearray(b"\xff" * c["block"])
        for o in range(0, len(d), 4 + page):
            addr = struct.unpack_from("<I", d, o)[0]
            if c["a"] <= addr < c["a"] + c["block"]:
                blk[addr - c["a"]:addr - c["a"] + page] = d[o + 4:o + 4 + page]
        assert calmap.markers(radio, blk)["valid"]
        dec = calmap.decode(radio, blk)
        L = calmap.LAYOUT[radio]
        assert dec["tx_possible"] and dec["uhf_apc"]["levels"]["low"]["cal"] == blk[L["uhf_cal"] + 2]
        assert 0 < dec["uhf_tune_points"]["400 MHz"] < 0x1000


# ------------------------------------------------------------------------------------------------ hw.py calram
def stock_chip(radio_key, tmp_path, monkeypatch, blk):
    r = hw.RADIOS[radio_key]
    chip = FakeChip(r)
    img = bytes(range(256)) * 8
    stock = tmp_path / "stock.bin"
    stock.write_bytes(img)
    monkeypatch.setattr(r, "stock_image", str(stock))
    off = r.app_base - r.flash_base
    chip.flash[off:off + len(img)] = img
    for o, addr, n in calmap.CAL[radio_key]["ram"]:
        chip.write(addr, bytes(blk[o:o + n]))
    return r, chip


@pytest.mark.parametrize("radio", RADIOS)
def test_calram_reads_and_compares(radio, tmp_path, monkeypatch, capsys):
    blk = cal_block(radio)
    r, chip = stock_chip(radio, tmp_path, monkeypatch, blk)
    be, fj = jlink(chip)
    dump = tmp_path / "cal.bin"
    dump.write_bytes(bytes(blk))
    rep = hw.cmd_calram(be, r, Args(compare=str(dump), out=str(tmp_path / "o"), any_firmware=False))
    assert rep["compare"]["differing"] == 0 and rep["decoded"]["uhf_apc"]["levels"]["low"]["cal"] == 0x40
    lo, hi = r.sram
    for s in fj.scripts:                                     # only reads, of SRAM / chip id / app head
        for ln in s:
            assert ln.split()[0] in ("savebin", "exit")
    other = bytearray(blk)
    o = calmap.CAL[radio]["ram"][0][0] + 3
    other[o] ^= 0xFF
    dump.write_bytes(bytes(other))
    with pytest.raises(hw.HwError, match="differs"):
        hw.cmd_calram(be, r, Args(compare=str(dump), out=str(tmp_path / "o"), any_firmware=False))


def test_calram_ignores_loader_patched_bytes(tmp_path, monkeypatch):
    blk = cal_block("d890")
    r, chip = stock_chip("d890", tmp_path, monkeypatch, blk)
    chip.write(0x200151E1 + 6, b"\x01\xf4")                  # FUN_080e0440 forces these after the read
    be, _ = jlink(chip)
    dump = tmp_path / "cal.bin"
    dump.write_bytes(bytes(blk))
    rep = hw.cmd_calram(be, r, Args(compare=str(dump), out=str(tmp_path / "o"), any_firmware=False))
    assert rep["compare"]["differing"] == 0


def test_calram_refuses_non_stock(tmp_path, monkeypatch):
    blk = cal_block("d168")
    r, chip = stock_chip("d168", tmp_path, monkeypatch, blk)
    chip.flash[r.app_base - r.flash_base + 0x10] ^= 1
    be, _ = jlink(chip)
    with pytest.raises(hw.HwError, match="not stock"):
        hw.cmd_calram(be, r, Args(compare=None, out=str(tmp_path / "o"), any_firmware=False))
    hw.cmd_calram(be, r, Args(compare=None, out=str(tmp_path / "o"), any_firmware=True))


def test_calram_regions_in_sram():
    for k in RADIOS:
        lo, hi = hw.RADIOS[k].sram
        for off, addr, n in calmap.CAL[k]["ram"]:
            assert lo <= addr and addr + n <= (0x20018000 if k == "d168" else hi)   # fits even 96 KB mode
            assert off + n <= calmap.CAL[k]["crc_span"]


@pytest.mark.parametrize("radio", RADIOS)
def test_squelch_and_areas_decode(radio):
    blk = cal_block(radio)
    S = calmap.SQUELCH[radio]
    name, base_off, tab_off, ref = S["bands"][0]
    struct.pack_into("<H", blk, base_off, 0x96)
    q = calmap.decode(radio, blk)["squelch"][name]
    assert q["base"] == 0x96 and q["plausible"] and q["reg49_at_ref"]["1"] == 0x96
    assert q["reg49_at_ref"]["5"] == 0x96 + S["level_offsets"][4]
    ar = calmap.areas(radio, blk)
    assert len(ar) == len(calmap.AREAS[radio]) and all(len(x["data"]) == 2 * x["size"] for x in ar)
    for o, _, n in calmap.CAL[radio]["ram"]:                  # every stock loader read is inside a listed area
        assert any(int(x["offset"], 16) <= o and o + n <= int(x["offset"], 16) + x["size"] for x in ar), hex(o)
    for _, _, t, _ in S["bands"]:
        assert any(int(x["offset"], 16) == t for x in ar)


@pytest.mark.parametrize("radio", RADIOS)
def test_dump_reports_areas_and_related(radio):
    nand, A, B = nand_with(radio)
    c = calmap.CAL[radio]
    rel = calmap.RELATED[radio][0][0]
    blk = bytearray(b"\xff" * c["block"]); blk[c["ok_off"]:c["ok_off"] + 4] = calmap.OK_MARK
    nand[rel] = blk
    link = cal_dump.StockLink(FakeStockRadio(radio, nand), log=lambda s: None)
    link.enter()
    _, _, rep = cal_dump.dump(link, radio, full=False, log=lambda s: None)
    assert rep["areas"] and rep["related_blocks"][0]["first"]["valid"]
    r0 = rep["related_blocks"][0]
    assert ("second" not in r0) or not r0["second"]["valid"]
    if radio == "d890":                                           # partial copy at 0x04E80000: compared, not used
        assert "data" in r0 and r0["equals_in_use_prefix"] is False
    txt = cal_dump.summary(rep)
    assert "squelch" in txt and "related 0x" in txt


def test_d890_partial_copy_matches_in_use():
    nand, A, B = nand_with("d890")
    c = calmap.CAL["d890"]
    part = bytearray(b"\xff" * c["block"]); part[:0x800] = A[:0x800]; part[c["ok_off"]:c["ok_off"] + 4] = calmap.OK_MARK
    nand[0x04E80000] = part
    link = cal_dump.StockLink(FakeStockRadio("d890", nand), log=lambda s: None)
    link.enter()
    _, _, rep = cal_dump.dump(link, "d890", full=False, log=lambda s: None)
    r = rep["related_blocks"][0]
    assert r["first"]["addr"] == "0x04E80000" and r["first"]["valid"] and r["equals_in_use_prefix"]
    assert "= copy in use" in cal_dump.summary(rep)
