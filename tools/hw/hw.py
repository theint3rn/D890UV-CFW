#!/usr/bin/env python3
"""SWD tool for the real radios: AnyTone D890UV (STM32U575) and D168UV (Artery AT32F403A).

Probes / backends:
  --probe jlink    SEGGER J-Link through J-Link Commander (JLinkExe), one command file per step.  [default]
  --probe stlink   ST-Link through a running OpenOCD (TCL port 6666), config tools/hw/openocd_<radio>_<probe>.cfg.
  --probe jlink-openocd   J-Link through OpenOCD instead of JLinkExe (same configs, J-Link interface).

Commands (read-only unless marked WRITE):
  info                     CPU, chip ID, flash size, UID, read protection (RDP / FAP), TrustZone, watchdog option
                           bits, D168 SRAM size (EOPB0). Refuses a wrong --radio.
  backup [dir]             full internal flash (read twice, must match) + OTP / option bytes / USD + manifest with
                           SHA-256, into hw_backups/<radio>_<uid>/<time>/. Needed before any WRITE on that chip.
  verify-backup [dir]      read the flash again and compare with the (latest) backup of this chip.
  flash <file>        WRITE  program an app image (.bin / .elf -> its .bin) at the app base ONLY. Refuses the
                           bootloader, addresses outside the app window, a protected chip, a chip without a
                           verified backup, and a bad vector table. Typed confirmation "FLASH <uid8>".
                           Verifies (probe verify + own read-back), then re-checks protection + bootloader.
  recover [--backup D] [--stock]  WRITE  restore THIS chip's own app window from its first verified backup
                           (or --backup D); --stock writes the stock package image instead. Same guards as flash.
  codeplug <file>     WRITE  write a cfw_cps binary export (.d8cp) into the codeplug window only (D890
                           0x08180000-0x081DFFFF, D168 0x080A0000-0x080FFFFF): header, CRC and radio model checked,
                           window on the chip must be blank or hold a codeplug (never stock code). Same guards as flash.
  bootinfo [dir]           no probe: from a backup's flash_full.bin, list what the bootloader reads in the app area
                           (vector table, CRC / HASH units, refs to the activation key, app end, size words).
  calram [--compare F]     read the RF calibration copy the STOCK firmware loaded into RAM (SRAM reads only; stock
                           V1.05 / V1.07 must be running), save it, decode it, compare with a cal_dump.py file.
  check [--elf E] [--secs N]  reset, run N s, halt: fault registers, PC, OS markers (D890 stock/lifted), lift trap.
  log [--elf E] [--follow]    read the SEGGER RTT log of a `make DEBUG=1` cfw build (memory poll; no halt).
  server                   print the command that starts the GDB server for this radio/probe (does not run it).
  reset | halt | resume | read <addr> [count]

Never: writes below the app base (bootloader), option bytes / USD / RDP / FAP, mass erase, or lowers protection
(that mass-erases and kills the factory bootloader). RAM writes only for the RTT read pointer, inside SRAM.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))                  # D890UV_src
PROJECTS = os.path.dirname(os.path.dirname(SRC))              # work folder
BACKUP_ROOT = os.path.join(SRC, "hw_backups")


class HwError(Exception):
    pass


# --------------------------------------------------------------------------------------------- radio profiles
@dataclass
class Radio:
    key: str
    name: str
    mcu: str
    cpu_part: int                   # CPUID PARTNO (bits 15:4)
    jlink_device: str
    openocd_target: str             # OpenOCD target script (relative to its scripts dir)
    flash_base: int
    flash_size: int
    app_base: int                   # first writable address (everything below = bootloader, NEVER written)
    write_end: int                  # first address after the writable window
    sram: tuple                     # (start, end) for vector-table SP check and RTT pointer writes
    idcode_addr: int
    flash_kb_addr: int
    uid_addr: int
    prot_addr: int                  # register holding the read-protection state
    extra_dumps: list = field(default_factory=list)   # (file, addr, size), backup only
    stock_image: str = ""
    stock_label: str = ""
    cp_base: int = 0                # cfw codeplug window [cp_base, cp_end) (cfw_cps "D8CP" blob)
    cp_end: int = 0
    cp_model: int = 0               # D8CP header u16 @18: 0 = D890UV, 1 = D168UV
    eopb0_addr: int = 0             # D168: USD EOPB0 (SRAM 96 / 224 KB)
    crc_regs: tuple = ()            # (addr, label) of CRC / HASH units the bootloader might use on the app
    key_addr: int = 0               # D890 activation key (in the bootloader area)
    os_running_addr: int = 0        # D890 stock / lifted uC/OS markers
    os_tcb_cur_addr: int = 0
    notes: str = ""


RADIOS = {
    "d890": Radio(
        key="d890", name="AnyTone D890UV", mcu="STM32U575 (Cortex-M33, 2 MB flash)", cpu_part=0xD21,
        jlink_device="STM32U575ZI", openocd_target="target/stm32u5x.cfg",
        flash_base=0x08000000, flash_size=0x200000,
        app_base=0x0800C000, write_end=0x081E4000,           # 0x081E4000+ = stock runtime table cache (NAND copy)
        sram=(0x20000000, 0x200C0000),
        idcode_addr=0xE0044000, flash_kb_addr=0x0BFA07A0, uid_addr=0x0BFA0700, prot_addr=0x40022040,
        extra_dumps=[("otp_512B.bin", 0x0BFA0000, 0x200), ("flash_regs_0x40022000.bin", 0x40022000, 0x100)],
        stock_image=os.path.join(PROJECTS, "D890UV", "D890UV V1.05 DMR official release 260521",
                                 "D890UV_V1.05_DMR FW", "D890UV_V1.05_20260521.CDD"),
        stock_label="stock D890UV V1.05 (.CDD, raw app image at 0x0800C000)",
        cp_base=0x08180000, cp_end=0x081E0000, cp_model=0,
        crc_regs=((0x40023000, "CRC unit"), (0x50023000, "CRC unit (secure alias)"), (0x420C0400, "HASH unit"),
                  (0x520C0400, "HASH unit (secure alias)")),
        key_addr=0x0800A000,
        os_running_addr=0x2000A479, os_tcb_cur_addr=0x2000A47C,
        notes="bootloader 0x08000000-0x0800BFFF incl. activation key 0x0800A000 (chip-UID bound)"),
    "d168": Radio(
        key="d168", name="AnyTone D168UV", mcu="Artery AT32F403A (Cortex-M4F, 1 MB flash)", cpu_part=0xC24,
        jlink_device="AT32F403AVGT7", openocd_target="target/artery/at32f4x.cfg",
        flash_base=0x08000000, flash_size=0x100000,
        app_base=0x08004000, write_end=0x08100000,
        sram=(0x20000000, 0x20038000),                        # 224 KB (EOPB0 option set by the factory: keep it)
        idcode_addr=0xE0042000, flash_kb_addr=0x1FFFF7E0, uid_addr=0x1FFFF7E8, prot_addr=0x4002201C,
        extra_dumps=[("usd_0x1FFFF800.bin", 0x1FFFF800, 0x30), ("flash_regs_0x40022000.bin", 0x40022000, 0x60)],
        stock_image=os.path.join(PROJECTS, "D168UV", "fw", "d168uv_v107_08004000.bin"),
        stock_label="stock D168UV V1.07 (flat image of the .CDD at 0x08004000)",
        cp_base=0x080A0000, cp_end=0x08100000, cp_model=1, eopb0_addr=0x1FFFF810,
        crc_regs=((0x40023000, "CRC unit"), (0x400220F4, "FLASH_CRC_CTRL (flash CRC check)"),
                  (0x400220F8, "FLASH_CRC_CHKR (flash CRC result)")),
        notes="bootloader 0x08000000-0x08003FFF; 2 KB sectors, bank 2 from 0x08080000"),
}


def protection_from_word(radio, word):
    """Decode the protection register. Returns dict(level, readable, writable, text)."""
    if radio.key == "d890":                                   # FLASH_OPTR: RDP byte, TZEN bit 31
        rdp = word & 0xFF
        tz = word >> 31 & 1
        if rdp == 0xAA:
            lvl, rd, wr, txt = "RDP0", True, True, "RDP Level 0 (open)"
        elif rdp == 0x55:
            lvl, rd, wr, txt = "RDP0.5", True, False, "RDP Level 0.5 (TrustZone: non-secure debug only)"
        elif rdp == 0xCC:
            lvl, rd, wr, txt = "RDP2", False, False, "RDP Level 2 (LOCKED for good: SWD is dead)"
        else:
            lvl, rd, wr, txt = "RDP1", False, False, "RDP Level 1 (flash read-protected)"
        return dict(level=lvl, readable=rd, writable=wr, raw=word,
                    text="%s, TZEN=%d (FLASH_OPTR 0x%08x)" % (txt, tz, word))
    fap = word >> 1 & 1                                       # AT32 FLASH_USD: bit1 FAP, bit0 USDERR
    if fap:
        return dict(level="FAP", readable=False, writable=False, raw=word,
                    text="FAP ON (flash access protection: reads blocked) (FLASH_USD 0x%08x)" % word)
    return dict(level="open", readable=True, writable=True, raw=word,
                text="FAP off (open) (FLASH_USD 0x%08x)" % word)


def option_bits(radio, prot_word, eopb0=None):
    """Decode the option bits that matter before a write: TrustZone, watchdogs, bank swap, D168 SRAM size.
    Returns dict(tzen, iwdg_hw, wwdg_hw, wdt_auto, sram_end, lines)."""
    lo, hi = radio.sram
    o = dict(tzen=False, iwdg_hw=False, wwdg_hw=False, wdt_auto=False, sram_end=hi, lines=[])
    if radio.key == "d890":                                   # RM0456 FLASH_OPTR
        b = lambda n: prot_word >> n & 1
        o["tzen"] = bool(b(31))
        o["iwdg_hw"] = not b(16)
        o["wwdg_hw"] = not b(19)
        o["lines"] = [
            "TrustZone: TZEN=%d%s" % (b(31), " (secure boot / secure flash: app writes REFUSED)" if b(31) else ""),
            "IWDG: %s, %s in Stop, %s in Standby (IWDG_SW=%d IWDG_STOP=%d IWDG_STDBY=%d)"
            % ("HARDWARE (starts at reset: cfw must refresh it, audit F3)" if not b(16) else "software (off until started)",
               "runs" if b(17) else "frozen", "runs" if b(18) else "frozen", b(16), b(17), b(18)),
            "WWDG: %s (WWDG_SW=%d)" % ("HARDWARE (starts at reset)" if not b(19) else "software", b(19)),
            "banks: DUALBANK=%d SWAP_BANK=%d%s" % (b(21), b(20), " (SWAPPED: bank 2 is mapped at 0x08000000)" if b(20) else ""),
            "boot: nSWBOOT0=%d nBOOT0=%d" % (b(26), b(27)),
        ]
        return o
    auto = not (prot_word >> 2 & 1)                           # AT32 FLASH_USD bit2 nWDT_ATO_EN (1 = no auto start)
    o["wdt_auto"] = auto
    o["lines"].append("WDT: %s (nWDT_ATO_EN=%d)" % ("AUTO-START at reset (cfw must refresh it, audit F3)" if auto
                                                     else "software (off until started)", prot_word >> 2 & 1))
    if eopb0 is not None:
        v, inv = eopb0 & 0xFF, eopb0 >> 8 & 0xFF
        valid = v ^ inv == 0xFF
        kb = 224 if valid and not v & 1 else 96                # Artery: EOPB0 0xFE = 224 KB, 0xFF / erased = 96 KB
        o["sram_end"] = lo + kb * 1024
        o["lines"].append("SRAM: %d KB (EOPB0 0x%04x%s) -> 0x%08x-0x%08x" % (
            kb, eopb0 & 0xFFFF, "" if valid else ", complement bad / erased = default", lo, o["sram_end"]))
    return o


# --------------------------------------------------------------------------------------------- guards
def write_windows(radio):
    """Flash windows a command may write, each written from its base only: {name: (base, end)}."""
    w = {"app": (radio.app_base, radio.write_end)}
    if radio.cp_base:
        w["codeplug"] = (radio.cp_base, radio.cp_end)
    return w


def check_write_range(radio, addr, length, window="app"):
    """A write must start at the base of one window and end inside it. app = [app_base, write_end),
    codeplug = [cp_base, cp_end) (inside the app window). Nothing below app_base (bootloader), ever."""
    if length <= 0:
        raise HwError("empty image")
    end = addr + length
    if addr < radio.app_base:
        raise HwError("refusing: 0x%08x is below the app base 0x%08x (bootloader)" % (addr, radio.app_base))
    windows = write_windows(radio)
    if window not in windows:
        raise HwError("no %s window on %s" % (window, radio.name))
    base, wend = windows[window]
    if addr != base:
        raise HwError("refusing: %s images go to the %s base 0x%08x only (got 0x%08x)"
                      % (window, "app" if window == "app" else window, base, addr))
    if end > wend:
        raise HwError("refusing: image ends 0x%08x, past the writable %s window end 0x%08x" % (end, window, wend))


def check_write_any(radio, addr, length):
    """Probe-level guard: [addr, addr+length) must be a legal write of some window."""
    for name, (base, _) in write_windows(radio).items():
        if addr == base:
            return check_write_range(radio, addr, length, name)
    return check_write_range(radio, addr, length, "app")       # raises (bootloader / not a window base)


CP_MAGIC = b"D8CP"
CP_MODELS = {0: "D890UV", 1: "D168UV"}


def check_codeplug(radio, data):
    """cfw_cps binary export: magic, version 1, header 32, length, CRC-32 of [16, total), model == radio."""
    import zlib
    if len(data) < 32 or data[:4] != CP_MAGIC:
        raise HwError("not a cfw codeplug (no 'D8CP' magic): export the .d8cp binary with cfw_cps")
    ver, hs, total, crc = struct.unpack_from("<HHII", data, 4)
    model = struct.unpack_from("<H", data, 18)[0]
    if ver != 1 or hs != 32:
        raise HwError("codeplug format version %d / header %d not supported (want 1 / 32)" % (ver, hs))
    if not 32 <= total <= len(data):
        raise HwError("codeplug length %d out of range (file %d bytes)" % (total, len(data)))
    if total > radio.cp_end - radio.cp_base:
        raise HwError("codeplug is %d bytes, the %s window holds %d" % (total, radio.key, radio.cp_end - radio.cp_base))
    if zlib.crc32(data[16:total]) & 0xFFFFFFFF != crc:
        raise HwError("codeplug CRC mismatch")
    if model != radio.cp_model:
        raise HwError("codeplug is for %s (model %d), --radio %s needs model %d: convert it with cfw_cps export --model"
                      % (CP_MODELS.get(model, "?"), model, radio.key, radio.cp_model))
    return data[:total]


def check_image(radio, data, addr):
    if len(data) < 8:
        raise HwError("image too small")
    sp, reset = struct.unpack_from("<II", data, 0)
    lo, hi = radio.sram
    if not (lo < sp <= hi):
        raise HwError("vector table: SP 0x%08x not in SRAM 0x%08x-0x%08x (not an app image for %s?)"
                      % (sp, lo, hi, radio.name))
    if not (reset & 1 and addr <= (reset & ~1) < addr + len(data)):
        raise HwError("vector table: reset 0x%08x not a Thumb address inside the image" % reset)


def check_ram_write(radio, addr):
    lo, hi = radio.sram
    if addr & 3 or not (lo <= addr and addr + 4 <= hi):
        raise HwError("refusing RAM write at 0x%08x (outside SRAM / unaligned)" % addr)


def load_image(path):
    if path.endswith(".elf"):
        bin_path = path[:-4] + ".bin"
        if not os.path.exists(bin_path) or os.path.getmtime(bin_path) < os.path.getmtime(path):
            raise HwError("%s missing or older than the ELF: run make first" % bin_path)
        path = bin_path
    with open(path, "rb") as f:
        return f.read()


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# --------------------------------------------------------------------------------------------- backends
class Backend:
    """Probe access. Subclasses implement read_many / write_ram32 / program / run control."""
    def __init__(self, radio):
        self.radio = radio

    def read(self, addr, size):
        return self.read_many([(addr, size)])[0]

    def rd32(self, addr, n=1):
        return list(struct.unpack("<%dI" % n, self.read(addr, 4 * n)))

    def read_many(self, regions):
        raise NotImplementedError

    def write_ram32(self, addr, value):
        raise NotImplementedError

    def program(self, path, addr):
        """Erase + write + verify a .bin at addr. Returns the probe's own verify result (bool)."""
        raise NotImplementedError

    def reset_halt(self): raise NotImplementedError
    def reset_run(self): raise NotImplementedError
    def halt(self): raise NotImplementedError
    def resume(self): raise NotImplementedError
    def regs_text(self): raise NotImplementedError


