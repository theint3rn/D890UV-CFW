"""RF / TX calibration block of the stock D890UV and D168UV: where it lives, how the stock firmware loads it, and a
decoder for the values the custom firmware (cfw/drivers/dac.c, D168UV/cfw/drivers/dac.c) uses.

The block sits in the external SPI-NAND in two copies, A and B (B = A + one NAND block). A copy is valid when it
ends with 55 55 AA AA and its "invalid" marker is not AA AA 55 55 (stock FUN_08023ea8 / D168 FUN_08029e28).
Stock loaders: D890 FUN_080e0440 (V1.05), D168 FUN_0807a64c (V1.07). Both read copy A when it is valid, else B
(even an invalid B). The cfw reads A, else a valid B, else nothing (no TX: pre-flash audit F5).

Nothing here talks to a radio; cal_dump.py (PC cable, stock 'R' reads) and hw.py calram (J-Link, RAM) use it.
"""
import struct
import zlib

OK_MARK = b"\x55\x55\xaa\xaa"
BAD_MARK = b"\xaa\xaa\x55\x55"

# Per radio: NAND copies, block size, marker offsets, CRC span (the bytes the cfw checksums in its RTT boot line),
# and the stock loader's (offset in block, RAM address, length) list for the stock firmware version named.
CAL = {
    "d890": dict(
        name="D890UV", a=0x04F00000, b=0x04F40000, block=0x40000, ok_off=0x3FFFC, bad_off=0x3FDF8,
        crc_span=0x2000, stock="V1.05", loader="FUN_080e0440",
        ram=[(0x000, 0x200152AC, 0x2A), (0x100, 0x200152D6, 0x2C), (0x200, 0x20013484, 10),
             (0x400, 0x200151E1, 0x27), (0x500, 0x20015208, 0x27), (0x300, 0x200151AE, 0x21),
             (0x600, 0x20014EEE, 8), (0x700, 0x200151CF, 0x12), (0x800, 0x2001AE3C, 0xC), (0x900, 0x2001AE56, 0xC),
             (0xA00, 0x2001AE48, 0xC), (0xB00, 0x2001AE62, 0xC),
             (0xC00, 0x2000C178, 0x79), (0xD00, 0x2000C02C, 0x79), (0xE00, 0x2000C0D2, 0x79),
             (0xF00, 0x2000BF86, 0x79),
             (0x1000, 0x2000C1F1, 0x2D), (0x1100, 0x2000C0A5, 0x2D), (0x1200, 0x2000C14B, 0x2D),
             (0x1300, 0x2000BFFF, 0x2D),
             (0x1400, 0x20001D02, 0x19), (0x1500, 0x20001D1B, 0x19), (0x1600, 0x20003056, 0x19),
             (0x1700, 0x2000306F, 0x19), (0x1800, 0x20015302, 0x1C), (0x1C00, 0x2001446E, 0xE)],
        # bytes the loader changes after reading (offset in block): +0x28 zeroed if > 0x12, +0x406/7 and +0x506/7
        # forced to 01 F4
        ram_patched={0x28, 0x406, 0x407, 0x506, 0x507}),
    "d168": dict(
        name="D168UV", a=0x02F60000, b=0x02F80000, block=0x20000, ok_off=0x1FFFC, bad_off=0x1FFF8,
        crc_span=0x800, stock="V1.07", loader="FUN_0807a64c",
        ram=[(0x000, 0x2000CC28, 0x20), (0x040, 0x2000CC48, 0x20), (0x100, 0x2000CC77, 0x27),
             (0x140, 0x2000CC9E, 0x27), (0x080, 0x2000CC68, 0xF), (0x0C0, 0x2000D00F, 0x12),
             (0x180, 0x20001560, 8), (0x200, 0x2000CCC5, 0xC), (0x220, 0x2000CCD1, 0xC), (0x240, 0x2000CCDD, 0xC),
             (0x260, 0x2000CCE9, 0xC),
             (0x300, 0x2000CD62, 0x79), (0x380, 0x2000CDDB, 0x79), (0x400, 0x2000CE54, 0x79),
             (0x480, 0x2000CECD, 0x79),
             (0x500, 0x2000CF46, 0x27), (0x540, 0x2000CF6D, 0x27), (0x580, 0x2000CF94, 0x27),
             (0x5C0, 0x2000CFBB, 0x27),
             (0x620, 0x2000CFF6, 0x19), (0x600, 0x2000CFE2, 0x14), (0x640, 0x2000D054, 0x18)],
        ram_patched={0x1D}),            # zeroed if > 0x11
}

