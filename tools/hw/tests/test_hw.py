"""Offline tests for tools/hw/hw.py: no probe, no J-Link / OpenOCD install needed.

A FakeChip stands in for the radio's MCU. The J-Link backend runs against a fake JLinkExe (interprets the generated
command file: savebin / w4 / loadfile / verifybin / r / h / g); the OpenOCD backend talks to a fake TCL server.
So command building, address guards, backup / flash flows and RTT parsing are all exercised for both radios.
Run: python3 -m pytest tools/hw/tests -q
"""
import json
import os
import re
import socket
import struct
import sys
import threading

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hw  # noqa: E402

D890, D168 = hw.RADIOS["d890"], hw.RADIOS["d168"]


# ------------------------------------------------------------------------------------------------ fake chip
class FakeChip:
    def __init__(self, radio, prot=None, uid=None, boot_fill=0x5A):
        self.radio = radio
        self.regions = []
        self.flash = bytearray(b"\xff" * radio.flash_size)
        boot = radio.app_base - radio.flash_base
        self.flash[:boot] = bytes((boot_fill + i) & 0xFF for i in range(boot))
        self.add(radio.flash_base, self.flash)
        lo, hi = radio.sram
        self.ram = bytearray(hi - lo)
        self.add(lo, self.ram)
        cpuid = 0x410FD212 if radio.key == "d890" else 0x410FC241
        self.scb = bytearray(0x40)
        struct.pack_into("<I", self.scb, 0, cpuid)
        self.add(0xE000ED00, self.scb)
        self.word(radio.idcode_addr, 0x10016482 if radio.key == "d890" else 0x70050242)
        self.word(radio.flash_kb_addr, radio.flash_size // 1024)
        self.add(radio.uid_addr, bytearray(uid or bytes(range(0x30, 0x3C))))
        self.prot = bytearray(struct.pack("<I", prot if prot is not None else
                                          (0x1FEFF8AA if radio.key == "d890" else 0x00000000)))
        self.add(radio.prot_addr, self.prot)
        for _, addr, size in radio.extra_dumps:
            if not self.find(addr, size):
                self.add(addr, bytearray(range(size)) if size < 256 else bytearray(size))
        if radio.eopb0_addr:                                   # factory D168: EOPB0 0xFE (224 KB), complement 0x01
            self.write(radio.eopb0_addr, b"\xfe\x01")
        self.halted = False
        self.programs = []
        self.corrupt_program = False

    def add(self, addr, buf):
        self.regions.append((addr, buf))

    def word(self, addr, v):
        self.add(addr, bytearray(struct.pack("<I", v)))

    def find(self, addr, size):
        for base, buf in self.regions:
            if base <= addr and addr + size <= base + len(buf):
                return base, buf
        return None

    def read(self, addr, size):
        f = self.find(addr, size)
        if not f:
            raise IOError("bus fault 0x%08x" % addr)
        base, buf = f
        return bytes(buf[addr - base:addr - base + size])

    def write(self, addr, data):
        base, buf = self.find(addr, len(data))
        buf[addr - base:addr - base + len(data)] = data

    def program(self, addr, data):
        assert addr >= self.radio.app_base, "FAKE CHIP: bootloader write attempted"
        self.programs.append((addr, len(data)))
        if self.corrupt_program:
            data = data[:-1] + bytes([data[-1] ^ 1])
        self.write(addr, data)


# ------------------------------------------------------------------------------------------------ fake JLinkExe
class FakeJLink:
    def __init__(self, chip):
        self.chip = chip
        self.argvs = []
        self.scripts = []

    def __call__(self, argv):
        self.argvs.append(argv)
        path = argv[argv.index("-CommandFile") + 1]
        lines = [ln.strip() for ln in open(path) if ln.strip()]
        self.scripts.append(lines)
        out = []
        for ln in lines:
            w = ln.split()
            c = w[0].lower()
            if c == "savebin":
                with open(w[1], "wb") as f:
                    f.write(self.chip.read(int(w[2], 16), int(w[3], 16)))
            elif c == "w4":
                self.chip.write(int(w[1], 16), struct.pack("<I", int(w[2], 16)))
            elif c == "loadfile":
                self.chip.program(int(w[2], 16), open(w[1], "rb").read())
            elif c == "verifybin":
                data = open(w[1], "rb").read()
                ok = self.chip.read(int(w[2], 16), len(data)) == data
                out.append("Verify successful." if ok else "Verify failed.")
            elif c in ("r", "h"):
                self.chip.halted = True
            elif c == "g":
                self.chip.halted = False
            elif c == "regs":
                out.append("PC = 0800C2F8, CycleCnt = 00000000\nR13(SP)= 200BFFB8, R14(LR) = 08010729")
            elif c == "exit":
                break
            else:
                return 1, "Unknown command " + ln
        return 0, "\n".join(out)


def jlink(chip):
    fj = FakeJLink(chip)
    return hw.JLinkBackend(chip.radio, runner=fj, exe="JLinkExe"), fj


@pytest.fixture(autouse=True)
def backups(tmp_path, monkeypatch):
    monkeypatch.setattr(hw, "BACKUP_ROOT", str(tmp_path / "hw_backups"))
    return tmp_path


class Args:
    def __init__(self, **kw):
        self.dir = None
        self.probe = "jlink"
        self.elf = None
        self.__dict__.update(kw)


def app_image(radio, size=0x3000, sp=None):
    data = bytearray(os.urandom(size))
    struct.pack_into("<II", data, 0, sp or radio.sram[0] + 0x8000, radio.app_base + 0x101)
    return bytes(data)


def yes(prompt, phrase):
    yes.last = (prompt, phrase)


# ------------------------------------------------------------------------------------------------ guards
@pytest.mark.parametrize("radio", [D890, D168])
def test_write_range_guards(radio):
    hw.check_write_range(radio, radio.app_base, 0x1000)
    hw.check_write_range(radio, radio.app_base, radio.write_end - radio.app_base)
    for addr in (radio.flash_base, radio.app_base - 0x800, radio.app_base - 4):
        with pytest.raises(hw.HwError, match="bootloader"):
            hw.check_write_range(radio, addr, 0x10)
    with pytest.raises(hw.HwError, match="app base"):
        hw.check_write_range(radio, radio.app_base + 0x800, 0x10)
    with pytest.raises(hw.HwError, match="past"):
        hw.check_write_range(radio, radio.app_base, radio.write_end - radio.app_base + 1)
    with pytest.raises(hw.HwError):
        hw.check_write_range(radio, radio.app_base, 0)


def test_windows():
    assert (D890.app_base, D890.write_end) == (0x0800C000, 0x081E4000)       # table cache above stays untouched
    assert (D168.app_base, D168.write_end) == (0x08004000, 0x08100000)
    assert D890.app_base - D890.flash_base == 0xC000 and D168.app_base - D168.flash_base == 0x4000


@pytest.mark.parametrize("radio", [D890, D168])
def test_image_checks(radio):
    hw.check_image(radio, app_image(radio), radio.app_base)
    bad_sp = bytearray(app_image(radio)); struct.pack_into("<I", bad_sp, 0, 0x08001000)
    with pytest.raises(hw.HwError, match="SP"):
        hw.check_image(radio, bytes(bad_sp), radio.app_base)
    bad_rst = bytearray(app_image(radio)); struct.pack_into("<I", bad_rst, 4, radio.app_base + 0x100)   # not thumb
    with pytest.raises(hw.HwError, match="reset"):
        hw.check_image(radio, bytes(bad_rst), radio.app_base)
    out = bytearray(app_image(radio)); struct.pack_into("<I", out, 4, 0x08000101)                       # bootloader
    with pytest.raises(hw.HwError, match="reset"):
        hw.check_image(radio, bytes(out), radio.app_base)


def test_d890_image_rejected_on_d168():
    with pytest.raises(hw.HwError):          # D890 vector table (app 0x0800C000) points outside a D168 image base
        hw.check_image(D168, app_image(D890), D168.app_base)


@pytest.mark.parametrize("radio", [D890, D168])
def test_stock_images_pass_guards(radio):
    if not os.path.exists(radio.stock_image):
        pytest.skip("stock image not present")
    data = open(radio.stock_image, "rb").read()
    hw.check_write_range(radio, radio.app_base, len(data))
    hw.check_image(radio, data, radio.app_base)


def test_cfw_images_pass_guards():
    for radio, b in ((D890, os.path.join(hw.SRC, "cfw", "build", "cfw.bin")),
                     (D168, os.path.join(hw.PROJECTS, "D168UV", "cfw", "build", "cfw168.bin"))):
        if os.path.exists(b):
            data = open(b, "rb").read()
            hw.check_write_range(radio, radio.app_base, len(data))
            hw.check_image(radio, data, radio.app_base)


def test_ram_write_guard():
    hw.check_ram_write(D168, 0x20000100)
    for a in (0x08004000, 0x20038000, 0x20000102, 0x40022000, 0x1FFFF800):
        with pytest.raises(hw.HwError):
            hw.check_ram_write(D168, a)


def test_protection_decode():
    assert hw.protection_from_word(D890, 0x1FEFF8AA)["writable"]
    p = hw.protection_from_word(D890, 0x1FEFF855)
    assert p["readable"] and not p["writable"]
    for w in (0x1FEFF8BB, 0x1FEFF800):
        p = hw.protection_from_word(D890, w)
        assert not p["readable"] and not p["writable"] and p["level"] == "RDP1"
    assert hw.protection_from_word(D890, 0x1FEFF8CC)["level"] == "RDP2"
    assert hw.protection_from_word(D168, 0)["writable"]
    p = hw.protection_from_word(D168, 2)
    assert not p["readable"] and not p["writable"]


# ------------------------------------------------------------------------------------------------ J-Link commands
def test_jlink_argv_and_scripts(tmp_path):
    chip = FakeChip(D168)
    be, fj = jlink(chip)
    be.serial = "123456"
    be.read(0x08000000, 16)
    a = fj.argvs[-1]
    assert a[0] == "JLinkExe" and "-NoGui" not in a           # dialogs visible unless --nogui
    assert a[a.index("-Device") + 1] == "AT32F403AVGT7"
    assert a[a.index("-If") + 1] == "SWD" and a[a.index("-USB") + 1] == "123456"
    assert a[a.index("-AutoConnect") + 1] == "1" and a[a.index("-ExitOnError") + 1] == "1"
    assert re.match(r"savebin \S+ 0x08000000 0x10$", fj.scripts[-1][0]) and fj.scripts[-1][-1] == "exit"
    d890, fj2 = jlink(FakeChip(D890))
    d890.read(0x08000000, 4)
    assert fj2.argvs[-1][fj2.argvs[-1].index("-Device") + 1] == "STM32U575ZI"


@pytest.mark.parametrize("line", ["erase", "Erase 0x08000000 0x08004000", "unlock", "exec SetResetType = 12",
                                  "loadbin x.bin 0x08000000", "w4 0x40022040 0x000000BB", "w4 0x1FFFF800 0xA5",
                                  "w1 0x08000000 00", "RSetType 2", "wreg R15 0"])
def test_jlink_line_guard(line):
    be, fj = jlink(FakeChip(D168))
    with pytest.raises(hw.HwError):
        be.run([line])
    assert fj.argvs == []                      # nothing reached the probe


def test_jlink_loadfile_guard(tmp_path):
    be, fj = jlink(FakeChip(D890))
    p = tmp_path / "x.bin"
    p.write_bytes(b"\0" * 64)
    with pytest.raises(hw.HwError, match="bootloader"):
        be.run(["loadfile %s 0x08000000" % p])
    with pytest.raises(hw.HwError):
        be.program(str(p), 0x0800A000)        # activation key page
    assert fj.argvs == []


def test_jlink_failure_raises():
    be = hw.JLinkBackend(D890, runner=lambda argv: (1, "Cannot connect to target."))
    with pytest.raises(hw.HwError, match="connect"):
        be.read(0x08000000, 4)


# ------------------------------------------------------------------------------------------------ flows
@pytest.mark.parametrize("radio", [D890, D168])
def test_info_and_wrong_radio(radio, capsys):
    chip = FakeChip(radio)
    be, _ = jlink(chip)
    hw.cmd_info(be, radio, Args())
    out = capsys.readouterr().out
    assert chip.radio.name in out and "303132333435363738393a3b" in out and "PC=0x0800C2F8" in out
    other = D168 if radio is D890 else D890
    with pytest.raises(hw.HwError, match="wrong --radio"):
        hw.chip_info(be, other)


@pytest.mark.parametrize("radio", [D890, D168])
def test_backup_then_flash_then_recover_shape(radio, capsys):
    chip = FakeChip(radio)
    be, fj = jlink(chip)
    img = app_image(radio)
    with pytest.raises(hw.HwError, match="no verified backup"):
        hw.write_app(be, radio, img, "t", input_fn=yes)
    assert chip.programs == []
    hw.cmd_backup(be, radio, Args())
    found = hw.find_backup(radio, chip.read(radio.uid_addr, 12).hex())
    assert found
    d, man = found
    assert man["verified"] and man["read_twice_identical"] and man["bootloader_present"]
    assert open(os.path.join(d, "flash_full.bin"), "rb").read() == bytes(chip.flash)
    for name, _, size in radio.extra_dumps:
        assert os.path.getsize(os.path.join(d, name)) == size
    hw.cmd_verify_backup(be, radio, Args())
    hw.write_app(be, radio, img, "t", input_fn=yes)
    assert yes.last[1] == "FLASH 38393a3b"                      # last 4 UID bytes
    assert chip.programs == [(radio.app_base, len(img))]
    assert bytes(chip.flash[radio.app_base - radio.flash_base:][:len(img)]) == img
    assert any(ln.startswith("loadfile") for s in fj.scripts for ln in s)
    assert not chip.halted                                      # reset + go at the end


def test_flash_refuses_protected_chip():
    chip = FakeChip(D890)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D890, Args())
    chip.prot[0] = 0x55                                        # RDP 0.5 now
    with pytest.raises(hw.HwError, match="protection"):
        hw.write_app(be, D890, app_image(D890), "t", input_fn=yes)
    assert chip.programs == []