class OpenOCDBackend(Backend):
    """Talks to a running OpenOCD over TCL (start it with `hw.py server` output)."""
    def __init__(self, radio, host="127.0.0.1", port=6666, sock=None):
        super().__init__(radio)
        self.s = sock
        self.host, self.port = host, port

    def _conn(self):
        if self.s is None:
            try:
                self.s = socket.create_connection((self.host, self.port), timeout=60)
            except OSError:
                raise HwError("no OpenOCD on %s:%d - start it first (python3 tools/hw/hw.py server)"
                              % (self.host, self.port))
        return self.s

    def cmd(self, c):
        s = self._conn()
        s.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = s.recv(65536)
            if not chunk:
                raise HwError("OpenOCD closed the connection")
            buf += chunk
        return buf[:-1].decode(errors="replace").strip()

    def read_many(self, regions):
        out = []
        for addr, size in regions:
            if size <= 0x1000:
                r = self.cmd("read_memory 0x%08x 8 %d" % (addr, size))
                try:
                    b = bytes(int(x, 0) for x in r.split())
                except ValueError:
                    raise HwError("read 0x%08x failed: %s" % (addr, r))
            else:
                fd, tmp = tempfile.mkstemp(suffix=".bin")
                os.close(fd)
                try:
                    r = self.cmd("dump_image %s 0x%08x 0x%x" % (tmp, addr, size))
                    with open(tmp, "rb") as f:
                        b = f.read()
                finally:
                    os.remove(tmp)
            if len(b) != size:
                raise HwError("read 0x%08x+0x%x returned %d bytes" % (addr, size, len(b)))
            out.append(b)
        return out

    def write_ram32(self, addr, value):
        check_ram_write(self.radio, addr)
        self.cmd("write_memory 0x%08x 32 {0x%08x}" % (addr, value))

    def program(self, path, addr):
        check_write_any(self.radio, addr, os.path.getsize(path))
        size = os.path.getsize(path)
        if not openocd_banks_cover(self.cmd("flash banks"), addr, size):
            raise HwError("this OpenOCD has no flash bank covering 0x%08x+0x%x on %s (no driver, or single-bank "
                          "driver). Use --probe jlink." % (addr, size, self.radio.mcu))
        self.reset_halt()
        print(self.cmd("flash write_image erase %s 0x%08x bin" % (path, addr)))
        v = self.cmd("verify_image %s 0x%08x bin" % (path, addr))
        print(v)
        return "verified" in v.lower()

    def reset_halt(self): return self.cmd("reset halt")
    def reset_run(self): return self.cmd("reset run")
    def halt(self): return self.cmd("halt 1000")
    def resume(self): return self.cmd("resume")

    def regs_text(self):
        return " | ".join(self.cmd("reg %s" % r).strip() for r in ("pc", "lr", "sp"))