# APC / tune layout used by the cfw (dac.c), per radio
LAYOUT = {
    "d890": dict(uhf_cal=0x000, vhf_cal=0x100, uhf_tune=(0x18, (400, 460, 520)), vhf_tune=(0x11E, (136, 174, 225)),
                 uhf_tab=(0xC00, 0x100, 0x79, 0x28), vhf_tab=(0x1000, 0x100, 0x2D, 9)),
    "d168": dict(uhf_cal=0x000, vhf_cal=0x040, uhf_tune=(0x12, (400, 440, 480)), vhf_tune=(0x52, (136, 155, 174)),
                 uhf_tab=(0x300, 0x80, 0x79, 0x28), vhf_tab=(0x500, 0x40, 0x27, 9), vhf_200_300=0x5E),
}
LEVELS = ("low", "mid", "high", "turbo")

# Every sub-area the stock loader reads from the calibration block: (offset, size, kind, role, cfw use).
# kind: "tx" / "rx" / "?" (stock users known, meaning not identified). Roles come from the stock code named.
AREAS = {
    "d890": [
        (0x000, 0x2A, "tx+rx", "UHF struct: APC cal per level +0x02..+0x05 (FUN_0811a75c), squelch base u16 +0x10 "
         "(RF chip 0) / +0x12 (chip 1) (FUN_080e373c -> reg 0x49), RX tune points u16 +0x18/+0x1A/+0x1C "
         "400/460/520 MHz (FUN_08049fc4); +0x28 = a mode byte the loader zeroes if > 0x12", "APC, tune: used; squelch: NOT used"),
        (0x100, 0x2C, "tx+rx", "VHF struct: APC cal +0x102..+0x105, squelch base u16 +0x116 (chip 0) / +0x118 (chip 1),"
         " 200-300 MHz squelch base +0x124 / +0x126, RX tune points +0x11E/+0x120/+0x122 136/174/225 MHz "
         "(FUN_0804a0a0, VHF tune goes to RF reg 0x5C)", "APC, tune: used; squelch: NOT used"),
        (0x200, 0x0A, "?", "RAM 0x20013484 (many users)", "not used"),
        (0x300, 0x21, "?", "RAM 0x200151AE (FUN_0803f208, FUN_080613a0, FUN_08117b3c)", "not used"),
        (0x400, 0x27, "?", "RAM 0x200151E1; floating-point pair math in FUN_0803c020 (+0x0A/+0x0E u16 pairs); "
         "loader forces +0x06/+0x07 = 01 F4", "not used"),
        (0x500, 0x27, "?", "RAM 0x20015208, as +0x400 for the other band", "not used"),
        (0x600, 0x08, "?", "RAM 0x20014EEE (FUN_080e4370, FUN_0811a588)", "not used"),
        (0x700, 0x12, "?", "RAM 0x200151CF (FUN_0803c020, TX path FUN_08117b3c..)", "not used"),
        (0x800, 0x0C, "?", "RAM 0x2001AE3C", "not used"), (0x900, 0x0C, "?", "RAM 0x2001AE56", "not used"),
        (0xA00, 0x0C, "?", "RAM 0x2001AE48", "not used"), (0xB00, 0x0C, "?", "RAM 0x2001AE62", "not used"),
        (0xC00, 0x400, "tx", "UHF APC tables, 4 levels x 0x100, 121 bytes each, 1 MHz steps 400-520 MHz, ref index 40",
         "used"),
        (0x1000, 0x400, "tx", "VHF APC tables, 4 levels x 0x100, 45 bytes (136-174 MHz, 220-225 MHz from index 39), "
         "ref index 9", "used"),
        (0x1400, 0x19, "rx", "squelch frequency correction, UHF, RF chip 0: 25 bytes, 5 MHz steps from 400 MHz, ref "
         "index 8 (440 MHz) (FUN_080e373c)", "NOT used"),
        (0x1500, 0x19, "rx", "squelch frequency correction, VHF, chip 0: 2 MHz steps from 136 MHz, ref index 5 (146 MHz);"
         " index 20 = 200-300 MHz value", "NOT used"),
        (0x1600, 0x19, "rx", "as +0x1400 for RF chip 1", "NOT used (cfw drives chip 0 only)"),
        (0x1700, 0x19, "rx", "as +0x1500 for RF chip 1", "NOT used"),
        (0x1800, 0x1C, "?", "RAM 0x20015302 (FUN_0803c020, FUN_08127794)", "not used"),
        (0x1C00, 0x0E, "?", "RAM 0x2001446E", "not used"),
    ],
    "d168": [
        (0x000, 0x20, "tx+rx", "UHF struct: APC cal +0x02..+0x05 (FUN_0808ca20), squelch base u16 +0x10 "
         "(FUN_0807bf6c -> reg 0x49), RX tune points +0x12/+0x14/+0x16 400/440/480 MHz (FUN_08039030); +0x1D = a "
         "byte the loader zeroes if > 0x11", "APC, tune: used; squelch: NOT used"),
        (0x040, 0x20, "tx+rx", "VHF struct: APC cal +0x42..+0x45, squelch base u16 +0x50, RX tune points "
         "+0x52/+0x54/+0x56 136/155/174 MHz (FUN_080390e0), +0x5E = 200-300 MHz APC value", "APC, tune: used; "
         "squelch: NOT used"),
        (0x080, 0x0F, "?", "RAM 0x2000CC68 (FUN_0800ed04, FUN_0801157c)", "not used"),
        (0x0C0, 0x12, "?", "RAM 0x2000D00F (FUN_0801136c, FUN_08055b70)", "not used"),
        (0x100, 0x27, "?", "RAM 0x2000CC77, 39 bytes = one per MHz 136-174 (FUN_0800f188, FUN_08039908)", "not used"),
        (0x140, 0x27, "?", "RAM 0x2000CC9E, as +0x100", "not used"),
        (0x180, 0x08, "?", "RAM 0x20001560 (FUN_08010e98, FUN_0803ea7c)", "not used"),
        (0x200, 0x6C, "?", "four 12-byte records at +0x200/+0x220/+0x240/+0x260, RAM 0x2000CCC5.. (FUN_0809127c)", "not used"),
        (0x300, 0x200, "tx", "UHF APC tables, 4 levels x 0x80, 121 bytes, 1 MHz steps 400-520 MHz, ref index 40", "used"),
        (0x500, 0x100, "tx", "VHF APC tables, 4 levels x 0x40, 39 bytes 136-174 MHz, ref index 9", "used"),
        (0x600, 0x14, "rx", "squelch frequency correction, VHF: 20 bytes, 2 MHz steps from 136 MHz, ref index 10 "
         "(156 MHz) (FUN_0807bf6c)", "NOT used"),
        (0x620, 0x19, "rx", "squelch frequency correction, UHF: 25 bytes, 5 MHz steps from 400 MHz, ref index 8 "
         "(440 MHz)", "NOT used"),
        (0x640, 0x18, "?", "RAM 0x2000D054 (FUN_08097a30, FUN_08098158)", "not used"),
    ],
}