def test_backup_refuses_fap():
    chip = FakeChip(D168, prot=2)
    be, _ = jlink(chip)
    with pytest.raises(hw.HwError, match="lower protection"):
        hw.cmd_backup(be, D168, Args())


def test_backup_of_other_chip_does_not_count():
    a = FakeChip(D168)
    be, _ = jlink(a)
    hw.cmd_backup(be, D168, Args())
    b = FakeChip(D168, uid=bytes(range(0x40, 0x4C)))
    be2, _ = jlink(b)
    with pytest.raises(hw.HwError, match="no verified backup"):
        hw.write_app(be2, D168, app_image(D168), "t", input_fn=yes)


def test_bootloader_change_since_backup_stops():
    chip = FakeChip(D168)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D168, Args())
    chip.flash[0x100] ^= 0xFF
    with pytest.raises(hw.HwError, match="bootloader"):
        hw.write_app(be, D168, app_image(D168), "t", input_fn=yes)
    assert chip.programs == []


def test_confirmation_required(monkeypatch):
    chip = FakeChip(D890)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D890, Args())
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH")    # wrong phrase (no UID tag)
    with pytest.raises(hw.HwError, match="aborted"):
        hw.write_app(be, D890, app_image(D890), "t")
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH 38393a3b")
    hw.write_app(be, D890, app_image(D890), "t")
    assert len(chip.programs) == 1