class JLinkBackend(Backend):
    """SEGGER J-Link Commander (JLinkExe) in batch mode: one command file per operation.

    Connect: -AutoConnect 1. When the firmware has taken the SWD pins, J-Link falls back to connect-under-reset
    by itself if NRST is wired (it prints "Trying connect under reset"). --jlink-script passes a J-Link script
    (-JLinkScriptFile) for a custom connect sequence."""
    EXE = "JLinkExe"

    def __init__(self, radio, device=None, speed=4000, serial=None, script=None, runner=None, exe=None, nogui=False):
        super().__init__(radio)
        self.device = device or radio.jlink_device
        self.speed = speed
        self.serial = serial
        self.script = script
        self.runner = runner or self._run_exe
        self.exe = exe or os.environ.get("JLINKEXE", self.EXE)
        self.last_output = ""
        self.nogui = nogui            # default: J-Link dialogs stay visible, so an "unsecure / erase?" prompt reaches
                                      # the user (answer NO) instead of being answered by a default

    def argv(self, cmdfile):
        a = [self.exe] + (["-NoGui", "1"] if self.nogui else []) + ["-ExitOnError", "1", "-Device", self.device, "-If", "SWD",
             "-Speed", str(self.speed), "-AutoConnect", "1"]
        if self.serial:
            a += ["-USB", str(self.serial)]
        if self.script:
            a += ["-JLinkScriptFile", self.script]
        return a + ["-CommandFile", cmdfile]

    def _run_exe(self, argv):
        if shutil.which(argv[0]) is None:
            raise HwError("%s not found: install the SEGGER J-Link Software Pack (see docs/HARDWARE.md)" % argv[0])
        p = subprocess.run(argv, capture_output=True, text=True, timeout=600)
        return p.returncode, p.stdout + p.stderr

    def run(self, lines):
        """Run Commander lines (an 'exit' is appended). Returns the output; raises on error exit."""
        for ln in lines:
            self._guard_line(ln)
        fd, path = tempfile.mkstemp(suffix=".jlink")
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(list(lines) + ["exit"]) + "\n")
        try:
            rc, out = self.runner(self.argv(path))
        finally:
            os.remove(path)
        self.last_output = out
        if rc != 0:
            raise HwError("JLinkExe failed (rc %d):\n%s" % (rc, (out or "")[-2000:]))
        return out

    def _guard_line(self, ln):
        """Last line of defence: no command that can erase/write flash or option bytes gets out except the
        loadfile built by program() for the checked app window."""
        w = ln.strip().split()
        if not w:
            return
        c = w[0].lower()
        if c in ("erase", "unlock", "loadbin", "exec", "unsecure", "rsettype", "wreg"):
            raise HwError("refusing J-Link command %r" % ln)
        if c in ("w1", "w2", "w4", "write1", "write2", "write4", "w8", "write8"):
            check_ram_write(self.radio, int(w[1], 16))
        if c == "loadfile":
            check_write_any(self.radio, int(w[2], 16), os.path.getsize(w[1]))

    def read_many(self, regions):
        tmpd = tempfile.mkdtemp(prefix="jl_")
        try:
            lines, paths = [], []
            for i, (addr, size) in enumerate(regions):
                p = os.path.join(tmpd, "r%d.bin" % i)
                paths.append(p)
                lines.append("savebin %s 0x%08X 0x%X" % (p, addr, size))
            self.run(lines)
            out = []
            for p, (addr, size) in zip(paths, regions):
                if not os.path.exists(p) or os.path.getsize(p) != size:
                    raise HwError("J-Link read 0x%08x+0x%x failed:\n%s" % (addr, size, self.last_output[-1500:]))
                with open(p, "rb") as f:
                    out.append(f.read())
            return out
        finally:
            shutil.rmtree(tmpd, ignore_errors=True)

    def write_ram32(self, addr, value):
        check_ram_write(self.radio, addr)
        self.run(["w4 0x%08X 0x%08X" % (addr, value)])

    def program(self, path, addr):
        check_write_any(self.radio, addr, os.path.getsize(path))
        out = self.run(["r", "h", "loadfile %s 0x%08X" % (path, addr), "verifybin %s 0x%08X" % (path, addr)])
        print(out[-1500:])
        return bool(re.search(r"(?i)verify successful|o\.k\.", out))

    def reset_halt(self): return self.run(["r", "h"])
    def reset_run(self): return self.run(["r", "g"])
    def halt(self): return self.run(["h"])
    def resume(self): return self.run(["g"])

    def regs_text(self):
        out = self.run(["h", "regs"])
        regs = dict(re.findall(r"\b(PC|SP|LR|R13|R14|R15)\b(?:\s*\([A-Z]+\))?\s*=\s*([0-9A-Fa-f]{8})", out))
        return " | ".join("%s=0x%s" % kv for kv in regs.items()) or out[-600:]