# Squelch (stock): reg 0x49 threshold = base + level offset + table[ref] - table[f] (- a temperature term).
SQUELCH = {
    "d890": dict(level_offsets=(0, 7, 13, 19, 23), bands=[
        ("UHF chip 0", 0x10, 0x1400, 8), ("VHF chip 0", 0x116, 0x1500, 5), ("200-300 MHz chip 0", 0x124, 0x1500, 5),
        ("UHF chip 1", 0x12, 0x1600, 8), ("VHF chip 1", 0x118, 0x1700, 5), ("200-300 MHz chip 1", 0x126, 0x1700, 5)]),
    "d168": dict(level_offsets=(0, 4, 8, 12, 16), bands=[("UHF", 0x10, 0x620, 8), ("VHF", 0x50, 0x600, 10)]),
}

# Other NAND blocks near the calibration that the stock code reads or writes: (first, second or None, role, bytes
# to dump for comparison). Not used by the cfw.
RELATED = {
    "d890": [(0x04E80000, None, "partial calibration copy: on END after a CPS write session the stock copies the "
              "first 0x800 bytes of the copy in use here (FUN_0812ed60 -> FUN_0813a3c8)", 0x800),
             (0x03480000, 0x034C0000, "settings written by the stock CPS (in the CPS codeplug capture, absent from "
              "the factory-formatted image), loader FUN_080e0750", 0),
             (0x04F80000, 0x04FC0000, "radio stun / kill state (FUN_0813a064), written on remote stun commands", 0)],
    "d168": [(0x02FA0000, 0x02FC0000, "runtime state the radio itself erases and rewrites (0x200 bytes from RAM "
              "0x2000E317, defaults in FUN_0803f3f8), not factory data", 0)],
}