def test_readback_mismatch_reported():
    chip = FakeChip(D168)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D168, Args())
    chip.corrupt_program = True
    with pytest.raises(hw.HwError, match="recover"):
        hw.write_app(be, D168, app_image(D168), "t", input_fn=yes)


def test_protection_change_after_write_reported(monkeypatch):
    chip = FakeChip(D890)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D890, Args())
    orig = chip.program

    def evil(addr, data):
        orig(addr, data)
        chip.prot[0] = 0xBB
    chip.program = evil
    with pytest.raises(hw.HwError, match="PROTECTION CHANGED"):
        hw.write_app(be, D890, app_image(D890), "t", input_fn=yes)


def test_unverified_backup_blank_bootloader():
    chip = FakeChip(D168)
    chip.flash[:0x4000] = b"\xff" * 0x4000
    be, _ = jlink(chip)
    with pytest.raises(hw.HwError, match="not verified"):
        hw.cmd_backup(be, D168, Args())
    assert hw.find_backup(D168, chip.read(D168.uid_addr, 12).hex()) is None


# ------------------------------------------------------------------------------------------------ RTT
# control block + buffer captured from `make DEBUG=1` D890 cfw in the Renode emulator (sysbus ReadBytes)
EMU_CB = bytes.fromhex("534547474552205254540000000000000100000001000000359402082cb80020000800001000000000000000"
                       "00000000359402080000000000000000000000000000000000000000")