def openocd_banks_cover(banks_text, addr, size):
    """True if the `flash banks` listing has banks covering [addr, addr+size) without a gap."""
    spans = sorted((int(b, 16), int(b, 16) + int(n, 16))
                   for b, n in re.findall(r"at (0x[0-9a-fA-F]+), size (0x[0-9a-fA-F]+)", banks_text))
    pos, end = addr, addr + size
    for lo, hi in spans:
        if lo <= pos < hi:
            pos = hi
        if pos >= end:
            return True
    return False


# --------------------------------------------------------------------------------------------- server lines
def openocd_cfg(radio_key, probe):
    p = "stlink" if probe == "stlink" else "jlink"
    return os.path.join(HERE, "openocd_%s_%s.cfg" % (radio_key, p))


def server_command(radio, probe, speed=4000, serial=None):
    if probe == "jlink":
        c = ["JLinkGDBServerCLExe", "-device", radio.jlink_device, "-if", "SWD", "-speed", str(speed),
             "-port", "2331", "-nogui", "-noir", "-localhostonly", "1", "-nohalt"]
        if serial:
            c += ["-select", "USB=%s" % serial]
        return c
    return ["openocd", "-f", openocd_cfg(radio.key, probe)]


def make_backend(a, radio):
    if a.probe == "jlink":
        return JLinkBackend(radio, device=a.jlink_device, speed=a.speed, serial=a.serial, script=a.jlink_script,
                            nogui=a.nogui)
    return OpenOCDBackend(radio, port=a.tcl_port)


# --------------------------------------------------------------------------------------------- operations
def chip_info(be, radio):
    # CPUID alone first: the other addresses are radio specific (wrong MCU -> bus faults / wrong meaning)
    cpuid = be.rd32(0xE000ED00)[0]
    part = cpuid >> 4 & 0xFFF
    if part != radio.cpu_part:
        raise HwError("CPUID 0x%08x (part 0x%03x) is not a %s (expected part 0x%03x): wrong --radio?"
                      % (cpuid, part, radio.mcu, radio.cpu_part))
    regions = [(radio.idcode_addr, 4), (radio.flash_kb_addr, 4), (radio.uid_addr, 12), (radio.prot_addr, 4)]
    if radio.eopb0_addr:
        regions.append((radio.eopb0_addr, 4))
    got = be.read_many(regions)
    idcode, kb, uid, prot = got[:4]
    pw = struct.unpack("<I", prot)[0]
    eopb0 = struct.unpack("<I", got[4])[0] & 0xFFFF if radio.eopb0_addr else None
    info = dict(cpuid=cpuid, idcode=struct.unpack("<I", idcode)[0], flash_kb=struct.unpack("<I", kb)[0] & 0xFFFF,
                uid=uid.hex(), prot=protection_from_word(radio, pw), opt=option_bits(radio, pw, eopb0))
    return info


def uid_tag(uid_hex):
    return uid_hex[-8:]


def print_info(radio, info):
    print("%s: %s" % (radio.name, radio.mcu))
    print("CPUID 0x%08x, IDCODE 0x%08x%s" % (info["cpuid"], info["idcode"],
          " (DEV_ID 0x%03x, 0x482 = STM32U575/585)" % (info["idcode"] & 0xFFF) if radio.key == "d890" else ""))
    print("flash %d KB, UID %s" % (info["flash_kb"], info["uid"]))
    print("protection: %s" % info["prot"]["text"])
    for ln in info["opt"]["lines"]:
        print("  " + ln)
    print("backup possible: %s, app writes possible: %s" % (info["prot"]["readable"], info["prot"]["writable"]))


def cmd_info(be, radio, a):
    print_info(radio, chip_info(be, radio))
    print("regs:", be.regs_text())
    be.resume()


def backup_dir_for(radio, uid):
    return os.path.join(BACKUP_ROOT, "%s_%s" % (radio.key, uid))


def find_backup(radio, uid):
    """Latest verified backup of this exact chip, or None."""
    for d in sorted(glob.glob(os.path.join(backup_dir_for(radio, uid), "*")), reverse=True):
        m = os.path.join(d, "manifest.json")
        if os.path.exists(m):
            with open(m) as f:
                man = json.load(f)
            if man.get("radio") == radio.key and man.get("uid") == uid and man.get("verified"):
                return d, man
    return None