def markers(radio, blk):
    """Marker state of one copy (blk = the whole block, or at least up to ok_off + 4)."""
    c = CAL[radio]
    ok = bytes(blk[c["ok_off"]:c["ok_off"] + 4])
    bad = bytes(blk[c["bad_off"]:c["bad_off"] + 4])
    return dict(ok_marker=ok.hex(" "), invalid_marker=bad.hex(" "), valid=ok == OK_MARK and bad != BAD_MARK)


def crc(radio, blk):
    return zlib.crc32(bytes(blk[:CAL[radio]["crc_span"]])) & 0xFFFFFFFF


def _band(blk, cal_off, tab):
    base, stride, n, ref = tab
    cal = list(blk[cal_off + 2:cal_off + 6])
    tabs = [list(blk[base + stride * l:base + stride * l + n]) for l in range(4)]
    ff = all(v == 0xFF for t in tabs for v in t)
    zero = all(v == 0 for t in tabs for v in t)
    mx = max([cal[l] + t[i] - t[ref] for l, t in enumerate(tabs) for i in range(n)] + [0])
    levels = {}
    for l, t in enumerate(tabs):
        levels[LEVELS[l]] = dict(cal=cal[l], at_ref=max(0, min(cal[l], mx)), table_min=min(t), table_max=max(t),
                                 table_ref=t[ref])
    return dict(usable=not ff and not zero and mx > 0, apc_max=min(mx, 0xFF), levels=levels, ref_index=ref,
                table=[bytes(t).hex() for t in tabs])


def decode(radio, blk):
    """Values the cfw takes from one copy (stock-compatible layout, see dac.c)."""
    L = LAYOUT[radio]
    u16 = lambda o: struct.unpack_from("<H", bytes(blk), o)[0]
    out = dict(radio=CAL[radio]["name"], crc32="%08x" % crc(radio, blk), crc_span=CAL[radio]["crc_span"])
    to, mhz = L["uhf_tune"]
    out["uhf_tune_points"] = {"%d MHz" % f: u16(to + 2 * i) for i, f in enumerate(mhz)}
    to, mhz = L["vhf_tune"]
    out["vhf_tune_points"] = {"%d MHz" % f: u16(to + 2 * i) for i, f in enumerate(mhz)}
    out["uhf_apc"] = _band(blk, L["uhf_cal"], L["uhf_tab"])
    out["vhf_apc"] = _band(blk, L["vhf_cal"], L["vhf_tab"])
    if "vhf_200_300" in L:
        out["apc_200_300MHz"] = blk[L["vhf_200_300"]]
    out["tx_possible"] = out["uhf_apc"]["usable"] or out["vhf_apc"]["usable"]
    out["squelch"] = decode_squelch(radio, blk)
    return out


def decode_squelch(radio, blk):
    """Stock squelch calibration: base threshold per band (and RF chip on the D890), the per-frequency correction
    table, and the reg 0x49 threshold at the table's reference frequency for squelch levels 1..5."""
    S = SQUELCH[radio]
    out = {}
    for name, base_off, tab_off, ref in S["bands"]:
        base = struct.unpack_from("<H", bytes(blk), base_off)[0]
        n = 0x14 if (radio == "d168" and tab_off == 0x600) else 0x19
        tab = list(blk[tab_off:tab_off + n])
        out[name] = dict(base=base, base_offset="0x%x" % base_off, table_offset="0x%x" % tab_off, ref_index=ref,
                         table=bytes(tab).hex(), plausible=base not in (0, 0xFFFF) and any(t != 0xFF for t in tab),
                         reg49_at_ref={str(l + 1): base + o for l, o in enumerate(S["level_offsets"])})
    return out


def areas(radio, blk):
    """Every stock sub-area: offset, size, kind, role, cfw use, data."""
    return [dict(offset="0x%04x" % o, size=n, kind=k, role=r, cfw=u, data=bytes(blk[o:o + n]).hex())
            for o, n, k, r, u in AREAS[radio]]


def choose(radio, a_blk, b_blk):
    """Which copy the stock firmware / the cfw use. Returns dict(stock, cfw) with 'A' / 'B' / None (cfw)."""
    va, vb = markers(radio, a_blk)["valid"], markers(radio, b_blk)["valid"] if b_blk is not None else False
    return dict(stock="A" if va else "B", cfw="A" if va else ("B" if vb else None))