EMU_BUF = b"cfw D890UV boot\n"


def test_rtt_from_emulator_capture():
    ups = hw.rtt_parse_cb(EMU_CB)
    assert ups[0]["buf"] == 0x2000B82C and ups[0]["size"] == 0x800 and ups[0]["wr"] == 16 and ups[0]["rd"] == 0
    chip = FakeChip(D890)
    chip.write(0x2000C030, EMU_CB)
    chip.write(0x2000B82C, EMU_BUF)
    be, _ = jlink(chip)
    assert hw.rtt_find(be, D890) == 0x2000C030                 # RAM scan
    assert hw.rtt_poll(be, D890, 0x2000C030) == EMU_BUF
    assert struct.unpack_from("<I", chip.read(0x2000C030 + 24 + 16, 4))[0] == 16   # RdOff advanced
    assert hw.rtt_poll(be, D890, 0x2000C030) == b""


def test_rtt_wraparound_d168():
    chip = FakeChip(D168)
    cb, buf, size = 0x20008480, 0x20001000, 32
    blk = bytearray(b"SEGGER RTT\0\0\0\0\0\0" + struct.pack("<ii", 1, 1))
    blk += struct.pack("<6I", 0x08001234, buf, size, 6, 28, 0) + bytes(24)
    chip.write(cb, bytes(blk))
    chip.write(buf, b"o\n" + b"xxxx" + bytes(22) + b"hell")
    be, _ = jlink(chip)
    assert hw.rtt_poll(be, D168, cb) == b"hello\n"[:4] + b"o\n" + b"xxxx"
    with pytest.raises(hw.HwError):
        hw.rtt_parse_cb(b"\0" * 72)


def test_rtt_rejects_buffer_outside_sram():
    chip = FakeChip(D168)
    blk = b"SEGGER RTT\0\0\0\0\0\0" + struct.pack("<ii", 1, 0) + struct.pack("<6I", 0, 0x08004000, 64, 1, 0, 0)
    chip.write(0x20000000, blk + bytes(48))
    be, _ = jlink(chip)
    with pytest.raises(hw.HwError, match="SRAM"):
        hw.rtt_poll(be, D168, 0x20000000)


# ------------------------------------------------------------------------------------------------ OpenOCD backend
class FakeOpenOCD(threading.Thread):
    def __init__(self, chip, banks):
        super().__init__(daemon=True)
        self.chip, self.banks, self.cmds = chip, banks, []
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]

    def handle(self, c):
        w = c.split()
        if w[0] == "read_memory":
            a, n = int(w[1], 0), int(w[3])
            return " ".join("0x%02x" % b for b in self.chip.read(a, n))
        if w[0] == "dump_image":
            open(w[1], "wb").write(self.chip.read(int(w[2], 0), int(w[3], 0)))
            return "dumped"
        if w[0] == "write_memory":
            v = int(c.split("{")[1].rstrip("}"), 0)
            self.chip.write(int(w[1], 0), struct.pack("<I", v))
            return ""
        if c == "flash banks":
            return self.banks
        if w[:3] == ["flash", "write_image", "erase"]:
            self.chip.program(int(w[4], 0), open(w[3], "rb").read())
            return "wrote"
        if w[0] == "verify_image":
            data = open(w[1], "rb").read()
            return "verified %d bytes" % len(data) if self.chip.read(int(w[2], 0), len(data)) == data else "mismatch"
        if w[0] == "reg":
            return "%s (/32): 0x0800c2f8" % w[1]
        return ""

    def run(self):
        s, _ = self.srv.accept()
        buf = b""
        while True:
            d = s.recv(65536)
            if not d:
                return
            buf += d
            while b"\x1a" in buf:
                c, buf = buf.split(b"\x1a", 1)
                self.cmds.append(c.decode())
                s.sendall(self.handle(c.decode()).encode() + b"\x1a")