def cmd_backup(be, radio, a):
    info = chip_info(be, radio)
    print_info(radio, info)
    if not info["prot"]["readable"]:
        raise HwError("flash not readable (%s). Do NOT lower protection: it mass-erases the factory bootloader."
                      % info["prot"]["text"])
    d = a.dir or os.path.join(backup_dir_for(radio, info["uid"]), datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(d, exist_ok=True)
    be.reset_halt()
    flash1 = be.read(radio.flash_base, radio.flash_size)
    flash2 = be.read(radio.flash_base, radio.flash_size)
    extras = be.read_many([(addr, size) for _, addr, size in radio.extra_dumps])
    be.resume()
    files = {}

    def save(name, addr, data):
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
        files[name] = dict(addr="0x%08x" % addr, size=len(data), sha256=sha256(data))
        print("%-28s %s" % (name, files[name]["sha256"]))

    save("flash_full.bin", radio.flash_base, flash1)
    for (name, addr, _), data in zip(radio.extra_dumps, extras):
        save(name, addr, data)
    boot = flash1[:radio.app_base - radio.flash_base]
    same_twice = flash1 == flash2
    boot_present = boot != b"\xff" * len(boot) and boot != b"\x00" * len(boot)
    stock_match = None
    if os.path.exists(radio.stock_image):
        with open(radio.stock_image, "rb") as f:
            stock = f.read()
        off = radio.app_base - radio.flash_base
        stock_match = flash1[off:off + len(stock)] == stock
    man = dict(radio=radio.key, mcu=radio.mcu, uid=info["uid"], cpuid="0x%08x" % info["cpuid"],
               idcode="0x%08x" % info["idcode"], flash_kb=info["flash_kb"], protection=info["prot"]["text"],
               prot_raw="0x%08x" % info["prot"]["raw"], probe=a.probe, created=datetime.datetime.now().isoformat(),
               read_twice_identical=same_twice, bootloader_present=boot_present,
               bootloader_sha256=sha256(boot), app_matches_stock=stock_match, files=files,
               verified=same_twice and boot_present)
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(man, f, indent=1)
    print("read twice identical: %s, bootloader present: %s, app == %s: %s"
          % (same_twice, boot_present, radio.stock_label, stock_match))
    print("backup in %s (%s)" % (d, "VERIFIED" if man["verified"] else "NOT verified: writes stay refused"))
    if not man["verified"]:
        raise HwError("backup not verified (flash reads differ or bootloader blank)")


def cmd_verify_backup(be, radio, a):
    info = chip_info(be, radio)
    if a.dir:
        with open(os.path.join(a.dir, "manifest.json")) as f:
            d, man = a.dir, json.load(f)
    else:
        found = find_backup(radio, info["uid"])
        if not found:
            raise HwError("no verified backup for %s UID %s" % (radio.key, info["uid"]))
        d, man = found
    if man["uid"] != info["uid"]:
        raise HwError("backup %s is of UID %s, this chip is %s" % (d, man["uid"], info["uid"]))
    live = be.read(radio.flash_base, radio.flash_size)
    with open(os.path.join(d, "flash_full.bin"), "rb") as f:
        saved = f.read()
    if sha256(saved) != man["files"]["flash_full.bin"]["sha256"]:
        raise HwError("backup file %s/flash_full.bin is corrupt (hash differs from manifest)" % d)
    boot = radio.app_base - radio.flash_base
    print("bootloader identical: %s" % (live[:boot] == saved[:boot]))
    print("whole flash identical: %s" % (live == saved))
    if live[:boot] != saved[:boot]:
        raise HwError("bootloader differs from the backup!")


def confirm(prompt, phrase):
    try:
        got = input("%s\nType '%s' to continue: " % (prompt, phrase)).strip()
    except EOFError:
        got = ""
    if got != phrase:
        raise HwError("aborted (confirmation not given)")


def write_app(be, radio, data, label, input_fn=None, window="app"):
    """The one flash-write path (app image or codeplug). All guards in order; nothing is written before the last
    one passes."""
    addr = write_windows(radio)[window][0] if window in write_windows(radio) else radio.app_base
    check_write_range(radio, addr, len(data), window)
    if window == "app":
        check_image(radio, data, addr)
    else:
        check_codeplug(radio, data)
    info = chip_info(be, radio)
    print_info(radio, info)
    if not info["prot"]["writable"]:
        raise HwError("chip protection is %s: refusing (never change RDP/FAP)" % info["prot"]["text"])
    if info["opt"]["tzen"]:
        raise HwError("TrustZone is on (TZEN=1): secure flash / secure boot state unknown, refusing to write "
                      "(never change TZEN: that needs an RDP regression = mass erase)")
    if window == "app":
        sp = struct.unpack_from("<I", data, 0)[0]
        if sp > info["opt"]["sram_end"]:
            raise HwError("image initial SP 0x%08x is above this chip's SRAM end 0x%08x (%s): it would fault at reset"
                          % (sp, info["opt"]["sram_end"], "; ".join(info["opt"]["lines"])))
    else:
        cur = be.read(addr, 64)
        if cur[:4] != CP_MAGIC and cur != b"\xff" * len(cur):
            raise HwError("codeplug window 0x%08x on the chip is neither blank nor a codeplug (stock firmware code "
                          "there?): flash the cfw first" % addr)
    found = find_backup(radio, info["uid"])
    if not found:
        raise HwError("no verified backup of this chip (UID %s): run `hw.py --radio %s backup` first"
                      % (info["uid"], radio.key))
    bdir, man = found
    boot_len = radio.app_base - radio.flash_base
    if sha256(be.read(radio.flash_base, boot_len)) != man["bootloader_sha256"]:
        raise HwError("bootloader on the chip differs from backup %s: stop and investigate" % bdir)
    print("backup: %s" % bdir)
    phrase = "FLASH %s" % uid_tag(info["uid"])
    (input_fn or confirm)("Write %s (%d bytes) to 0x%08x-0x%08x on %s UID %s?"
                          % (label, len(data), addr, addr + len(data), radio.name, info["uid"]), phrase)
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    try:
        ok = be.program(tmp, addr)
    finally:
        os.remove(tmp)
    back = be.read(addr, len(data))
    after = chip_info(be, radio)
    boot_after = be.read(radio.flash_base, boot_len)
    problems = []
    if not ok:
        problems.append("probe verify failed")
    if back != data:
        problems.append("read-back differs from the image")
    if after["prot"]["raw"] != info["prot"]["raw"]:
        problems.append("PROTECTION CHANGED: 0x%08x -> 0x%08x" % (info["prot"]["raw"], after["prot"]["raw"]))
    if sha256(boot_after) != man["bootloader_sha256"]:
        problems.append("BOOTLOADER CHANGED")
    if problems:
        raise HwError("; ".join(problems) + " - do not power-cycle; run `hw.py --radio %s recover`" % radio.key)
    be.reset_run()
    print("flashed, verified (probe + read-back), protection and bootloader unchanged; reset")


def cmd_flash(be, radio, a):
    write_app(be, radio, load_image(a.file), os.path.basename(a.file))


def verified_backups(radio, uid):
    """All verified backups of this exact chip, oldest first: [(dir, manifest)]."""
    out = []
    for d in sorted(glob.glob(os.path.join(backup_dir_for(radio, uid), "*"))):
        m = os.path.join(d, "manifest.json")
        if os.path.exists(m):
            with open(m) as f:
                man = json.load(f)
            if man.get("radio") == radio.key and man.get("uid") == uid and man.get("verified"):
                out.append((d, man))
    return out


def load_backup_flash(d, man, radio):
    with open(os.path.join(d, "flash_full.bin"), "rb") as f:
        flash = f.read()
    if sha256(flash) != man["files"]["flash_full.bin"]["sha256"] or len(flash) != radio.flash_size:
        raise HwError("%s/flash_full.bin is corrupt (hash / size differs from the manifest)" % d)
    return flash


def cmd_recover(be, radio, a):
    if getattr(a, "stock", False):
        if not os.path.exists(radio.stock_image):
            raise HwError("stock image missing: %s" % radio.stock_image)
        return write_app(be, radio, load_image(radio.stock_image), radio.stock_label)
    uid = chip_info(be, radio)["uid"]
    if getattr(a, "backup", None):
        with open(os.path.join(a.backup, "manifest.json")) as f:
            d, man = a.backup, json.load(f)
        if man.get("uid") != uid or man.get("radio") != radio.key or not man.get("verified"):
            raise HwError("backup %s is not a verified backup of this chip (%s UID %s)" % (d, radio.key, uid))
    else:
        all_b = verified_backups(radio, uid)
        if not all_b:
            raise HwError("no verified backup of this chip (UID %s); `recover --stock` writes the package image" % uid)
        d, man = all_b[0]                                         # the first one = state before any cfw write
    flash = load_backup_flash(d, man, radio)
    lo, hi = radio.app_base - radio.flash_base, radio.write_end - radio.flash_base
    print("restoring app window 0x%08x-0x%08x from %s (taken %s, app == stock: %s)"
          % (radio.app_base, radio.write_end, d, man.get("created"), man.get("app_matches_stock")))
    write_app(be, radio, flash[lo:hi], "own backup %s" % os.path.basename(d))


def cmd_codeplug(be, radio, a):
    with open(a.file, "rb") as f:
        raw = f.read()
    cp = check_codeplug(radio, raw)
    pad = (radio.cp_end - radio.cp_base) - len(cp)
    print("codeplug %s: %d bytes, model %s; window 0x%08x-0x%08x (rest erased to 0xFF)"
          % (a.file, len(cp), CP_MODELS[radio.cp_model], radio.cp_base, radio.cp_end))
    write_app(be, radio, cp + b"\xff" * pad, "codeplug " + os.path.basename(a.file), window="codeplug")


# ---- bootinfo: what does the bootloader look at in the app area? (static, from a backup; no probe)
OBJDUMP = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")
SW_CRC_PATTERNS = (
    (struct.pack("<II", 0x00000000, 0x77073096), "CRC-32 table (reflected 0xEDB88320)"),
    (struct.pack("<II", 0x00000000, 0x04C11DB7), "CRC-32 table (normal 0x04C11DB7, STM32 CRC unit style)"),
    (struct.pack("<HHH", 0x0000, 0x1021, 0x2042), "CRC-16/CCITT table (0x1021)"),
    (struct.pack("<HHH", 0x0000, 0xC0C1, 0xC181), "CRC-16/IBM table (reflected 0xA001)"),
    (struct.pack("<I", 0x6A09E667), "SHA-256 initial hash (signature / hash check?)"),
)
SW_CRC_WORDS = {0xEDB88320: "CRC-32 polynomial (reflected)", 0x04C11DB7: "CRC-32 polynomial",
                0x00001021: "CRC-16/CCITT polynomial (as word)"}


def disassemble(code, base):
    """objdump the raw Thumb code. Returns [(addr, text)]; [] when objdump is missing."""
    if shutil.which(OBJDUMP) is None:
        return []
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    with os.fdopen(fd, "wb") as f:
        f.write(code)
    try:
        out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "arm", "-M", "force-thumb",
                              "--adjust-vma=0x%x" % base, tmp], capture_output=True, text=True, timeout=300).stdout
    finally:
        os.remove(tmp)
    res = []
    for ln in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s?)+\s+(.*)$", ln)
        if m:
            res.append((int(m.group(1), 16), m.group(2).strip()))
    return res