def test_openocd_backend_flow():
    chip = FakeChip(D890)
    srv = FakeOpenOCD(chip, "#0 : stm32l4x at 0x08000000, size 0x00200000, buswidth 0, chipwidth 0")
    srv.start()
    be = hw.OpenOCDBackend(D890, port=srv.port)
    hw.cmd_backup(be, D890, Args(probe="stlink"))
    img = app_image(D890)
    hw.write_app(be, D890, img, "t", input_fn=yes)
    assert chip.programs == [(D890.app_base, len(img))]
    assert any(c.startswith("flash write_image erase") and c.endswith("0x0800c000 bin") for c in srv.cmds)
    assert not any("option" in c or "mass_erase" in c or "unlock" in c for c in srv.cmds)


def test_openocd_d168_without_driver_refuses_write():
    chip = FakeChip(D168)
    srv = FakeOpenOCD(chip, "")
    srv.start()
    be = hw.OpenOCDBackend(D168, port=srv.port)
    hw.cmd_backup(be, D168, Args(probe="stlink"))              # reads work without a flash driver
    with pytest.raises(hw.HwError, match="no flash bank"):
        hw.write_app(be, D168, app_image(D168), "t", input_fn=yes)
    assert chip.programs == []


def test_openocd_bank_cover():
    one = "#0 : at32f4x at 0x08000000, size 0x00080000, buswidth 0, chipwidth 0"
    assert hw.openocd_banks_cover(one, 0x08004000, 0x20000)
    assert not hw.openocd_banks_cover(one, 0x08004000, 0xE59DC)          # D168 stock reaches bank 2
    two = one + "\n#1 : at32f4x at 0x08080000, size 0x00080000, buswidth 0, chipwidth 0"
    assert hw.openocd_banks_cover(two, 0x08004000, 0xE59DC)
    assert not hw.openocd_banks_cover("", 0x08004000, 4)


# ------------------------------------------------------------------------------------------------ CLI / configs
def test_server_commands(capsys):
    hw.main(["--radio", "d168", "--probe", "jlink", "server"])
    out = capsys.readouterr().out
    assert "JLinkGDBServerCLExe -device AT32F403AVGT7 -if SWD" in out and "-localhostonly 1" in out
    hw.main(["--radio", "d890", "--probe", "stlink", "server"])
    assert "openocd_d890_stlink.cfg" in capsys.readouterr().out


def test_openocd_configs_safe():
    for r in ("d890", "d168"):
        for p in ("stlink", "jlink"):
            path = hw.openocd_cfg(r, p)
            txt = open(path).read()
            assert "connect_assert_srst" in txt and "openocd_common.cfg" in txt
            assert not re.search(r"(?m)^\s*[^#\n]*(mass_erase|option_write|option_load|lock|unlock|program )", txt)
    common = open(os.path.join(hw.HERE, "openocd_common.cfg")).read()
    assert "gdb_flash_program disable" in common and "bindto 127.0.0.1" in common


def test_gdb_scripts_block_load():
    for r in ("d890", "d168"):
        txt = open(os.path.join(hw.HERE, "gdb", r + ".gdb")).read()
        assert "define hook-load" in txt and "GdbError" in txt
        assert not re.search(r"(?m)^\s*(load|monitor flash|monitor .*erase)", txt)


def test_cli_parses():
    p = hw.build_parser()
    a = p.parse_args(["--radio", "d168", "--probe", "jlink", "log", "--elf", "x.elf", "-f"])
    assert a.radio == "d168" and a.follow and a.elf == "x.elf"
    with pytest.raises(SystemExit):
        p.parse_args(["flash", "x.bin", "--yes"])                 # no confirmation bypass


# ------------------------------------------------------------------------------------------------ H2 option bits
def test_option_bits_d890():
    o = hw.option_bits(D890, 0x1FEFF8AA)
    assert not o["tzen"] and not o["iwdg_hw"] and not o["wwdg_hw"] and o["sram_end"] == D890.sram[1]
    assert any("IWDG: software" in ln for ln in o["lines"])
    o = hw.option_bits(D890, 0x1FEEF8AA)                       # IWDG_SW = 0
    assert o["iwdg_hw"] and any("IWDG: HARDWARE" in ln for ln in o["lines"])
    o = hw.option_bits(D890, 0x1FE6F8AA)                       # WWDG_SW = 0 too, IWDG_STOP 1
    assert o["wwdg_hw"] and o["iwdg_hw"]
    o = hw.option_bits(D890, 0x9FEFF8AA)
    assert o["tzen"] and any("TZEN=1" in ln for ln in o["lines"])
    assert any("SWAPPED" in ln for ln in hw.option_bits(D890, 0x1FFFF8AA)["lines"])


def test_option_bits_d168():
    o = hw.option_bits(D168, 0x00000000, 0x01FE)
    assert o["wdt_auto"] and o["sram_end"] == 0x20038000 and any("AUTO-START" in ln for ln in o["lines"])
    o = hw.option_bits(D168, 0x00000004, 0x00FF)
    assert not o["wdt_auto"] and o["sram_end"] == 0x20018000
    assert hw.option_bits(D168, 4, 0xFFFF)["sram_end"] == 0x20018000            # erased USD = 96 KB default
    assert hw.option_bits(D168, 4, 0x10FE)["sram_end"] == 0x20018000            # bad complement -> default


@pytest.mark.parametrize("radio", [D890, D168])
def test_info_shows_option_bits(radio, capsys):
    hw.cmd_info(jlink(FakeChip(radio))[0], radio, Args())
    out = capsys.readouterr().out
    if radio is D890:
        assert "TZEN=0" in out and "IWDG: software" in out and "WWDG: software" in out
    else:
        assert "WDT: AUTO-START" in out and "SRAM: 224 KB" in out


def test_flash_refuses_tzen():
    chip = FakeChip(D890)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D890, Args())
    chip.prot[3] |= 0x80                                       # TZEN = 1 (RDP still 0xAA)
    with pytest.raises(hw.HwError, match="TrustZone"):
        hw.write_app(be, D890, app_image(D890), "t", input_fn=yes)
    assert chip.programs == []


def test_d168_sp_above_configured_sram_refused():
    chip = FakeChip(D168)
    chip.write(D168.eopb0_addr, b"\xff\x00")                   # 96 KB mode
    be, _ = jlink(chip)
    hw.cmd_backup(be, D168, Args())
    with pytest.raises(hw.HwError, match="SRAM end"):
        hw.write_app(be, D168, app_image(D168, sp=0x2001F670), "t", input_fn=yes)    # stock D168 SP needs 224 KB
    assert chip.programs == []
    hw.write_app(be, D168, app_image(D168, sp=0x20018000), "t", input_fn=yes)        # fits 96 KB
    chip.write(D168.eopb0_addr, b"\xfe\x01")                   # 224 KB
    hw.write_app(be, D168, app_image(D168, sp=0x20038000), "t", input_fn=yes)
    assert len(chip.programs) == 2


# ------------------------------------------------------------------------------------------------ H1 recover
def put_app(chip, data):
    off = chip.radio.app_base - chip.radio.flash_base
    chip.flash[off:off + len(data)] = data


def app_window(chip):
    r = chip.radio
    return bytes(chip.flash[r.app_base - r.flash_base:r.write_end - r.flash_base])


@pytest.mark.parametrize("radio", [D890, D168])
def test_recover_restores_own_first_backup(radio, capsys):
    chip = FakeChip(radio)
    put_app(chip, app_image(radio, 0x5000))                    # "stock" app
    chip.flash[radio.cp_base - radio.flash_base:][:16] = b"factory data 123"   # something in free flash too
    before = app_window(chip)
    be, _ = jlink(chip)
    root = hw.backup_dir_for(radio, chip.read(radio.uid_addr, 12).hex())
    hw.cmd_backup(be, radio, Args(dir=os.path.join(root, "20261123-100000")))
    hw.write_app(be, radio, app_image(radio, 0x9000), "cfw", input_fn=yes)
    hw.cmd_backup(be, radio, Args(dir=os.path.join(root, "20261123-110000")))      # later backup (cfw on it)
    assert app_window(chip) != before
    import builtins
    orig_input = builtins.input
    builtins.input = lambda prompt: "FLASH 38393a3b"
    try:
        hw.cmd_recover(be, radio, Args(backup=None, stock=False))
    finally:
        builtins.input = orig_input
    assert app_window(chip) == before                          # the FIRST backup, whole window
    assert chip.programs[-1] == (radio.app_base, radio.write_end - radio.app_base)
    assert "20261123-100000" in capsys.readouterr().out


def test_recover_explicit_backup_and_refusals(monkeypatch, tmp_path):
    chip = FakeChip(D168)
    put_app(chip, app_image(D168, 0x4000))
    be, _ = jlink(chip)
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH 38393a3b")
    with pytest.raises(hw.HwError, match="--stock"):
        hw.cmd_recover(be, D168, Args(backup=None, stock=False))
    root = hw.backup_dir_for(D168, chip.read(D168.uid_addr, 12).hex())
    d1 = os.path.join(root, "a")
    hw.cmd_backup(be, D168, Args(dir=d1))
    other = FakeChip(D168, uid=bytes(range(0x40, 0x4C)))       # backup of another chip
    put_app(other, app_image(D168, 0x4000))
    d2 = str(tmp_path / "other")
    hw.cmd_backup(jlink(other)[0], D168, Args(dir=d2))
    with pytest.raises(hw.HwError, match="not a verified backup of this chip"):
        hw.cmd_recover(be, D168, Args(backup=d2, stock=False))
    open(os.path.join(d1, "flash_full.bin"), "r+b").write(b"\0")          # corrupt
    with pytest.raises(hw.HwError, match="corrupt"):
        hw.cmd_recover(be, D168, Args(backup=d1, stock=False))
    assert chip.programs == []


def test_recover_stock_option(monkeypatch, tmp_path):
    chip = FakeChip(D890)
    be, _ = jlink(chip)
    hw.cmd_backup(be, D890, Args())
    img = app_image(D890, 0x2000)
    p = tmp_path / "stock.bin"
    p.write_bytes(img)
    monkeypatch.setattr(D890, "stock_image", str(p))
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH 38393a3b")
    hw.cmd_recover(be, D890, Args(backup=None, stock=True))
    assert chip.programs == [(D890.app_base, len(img))]


def test_recover_cli():
    a = hw.build_parser().parse_args(["--radio", "d168", "recover", "--stock"])
    assert a.stock and a.backup is None
    a = hw.build_parser().parse_args(["recover", "--backup", "x"])
    assert a.backup == "x" and not a.stock