def bootinfo_report(radio, flash):
    """Static look at the bootloader [flash_base, app_base): which app-area addresses / CRC units it uses.
    Returns (text, summary dict)."""
    base, blen = radio.flash_base, radio.app_base - radio.flash_base
    boot = flash[:blen]
    app_lo, flash_end = radio.app_base, radio.flash_base + radio.flash_size
    alias = 0x04000000 if radio.key == "d890" else 0               # STM32U5 secure flash alias 0x0C000000
    named = {app_lo: "app base (app SP word)", app_lo + 4: "app reset vector", radio.write_end: "end of app window",
             flash_end: "end of flash", radio.cp_base: "cfw codeplug base"}
    if radio.key_addr:
        named[radio.key_addr] = "activation key"
    for a_, lbl in radio.crc_regs:
        named[a_] = lbl
    for w, lbl in SW_CRC_WORDS.items():
        named.setdefault(w, lbl)

    def classify(v):
        if v in named:
            return named[v]
        if alias and v - alias in named and v >= 0x0C000000:
            return named[v - alias] + " (secure alias)"
        for a_, lbl in radio.crc_regs:
            if a_ <= v < a_ + 0x400 and "unit" in lbl:
                return "%s +0x%x" % (lbl, v - a_)
        if radio.key_addr and radio.key_addr <= v < radio.key_addr + 0x800:
            return "activation key page +0x%x" % (v - radio.key_addr)
        for off in (0,) + ((alias,) if alias else ()):
            if app_lo + off <= v < flash_end + off:
                return "app area +0x%x%s" % (v - off - app_lo, " (secure alias)" if off else "")
        return None

    insns = disassemble(boot, base)
    idx = {a_: i for i, (a_, _) in enumerate(insns)}
    lit_refs = {}                                                  # literal address -> [insn index]
    hits = []                                                      # (const, label, where, insn index or None)
    movw = {}
    for i, (a_, t) in enumerate(insns):
        m = re.search(r"@ \((?:0x)?([0-9a-f]+)", t)
        if m and t.startswith(("ldr", "vldr", "adr")):
            lit_refs.setdefault(int(m.group(1), 16), []).append(i)
        m = re.match(r"(?:mov|movs|mov\.w|movs\.w|mvn|mvn\.w)\s+(?:r\d+|ip|lr|sl|fp|sb), #(-?\d+)", t)
        if m:                                                      # Thumb-2 modified immediate (e.g. 0x08100000)
            v = int(m.group(1)) & 0xFFFFFFFF
            v = (~v & 0xFFFFFFFF) if t.startswith("mvn") else v
            lbl = classify(v) if v >= 0x10000 else None
            if lbl:
                hits.append((v, lbl, "mov immediate at 0x%08x" % a_, i))
            continue
        m = re.match(r"movw\s+(r\d+|ip|lr|sl|fp|sb), #(\d+)", t)
        if m:
            movw[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r"movt\s+(r\d+|ip|lr|sl|fp|sb), #(\d+)", t)
        if m and m.group(1) in movw:
            v = int(m.group(2)) << 16 | movw.pop(m.group(1))
            lbl = classify(v)
            if lbl:
                hits.append((v, lbl, "movw/movt at 0x%08x" % a_, i))
    for off in range(0, blen - 3, 4):
        v = struct.unpack_from("<I", boot, off)[0]
        lbl = classify(v)
        if not lbl:
            continue
        refs = lit_refs.get(base + off, [])
        hits.append((v, lbl, "word at 0x%08x%s" % (base + off, "" if refs else " (no code ref found)"),
                     refs[0] if refs else None))
        for r in refs[1:]:
            hits.append((v, lbl, "word at 0x%08x" % (base + off), r))
    tables = [(base + boot.find(pat), lbl) for pat, lbl in SW_CRC_PATTERNS if boot.find(pat) >= 0]

    sp, rst = struct.unpack_from("<II", boot, 0)
    L = ["bootloader 0x%08x-0x%08x (%d KB), SP 0x%08x, reset 0x%08x" % (base, radio.app_base, blen // 1024, sp, rst),
         "disassembly: %s" % ("%d instructions (%s)" % (len(insns), OBJDUMP) if insns else
                              "NOT available (%s missing): literal words only" % OBJDUMP), ""]
    if not hits and not tables:
        L.append("no references to the app area, CRC/HASH units or CRC tables found")
    seen = set()
    for v, lbl, where, i in hits:
        key = (v, where, i)
        if key in seen:
            continue
        seen.add(key)
        L.append("0x%08x  %-38s %s" % (v, lbl, where))
        if i is not None:
            for a_, t in insns[i:i + 5]:
                L.append("      %08x  %s" % (a_, t))
    for a_, lbl in tables:
        L.append("table at 0x%08x: %s" % (a_, lbl))
    labels = [lbl for _, lbl, _, _ in hits]
    summ = dict(
        app_base_ref=any(l.startswith(("app base", "app reset")) for l in labels),
        crc_unit=any(l.startswith(("CRC unit", "HASH unit", "FLASH_CRC")) for l in labels),
        sw_crc_table=bool(tables) or any("polynomial" in l for l in labels),
        key_ref=any(l.startswith("activation key") for l in labels),
        app_end_ref=any(l.startswith(("end of app", "end of flash")) for l in labels),
        app_area_words=sorted({v for v, l, _, _ in hits if l.startswith("app area")}),
        disassembled=bool(insns))
    L += ["", "summary:",
          "  jumps to / reads the app vector table: %s" % ("yes" if summ["app_base_ref"] else "no reference found"),
          "  CRC / HASH unit referenced:           %s" % ("YES" if summ["crc_unit"] else "no"),
          "  software CRC / hash table:            %s" % ("YES" if summ["sw_crc_table"] else "no"),
          "  activation key referenced:            %s" % ("yes" if summ["key_ref"] else "no"),
          "  app window end / flash end words:     %s" % ("YES (size / range check?)" if summ["app_end_ref"] else "no"),
          "  other app-area words:                 %s" % (", ".join("0x%08x" % v for v in summ["app_area_words"][:12])
                                                          or "none"),
          "  verdict: %s" % ("CHECK LIKELY - read the listings above, stop and ask before the first write"
                             if summ["crc_unit"] or summ["sw_crc_table"] or summ["app_end_ref"] else
                             "no sign of an app checksum (static scan; not proof)")]
    return "\n".join(L), summ


def cmd_bootinfo(be, radio, a):
    d = a.dir
    if not d:
        cands = sorted(glob.glob(os.path.join(BACKUP_ROOT, "%s_*" % radio.key, "*", "manifest.json")))
        uids = {os.path.basename(os.path.dirname(os.path.dirname(c))) for c in cands}
        if not cands:
            raise HwError("no %s backup under %s: run backup first, or give the backup dir" % (radio.key, BACKUP_ROOT))
        if len(uids) > 1:
            raise HwError("backups of several %s chips (%s): give the backup dir" % (radio.key, ", ".join(sorted(uids))))
        d = os.path.dirname(cands[0])                              # oldest = state before any write
    with open(os.path.join(d, "manifest.json")) as f:
        man = json.load(f)
    if man.get("radio") != radio.key:
        raise HwError("%s is a %s backup, not %s" % (d, man.get("radio"), radio.key))
    flash = load_backup_flash(d, man, radio)
    text, summ = bootinfo_report(radio, flash)
    print("backup %s (UID %s)" % (d, man.get("uid")))
    print(text)
    with open(os.path.join(d, "bootinfo.txt"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(d, "bootinfo.json"), "w") as f:
        json.dump(summ, f, indent=1)
    print("written: %s/bootinfo.txt" % d)
    return summ


def cmd_calram(be, radio, a):
    """Stock firmware's RAM copy of the RF calibration block (loader D890 FUN_080e0440 / D168 FUN_0807a64c)."""
    sys.path.insert(0, HERE)
    import calmap
    c = calmap.CAL[radio.key]
    info = chip_info(be, radio)
    print_info(radio, info)
    lo, hi = radio.sram
    for off, addr, n in c["ram"]:
        if not (lo <= addr and addr + n <= hi):
            raise HwError("calibration RAM 0x%08x+%d outside SRAM" % (addr, n))
    head = be.read(radio.app_base, 0x400)
    if not os.path.exists(radio.stock_image):
        raise HwError("stock image missing (%s): cannot check the radio runs stock %s" % (radio.stock_image, c["stock"]))
    with open(radio.stock_image, "rb") as f:
        stock_head = f.read(0x400)
    if head != stock_head and not a.any_firmware:
        raise HwError("the app on the chip is not stock %s: the RAM addresses only hold for that version "
                      "(--any-firmware to read anyway)" % c["stock"])
    data = be.read_many([(addr, n) for _, addr, n in c["ram"]])
    span = max(off + n for off, _, n in c["ram"])
    img = bytearray(b"\xff" * c["crc_span"])
    known = set()
    for (off, addr, n), d in zip(c["ram"], data):
        img[off:off + n] = d
        known.update(range(off, off + n))
    rep = dict(radio=c["name"], uid=info["uid"], source="RAM (stock %s %s)" % (c["stock"], c["loader"]),
               regions=[dict(nand_offset="0x%04x" % off, ram="0x%08x" % addr, len=n, data=d.hex())
                        for (off, addr, n), d in zip(c["ram"], data)],
               loader_patched_offsets=sorted("0x%x" % o for o in c["ram_patched"]),
               decoded=calmap.decode(radio.key, img), span=span)
    rep["decoded"].pop("crc32", None)                    # gaps between regions: no CRC from RAM
    rep["decoded"].pop("crc_span", None)
    if a.compare:
        with open(a.compare, "rb") as f:
            nand = f.read()
        diff = [o for o in sorted(known) if o not in c["ram_patched"] and o < len(nand) and nand[o] != img[o]]
        rep["compare"] = dict(file=a.compare, bytes_compared=len(known - c["ram_patched"]), differing=len(diff),
                              first_diffs=["0x%04x: nand %02x ram %02x" % (o, nand[o], img[o]) for o in diff[:16]])
        print("compare with %s: %d bytes, %d differ%s" % (a.compare, rep["compare"]["bytes_compared"], len(diff),
              "" if not diff else " (first: %s)" % ", ".join(rep["compare"]["first_diffs"][:4])))
    d = a.out or os.path.join(BACKUP_ROOT, "%s_%s" % (radio.key, info["uid"]), "calram")
    os.makedirs(d, exist_ok=True)
    stem = os.path.join(d, "calram_%s" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    with open(stem + ".bin", "wb") as f:
        f.write(bytes(img))
    with open(stem + ".json", "w") as f:
        json.dump(rep, f, indent=1)
    dd = rep["decoded"]
    print("UHF tune %s, VHF tune %s" % (dd["uhf_tune_points"], dd["vhf_tune_points"]))
    for band in ("uhf_apc", "vhf_apc"):
        print("%s: usable %s, per level cal %s" % (band.upper(), dd[band]["usable"],
              " ".join("%s=%d" % (k, v["cal"]) for k, v in dd[band]["levels"].items())))
    print("saved %s.bin / .json (bytes outside the loaded regions are 0xFF)" % stem)
    if a.compare and rep["compare"]["differing"]:
        raise HwError("RAM copy differs from %s" % a.compare)
    return rep


def elf_symbol(elf, name):
    try:
        out = subprocess.check_output(["arm-none-eabi-nm", elf], text=True)
    except Exception:
        return None
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    return None


def cmd_check(be, radio, a):
    be.reset_run()
    time.sleep(a.secs)
    be.halt()
    print(be.regs_text())
    icsr, cfsr, hfsr = be.rd32(0xE000ED04)[0], be.rd32(0xE000ED28)[0], be.rd32(0xE000ED2C)[0]
    active = icsr & 0x1FF
    print("ICSR active=%d CFSR=0x%08x HFSR=0x%08x" % (active, cfsr, hfsr))
    ok = not (hfsr & 0x40000000) and active not in (3, 4, 5, 6)
    lift_trap = elf_symbol(a.elf, "g_lift_trap_address") if a.elf else None
    if radio.os_running_addr and (not a.elf or lift_trap):
        run = be.read(radio.os_running_addr, 1)[0]
        print("OSRunning=%d OSTCBCurPtr=0x%08x" % (run, be.rd32(radio.os_tcb_cur_addr)[0]))
        ok = ok and run == 1
    if lift_trap:
        trap = be.rd32(lift_trap)[0]
        print("g_lift_trap_address=0x%08x" % trap)
        ok = ok and trap == 0
    be.resume()
    print("RESULT:", "PASS" if ok else "FAIL")
    if not ok:
        raise HwError("check failed")


# ---- RTT (SEGGER layout: id[16], MaxUp, MaxDown, then 6-word buffer descriptors)
RTT_ID = b"SEGGER RTT"


def rtt_parse_cb(cb):
    if not cb.startswith(RTT_ID):
        raise HwError("no RTT control block there (id %r)" % cb[:16])
    max_up, max_down = struct.unpack_from("<ii", cb, 16)
    if not (0 < max_up <= 16 and 0 <= max_down <= 16):
        raise HwError("RTT control block looks corrupt (MaxUp %d MaxDown %d)" % (max_up, max_down))
    ups = []
    for i in range(max_up):
        name, buf, size, wr, rd, flags = struct.unpack_from("<6I", cb, 24 + 24 * i)
        ups.append(dict(name=name, buf=buf, size=size, wr=wr, rd=rd, flags=flags, rd_addr=0))
    return ups


def rtt_find(be, radio, elf=None):
    if elf:
        addr = elf_symbol(elf, "_SEGGER_RTT")
        if addr is None:
            raise HwError("%s has no _SEGGER_RTT: build with `make DEBUG=1`" % elf)
        return addr
    lo, hi = radio.sram
    ram = be.read(lo, hi - lo)
    i = ram.find(RTT_ID + b"\0")
    if i < 0:
        raise HwError("no 'SEGGER RTT' block in SRAM: not a DEBUG=1 build, or dbg_init() not reached yet")
    return lo + i


def rtt_poll(be, radio, cb_addr, channel=0):
    """Read the unread bytes of an up channel and advance its read pointer. Returns bytes."""
    cb = be.read(cb_addr, 24 + 24 * 2)
    ups = rtt_parse_cb(cb)
    if channel >= len(ups):
        raise HwError("RTT channel %d not present" % channel)
    u = ups[channel]
    lo, hi = radio.sram
    if not (lo <= u["buf"] and u["buf"] + u["size"] <= hi) or u["size"] == 0:
        raise HwError("RTT buffer 0x%08x+%d not in SRAM" % (u["buf"], u["size"]))
    wr, rd, size = u["wr"], u["rd"], u["size"]
    if wr >= size or rd >= size:
        raise HwError("RTT offsets out of range (wr %d rd %d size %d)" % (wr, rd, size))
    if wr == rd:
        return b""
    if wr > rd:
        data = be.read(u["buf"] + rd, wr - rd)
    else:
        parts = [(u["buf"] + rd, size - rd)] + ([(u["buf"], wr)] if wr else [])
        data = b"".join(be.read_many(parts))
    be.write_ram32(cb_addr + 24 + 24 * channel + 16, wr)       # RdOff
    return data


def cmd_log(be, radio, a):
    cb = rtt_find(be, radio, a.elf)
    print("RTT control block at 0x%08x" % cb, file=sys.stderr)
    while True:
        data = rtt_poll(be, radio, cb, a.channel)
        if data:
            sys.stdout.write(data.decode("latin-1"))
            sys.stdout.flush()
        if not a.follow:
            break
        time.sleep(a.interval)


def cmd_server(be, radio, a):
    c = server_command(radio, a.probe, a.speed, a.serial)
    print(" ".join("'%s'" % x if " " in x else x for x in c))
    if a.probe == "jlink":
        print("# GDB: target extended-remote :2331   RTT telnet: localhost:19021   "
              "(J-Link keeps SWD under reset if NRST is wired)")
    else:
        print("# GDB: target extended-remote :3333   TCL: 6666 (hw.py)   telnet: 4444")


def cmd_read(be, radio, a):
    addr = int(a.addr, 0)
    for i, w in enumerate(be.rd32(addr, int(a.count, 0))):
        print("0x%08x: 0x%08x" % (addr + 4 * i, w))


COMMANDS = {
    "info": cmd_info, "backup": cmd_backup, "verify-backup": cmd_verify_backup, "flash": cmd_flash,
    "recover": cmd_recover, "codeplug": cmd_codeplug, "calram": cmd_calram, "bootinfo": cmd_bootinfo,
    "check": cmd_check, "log": cmd_log, "server": cmd_server, "read": cmd_read,
    "reset": lambda be, r, a: print(be.reset_run()),
    "halt": lambda be, r, a: (be.halt(), print(be.regs_text())),
    "resume": lambda be, r, a: print(be.resume()),
}


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--radio", choices=sorted(RADIOS), default=os.environ.get("HW_RADIO", "d890"))
    ap.add_argument("--probe", choices=("jlink", "stlink", "jlink-openocd"), default=os.environ.get("HW_PROBE", "jlink"))
    ap.add_argument("--speed", type=int, default=4000, help="SWD kHz (J-Link)")
    ap.add_argument("--serial", help="J-Link serial number (two probes connected)")
    ap.add_argument("--jlink-device", help="override the J-Link device name")
    ap.add_argument("--jlink-script", help="J-Link script file (-JLinkScriptFile), e.g. custom connect sequence")
    ap.add_argument("--nogui", action="store_true",
                    help="J-Link: suppress dialogs (only after a first manual connect showed none)")
    ap.add_argument("--tcl-port", type=int, default=6666, help="OpenOCD TCL port")
    sub = ap.add_subparsers(dest="c", required=True)
    sub.add_parser("info")
    b = sub.add_parser("backup"); b.add_argument("dir", nargs="?")
    v = sub.add_parser("verify-backup"); v.add_argument("dir", nargs="?")
    f = sub.add_parser("flash"); f.add_argument("file")
    r = sub.add_parser("recover")
    r.add_argument("--backup", help="restore this backup dir (default: the first verified backup of the chip)")
    r.add_argument("--stock", action="store_true", help="write the stock package image instead of the own backup")
    cp = sub.add_parser("codeplug"); cp.add_argument("file", help="cfw_cps binary export (.d8cp)")
    cr = sub.add_parser("calram"); cr.add_argument("--compare", help="cal_dump.py .bin to compare with")
    cr.add_argument("--out", help="output dir (default hw_backups/<radio>_<uid>/calram)")
    cr.add_argument("--any-firmware", action="store_true", help="read even if the app is not the stock version")
    bi = sub.add_parser("bootinfo"); bi.add_argument("dir", nargs="?", help="backup dir (default: the only chip's oldest)")
    c = sub.add_parser("check"); c.add_argument("--elf"); c.add_argument("--secs", type=float, default=3)
    lg = sub.add_parser("log"); lg.add_argument("--elf"); lg.add_argument("--follow", "-f", action="store_true")
    lg.add_argument("--interval", type=float, default=0.2); lg.add_argument("--channel", type=int, default=0)
    sub.add_parser("server")
    for n in ("reset", "halt", "resume"):
        sub.add_parser(n)
    rd = sub.add_parser("read"); rd.add_argument("addr"); rd.add_argument("count", nargs="?", default="1")
    return ap


def main(argv=None):
    a = build_parser().parse_args(argv)
    radio = RADIOS[a.radio]
    be = None if a.c in ("server", "bootinfo") else make_backend(a, radio)
    try:
        COMMANDS[a.c](be, radio, a)
    except HwError as e:
        sys.exit("ERROR: %s" % e)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