# ------------------------------------------------------------------------------------------------ H4 codeplug
def make_cp(model, n=0x400, ver=1):
    import zlib
    body = bytearray(os.urandom(n))
    total = 32 + n
    hdr = bytearray(32)
    hdr[:4] = b"D8CP"
    struct.pack_into("<HHI", hdr, 4, ver, 32, total)
    struct.pack_into("<HH", hdr, 16, 2, model)
    tail = bytes(hdr[16:]) + bytes(body)
    struct.pack_into("<I", hdr, 12, zlib.crc32(tail) & 0xFFFFFFFF)
    return bytes(hdr) + bytes(body)


@pytest.mark.parametrize("radio", [D890, D168])
def test_codeplug_window_guards(radio):
    size = radio.cp_end - radio.cp_base
    assert size == 0x60000
    assert (D890.cp_base, D890.cp_end, D168.cp_base, D168.cp_end) == (0x08180000, 0x081E0000, 0x08080000 + 0x20000,
                                                                      0x08100000)
    hw.check_write_range(radio, radio.cp_base, size, "codeplug")
    with pytest.raises(hw.HwError, match="past"):
        hw.check_write_range(radio, radio.cp_base, size + 1, "codeplug")
    with pytest.raises(hw.HwError, match="codeplug base"):
        hw.check_write_range(radio, radio.cp_base + 0x800, 16, "codeplug")
    with pytest.raises(hw.HwError, match="bootloader"):
        hw.check_write_range(radio, radio.flash_base, 16, "codeplug")
    hw.check_write_any(radio, radio.cp_base, size)
    with pytest.raises(hw.HwError):
        hw.check_write_any(radio, radio.cp_base + 0x100, 16)


def test_jlink_loadfile_codeplug_window(tmp_path):
    be, fj = jlink(FakeChip(D168))
    ok = tmp_path / "ok.bin"; ok.write_bytes(b"\xff" * 0x60000)
    big = tmp_path / "big.bin"; big.write_bytes(b"\xff" * 0x60001)
    be._guard_line("loadfile %s 0x080A0000" % ok)
    with pytest.raises(hw.HwError, match="past"):
        be.run(["loadfile %s 0x080A0000" % big])
    with pytest.raises(hw.HwError):
        be.run(["loadfile %s 0x080A0800" % ok])
    assert fj.argvs == []


def test_check_codeplug():
    hw.check_codeplug(D890, make_cp(0))
    hw.check_codeplug(D168, make_cp(1))
    with pytest.raises(hw.HwError, match="for D890UV"):
        hw.check_codeplug(D168, make_cp(0))
    with pytest.raises(hw.HwError, match="for D168UV"):
        hw.check_codeplug(D890, make_cp(1))
    bad = bytearray(make_cp(0)); bad[100] ^= 1
    with pytest.raises(hw.HwError, match="CRC"):
        hw.check_codeplug(D890, bytes(bad))
    with pytest.raises(hw.HwError, match="magic"):
        hw.check_codeplug(D890, b"\x00" * 64)
    with pytest.raises(hw.HwError, match="version"):
        hw.check_codeplug(D890, make_cp(0, ver=2))
    with pytest.raises(hw.HwError, match="window holds"):
        hw.check_codeplug(D890, make_cp(0, n=0x60000))


def test_cfw_cps_example_codeplug():
    p = os.path.join(hw.SRC, "cfw_cps", "examples", "k6rrr.d8cp")
    if not os.path.exists(p):
        pytest.skip("example missing")
    data = open(p, "rb").read()
    model = struct.unpack_from("<H", data, 18)[0]
    good, bad = (D890, D168) if model == 0 else (D168, D890)
    hw.check_codeplug(good, data)
    with pytest.raises(hw.HwError, match="model"):
        hw.check_codeplug(bad, data)


@pytest.mark.parametrize("radio", [D890, D168])
def test_codeplug_write_flow(radio, tmp_path, monkeypatch):
    chip = FakeChip(radio)
    put_app(chip, app_image(radio, 0x4000))                    # cfw (small), window blank
    be, _ = jlink(chip)
    cp = make_cp(radio.cp_model, 0x1234)
    f = tmp_path / "x.d8cp"; f.write_bytes(cp + b"junk after total")
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH 38393a3b")
    with pytest.raises(hw.HwError, match="no verified backup"):
        hw.cmd_codeplug(be, radio, Args(file=str(f)))
    hw.cmd_backup(be, radio, Args())
    hw.cmd_codeplug(be, radio, Args(file=str(f)))
    size = radio.cp_end - radio.cp_base
    assert chip.programs == [(radio.cp_base, size)]
    win = chip.read(radio.cp_base, size)
    assert win[:len(cp)] == cp and win[len(cp):] == b"\xff" * (size - len(cp))
    hw.cmd_codeplug(be, radio, Args(file=str(f)))              # over an existing codeplug: fine
    assert len(chip.programs) == 2


@pytest.mark.parametrize("radio", [D890, D168])
def test_codeplug_refuses_stock_code_in_window(radio, tmp_path, monkeypatch):
    chip = FakeChip(radio)
    put_app(chip, app_image(radio, radio.cp_base - radio.app_base + 0x1000))     # stock reaches into the window
    be, _ = jlink(chip)
    hw.cmd_backup(be, radio, Args())
    f = tmp_path / "x.d8cp"; f.write_bytes(make_cp(radio.cp_model))
    monkeypatch.setattr("builtins.input", lambda prompt: "FLASH 38393a3b")
    with pytest.raises(hw.HwError, match="neither blank nor a codeplug"):
        hw.cmd_codeplug(be, radio, Args(file=str(f)))
    other = tmp_path / "o.d8cp"; other.write_bytes(make_cp(1 - radio.cp_model))
    with pytest.raises(hw.HwError, match="model"):
        hw.cmd_codeplug(be, radio, Args(file=str(other)))
    assert chip.programs == []


def test_codeplug_cli():
    a = hw.build_parser().parse_args(["--radio", "d168", "codeplug", "x.d8cp"])
    assert a.c == "codeplug" and a.file == "x.d8cp"
    with pytest.raises(SystemExit):
        hw.build_parser().parse_args(["codeplug", "x.d8cp", "--yes"])


# ------------------------------------------------------------------------------------------------ H3 bootinfo
def assemble(src, tmp_path):
    import shutil
    import subprocess
    if not shutil.which("arm-none-eabi-as") or not shutil.which("arm-none-eabi-objcopy"):
        pytest.skip("arm-none-eabi binutils missing")
    s, o, b = tmp_path / "b.s", tmp_path / "b.o", tmp_path / "b.bin"
    s.write_text(src)
    subprocess.check_call(["arm-none-eabi-as", "-mcpu=cortex-m4", "-mthumb", "-o", str(o), str(s)])
    subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", str(o), str(b)])
    return b.read_bytes()


BOOT_CHECKING = """
    .syntax unified
    .thumb
    .word 0x20001000, start + 1 + 0x{base:08x}
start:
    ldr r0, =0x{app:08x}
    ldr r1, [r0]
    ldr r2, =0x{crc:08x}
    str r1, [r2]
    movw r3, #0x{keylo:04x}
    movt r3, #0x{keyhi:04x}
    ldr r4, =0x{end:08x}
    bx lr
    .ltorg
    .word 0x00000000, 0x77073096
"""

BOOT_PLAIN = """
    .syntax unified
    .thumb
    .word 0x20001000, start + 1 + 0x{base:08x}
start:
    ldr r0, =0x{app:08x}
    ldr r1, [r0, #4]
    bx r1
    .ltorg
"""


def boot_flash(radio, code):
    flash = bytearray(b"\xff" * radio.flash_size)
    blen = radio.app_base - radio.flash_base
    flash[:blen] = bytes((0x5A + i) & 0xFF for i in range(blen))
    flash[:len(code)] = code
    return bytes(flash)


@pytest.mark.parametrize("radio", [D890, D168])
def test_bootinfo_finds_checks(radio, tmp_path):
    key = radio.key_addr or 0x08003000
    crc = radio.crc_regs[-1][0]
    code = assemble(BOOT_CHECKING.format(base=radio.flash_base, app=radio.app_base, crc=crc, keylo=key & 0xFFFF,
                                         keyhi=key >> 16, end=radio.write_end), tmp_path)
    text, summ = hw.bootinfo_report(radio, boot_flash(radio, code))
    assert summ["disassembled"] and summ["app_base_ref"] and summ["crc_unit"] and summ["sw_crc_table"]
    assert summ["app_end_ref"] and summ["key_ref"] == bool(radio.key_addr)
    assert "CHECK LIKELY" in text and "movw/movt" in (text if radio.key_addr else "movw/movt")
    assert re.search(r"ldr\s+r1, \[r0", text)                # the instruction after the literal load is shown


@pytest.mark.parametrize("radio", [D890, D168])
def test_bootinfo_plain_jump(radio, tmp_path, monkeypatch):
    code = assemble(BOOT_PLAIN.format(base=radio.flash_base, app=radio.app_base), tmp_path)
    text, summ = hw.bootinfo_report(radio, boot_flash(radio, code))
    assert summ["app_base_ref"] and not summ["crc_unit"] and not summ["sw_crc_table"] and not summ["app_end_ref"]
    assert "no sign of an app checksum" in text
    monkeypatch.setattr(hw, "OBJDUMP", "no-such-objdump")      # without objdump: literal words still found
    text, summ = hw.bootinfo_report(radio, boot_flash(radio, code))
    assert not summ["disassembled"] and summ["app_base_ref"] and "NOT available" in text


def test_bootinfo_secure_alias_d890(tmp_path):
    code = assemble(BOOT_PLAIN.format(base=0x0C000000, app=0x0C00C000), tmp_path)
    _, summ = hw.bootinfo_report(D890, boot_flash(D890, code))
    assert summ["app_base_ref"]


def test_bootinfo_cli_from_backup(tmp_path, capsys):
    code = assemble(BOOT_CHECKING.format(base=D168.flash_base, app=D168.app_base, crc=0x40023000, keylo=0x3000,
                                         keyhi=0x0800, end=D168.write_end), tmp_path)
    chip = FakeChip(D168)
    chip.flash[:len(code)] = code
    hw.cmd_backup(jlink(chip)[0], D168, Args())
    capsys.readouterr()
    with pytest.raises(SystemExit):                            # no D890 backup
        hw.main(["--radio", "d890", "bootinfo"])
    hw.main(["--radio", "d168", "bootinfo"])                   # no probe needed
    out = capsys.readouterr().out
    assert "CRC unit" in out and "CHECK LIKELY" in out
    d, _ = hw.find_backup(D168, chip.read(D168.uid_addr, 12).hex())
    assert os.path.exists(os.path.join(d, "bootinfo.txt"))
    assert json.load(open(os.path.join(d, "bootinfo.json")))["crc_unit"]
