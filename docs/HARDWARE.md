# Hardware debugging: D890UV and D168UV with a SEGGER J-Link

Goal: flash, run, debug and recover both radios over SWD without ever breaking something that cannot be undone.
Everything here was prepared and tested **offline** (no probe connected yet). The first real connection follows
section 6 step by step.

| Radio | MCU | App base (we write here only) | Bootloader (never written) | Protection |
|---|---|---|---|---|
| D890UV | STM32U575 (Cortex-M33, 2 MB) | `0x0800C000`–`0x081E3FFF` | `0x08000000`–`0x0800BFFF` (incl. activation key `0x0800A000`) | RDP in `FLASH_OPTR` |
| D168UV | Artery AT32F403A (Cortex-M4F, 1 MB) | `0x08004000`–`0x080FFFFF` | `0x08000000`–`0x08003FFF` | FAP in `FLASH_USD` |

Hard rules (enforced in `tools/hw/hw.py`, not only policy):
- Never write the bootloader, option bytes / user system data (USD), RDP or FAP. Never mass-erase.
- **Never lower read protection.** RDP 1→0 (STM32) or FAP off (AT32) mass-erases the chip, factory bootloader
  included. Then the official USB updater is gone for good.
- Nothing writes flash except `hw.py flash` / `recover` / `codeplug`. GDB `load` is blocked (OpenOCD
  `gdb_flash_program disable`, `hook-load` in `tools/hw/gdb/*.gdb`).
- RF tests only into a dummy load or attenuators (section 9).

## 1. What to buy

| Item | Notes |
|---|---|
| **SEGGER J-Link EDU** (hobby / non-commercial) or **J-Link BASE** | Supports both STM32U575 and Artery AT32F403A natively. All SEGGER tools: Commander, GDB server, RTT. **EDU Mini** also works: it is cheap, with a 10-pin 1.27 mm connector, but it cannot supply power, which we don't want anyway. |
| Adapter / wires | J-Link 20-pin 2.54 mm: female dupont jumpers are enough. EDU Mini: a 10-pin 1.27 mm Cortex-Debug breakout board. |
| Thin wire (0.1–0.2 mm enamelled or 30 AWG Kynar), flux, fine tip | For soldering to pads / MCU pins. Add strain relief with hot glue or Kapton tape. Optional: 2.54 mm header glued to the case. |
| Continuity meter | To find the pads (section 3). |
| 50 Ω dummy load(s), ≥10 W | **Always on the antenna port** of a radio running test firmware. |
| 2 × 30 dB (or 40 dB) power attenuators, ≥10 W, plus a step attenuator | For radio-to-radio tests (section 9). |
| Bench supply 7.4–8.4 V, current limit ~1.5 A | In place of the battery: a stuck PTT or a short cannot burn anything. |
| (optional) ST-Link V3MINIE | Works for the D890 through OpenOCD (`--probe stlink`). Not recommended for the D168: ST-Link firmware may refuse non-ST parts. |

## 2. Host software (Debian 13). Install it yourself.

1. **SEGGER J-Link Software and Documentation Pack**, Linux 64-bit `.deb`. Download it from segger.com (you
   accept the licence there), then install it:
   ```bash
   sudo apt install ./JLink_Linux_x86_64.deb
   ```
   It provides `JLinkExe`, `JLinkGDBServerCLExe`, `JLinkRTTViewerExe` and the udev rules. Re-plug the probe after
   installing.
2. GDB for ARM:
   ```bash
   sudo apt install gdb-multiarch
   ```
3. Optional: OpenOCD, for the ST-Link path or for `--probe jlink-openocd`:
   ```bash
   sudo apt install openocd
   ```
   The D168 needs `target/artery/at32f4x.cfg` and the `artery` flash driver. They only exist in recent upstream
   OpenOCD (2025+), and that driver is single-bank, so it does not cover `0x08080000`+. Without them, the D168
   OpenOCD config falls back to a plain Cortex-M4. Reads, RAM, RTT and GDB still work, but `hw.py` refuses flash
   writes. **Use the J-Link tools for D168 writes.**
4. Optional IDE: VS Code + the *Cortex-Debug* extension. Configs are in `cfw/.vscode/launch.json` and
   `D168UV/cfw/.vscode/launch.json`. They only attach (no flashing) and show RTT.

Check the install:
```bash
JLinkExe -?
```
```bash
python3 tools/hw/hw.py --radio d168 server
```
The second command only prints the GDB-server command line.

## 3. Wiring (both radios)

Five wires: **SWDIO, SWCLK, NRST, GND, VTref**. Optionally SWO.

| Signal | J-Link 20-pin | EDU Mini 10-pin | D890UV (STM32U575) | D168UV (AT32F403A) |
|---|---|---|---|---|
| VTref (sense only) | 1 | 1 | 3.3 V rail (VDD) | 3.3 V rail (VDD) |
| SWDIO | 7 | 2 | PA13 | PA13 (= **PF1 key**) |
| SWCLK | 9 | 4 | PA14 (= **PF2 key**) | PA14 (= **PF2 key**) |
| nRESET | 15 | 10 | NRST | NRST |
| GND | 4/6/8/…/20 | 3/5/9 | GND (shield / battery −) | GND |
| SWO (optional) | 13 | 6 | PB3 (also the RF 3-wire SCK: leave unconnected) | PB3 (RF bus too: leave unconnected) |

### Finding the pads

There are no published test-point maps for these boards, and none were found in the packages here. Do not guess:
probing a wrong pad on a powered radio can kill it.

1. Read the MCU marking. D890: STM32U575 + package letter, e.g. **V** = LQFP100, **Z** = LQFP144. D168:
   AT32F403A**V**GT7 = LQFP100. The firmware uses port E, so both are at least 100-pin.
2. Look up the pin numbers in the datasheet for that package: ST DS13737 (STM32U575), Artery AT32F403A datasheet.
   For a 100-pin package laid out like STM32F1/F4 LQFP100, these are usually **NRST = pin 14, PA13 = pin 72,
   PA14 = pin 76**. **Verify them in the datasheet** for the exact part: U5 pinouts differ from older STM32 parts.
3. With the radio **unpowered** (battery off), put one meter probe on the MCU pin. Use a sharp needle on the pin
   foot, not the pad between pins. Walk the other probe over the nearby test points, vias and the programming /
   accessory connector pads until it beeps (< 1 Ω). A beep through a ~1–10 kΩ resistor to a key pad is the side
   key on PA13/PA14. Its pad is usable too, but the key pull-up / RC filter may then limit SWD speed.
4. Find NRST the same way. It usually has a 100 nF cap to GND near the MCU, and sometimes a reset IC.
5. Use GND from a large copper area or the shield. Take VTref from a 3.3 V regulator output or an MCU VDD pin:
   with the radio on, measure 3.3 V to GND before you connect it.
6. Solder thin wires, add strain relief, and check that no two adjacent pins are bridged (meter between
   neighbours). Write down what you found: add a photo + table to this file.

### Level, power and key notes

- Both MCUs run at 3.3 V. J-Link adapts to VTref (1.2–5 V). VTref is **sense only**: do not enable the J-Link 5 V
  target supply (pin 19). The radio is powered by its own battery or bench supply.
- Power order: radio off → connect the probe → power the radio → connect. Disconnect in reverse order.
- PA13/PA14 are also side keys (D890: PF2 = PA14; D168: PF1 = PA13, PF2 = PA14). The keys pull the line to GND.
  **Never press PF1/PF2 while the probe is attached**: that shorts SWDIO/SWCLK and the session drops. With a cfw
  `KEEP_SWD=1` / `DEBUG=1` build, those keys do nothing anyway.
- The stock firmware (both radios) turns these pins into key inputs once it boots, so SWD dies after boot. Wire
  **NRST**: J-Link then *connects under reset*, catching the CPU before the firmware takes the pins. J-Link does
  this automatically when the normal attach fails ("Trying connect under reset"). The OpenOCD configs use
  `connect_assert_srst`.
- If SWD is flaky with long wires or the key RC filters, lower the speed with `--speed 1000` (or
  `adapter speed 1000` in OpenOCD).
- Keep the USB/PC programming cable unplugged while the probe is attached. This avoids ground loops and the
  updater fighting the debugger.

## 4. Tools

`tools/hw/hw.py` handles both radios and both probes. Global options:
- `--radio d890|d168` (default d890, or env `HW_RADIO`).
- `--probe jlink|stlink|jlink-openocd` (default jlink, or env `HW_PROBE`).
- `--speed`, `--serial <J-Link S/N>` (two probes), `--jlink-device`, `--jlink-script`, `--nogui`.

| Command | Writes? | What |
|---|---|---|
| `info` | no | CPU check (refuses a wrong `--radio`), IDCODE, flash size, UID, RDP/FAP, PC/SP. Option bits: D890 TZEN, IWDG_SW / IWDG_STOP / IWDG_STDBY, WWDG_SW, DUALBANK / SWAP_BANK, boot bits; D168 nWDT_ATO_EN and EOPB0 (SRAM 96 / 224 KB) |
| `backup [dir]` | no | Full flash read **twice** (must match), OTP / option registers / USD, SHA-256 manifest → `hw_backups/<radio>_<uid>/<time>/` |
| `verify-backup [dir]` | no | Reads the flash again and compares it with the backup of *this* chip |
| `flash <file.bin\|.elf>` | **app only** | See guards below |
| `recover [--backup D]` | **app only** | Restores the whole app window (D890 `0x0800C000`–`0x081E3FFF`, D168 `0x08004000`–`0x080FFFFF`) from **this chip's own** first verified backup (or backup dir `D`): stock app, cfw settings / codeplug pages and any factory data in free flash come back as they were |
| `recover --stock` | **app only** | Writes the stock package app instead: D890 V1.05 `.CDD`, D168 V1.07 flat image `D168UV/fw/d168uv_v107_08004000.bin` |
| `codeplug <file.d8cp>` | **codeplug window only** | Writes a `cfw_cps` binary export into D890 `0x08180000`–`0x081DFFFF` / D168 `0x080A0000`–`0x080FFFFF` (rest of the window erased to 0xFF). Checks magic `D8CP`, version, length, CRC-32 and the model (u16 @18: 0 D890, 1 D168) against `--radio`. The window on the chip must be blank or already hold a codeplug, so it never overwrites stock code (stock reaches into that window on both radios: flash the cfw first) |
| `calram [--compare F]` | no (SRAM reads) | RF calibration copy the **stock** firmware loaded into RAM (stock V1.05 / V1.07 must be running; refused otherwise), saved + decoded, compared byte for byte with a `cal_dump.py` dump (see `cfw/docs/CALIBRATION.md`) |
| `bootinfo [backup dir]` | no (no probe) | Static scan of the bootloader in a backup's `flash_full.bin` (objdump): references to the app vector table, CRC / HASH units (D168: also the flash CRC check registers), software CRC tables, the activation key (D890), the app window / flash end. Writes `bootinfo.txt` / `.json` into the backup dir |
| `check [--elf E] [--secs N]` | no | Reset, run N s, halt. Fault registers, PC, uC/OS markers (D890 stock/lifted), lift trap |
| `log [--elf E] [-f]` | RAM only | Reads the RTT log of a cfw `DEBUG=1` build (section 7) |
| `server` | no | Prints the GDB-server command (JLinkGDBServerCLExe :2331, or OpenOCD :3333/TCL 6666) |
| `read <addr> [n]`, `reset`, `halt`, `resume` | no | |

Guards for `flash` / `recover` / `codeplug`, in this order. Nothing is written until all of them pass:
1. The image goes **exactly** at the window base and ends inside the window: app D890 < `0x081E4000` (table cache
   above), D168 < `0x08100000`; codeplug as in the table.
2. App images: vector table SP inside this radio's SRAM, reset handler a Thumb address inside the image. This catches
   a D890 image sent to a D168 and vice versa. Codeplugs: header, CRC and model as above.
3. CPUID matches the radio. RDP must be Level 0 (`0xAA`) / FAP off. Anything else is refused, never "fixed".
   D890 with **TZEN = 1** is refused. D168: the image's initial SP must fit the SRAM that EOPB0 configures (96 KB
   unless EOPB0 = `0xFE`; the stock SP `0x2001F670` needs 224 KB). Codeplug: the chip's window must be blank or a
   codeplug.
4. A **verified backup of this exact chip UID** exists, and the bootloader on the chip still hashes the same as in
   that backup.
5. Typed confirmation `FLASH <last 8 hex digits of UID>`. There is no `--yes`. Type the phrase yourself.
6. After writing: the probe's verify, plus hw.py's own read-back compare, plus protection register and bootloader
   hash unchanged. Any difference: stop and tell you to run `recover`. Only then reset + run.

The J-Link backend also refuses to send `erase`, `unlock`, `exec`, `RSetType`, `loadbin` and any memory write
outside SRAM. Every J-Link command file is checked line by line before `JLinkExe` runs.

Offline tests (fake chip behind a fake JLinkExe and a fake OpenOCD; no probe needed):
```bash
python3 -m pytest tools/hw/tests -q
```

## 5. Custom firmware debug builds

Both cfw trees (D890: `cfw/`, D168: `D168UV/cfw/`, which shares `cfw/` sources):

```bash
make DEBUG=1
```
This builds into `build_dbg/`. It implies `KEEP_SWD=1`: SWD pins stay SWD, so PF2 (D890) or PF1+PF2 (D168) do
nothing. It also enables the RTT log (`hal/dbg.h`: `DBG("x=%d\n", x)`). The D168 build also sets
`DEBUG_CTRL.SLEEP_DEBUG`, so the probe can read RAM while the main loop sits in WFI.

```bash
make KEEP_SWD=1
```
Keeps SWD alive without the log.

```bash
make HW=1 DEBUG=1
```
A real-radio build (no settings/codeplug writes, digital TX off) with SWD + log. Its output goes to `build_hw_dbg/`
(both radios; the D890 `HW=1` build has its own `build_hw/` since 2026-11-22).

```bash
make DEBUG=1 gdb
```
Starts gdb-multiarch on the ELF with `tools/hw/gdb/<radio>.gdb` and attaches to `:3333` (OpenOCD). For
JLinkGDBServer use `make DEBUG=1 gdb GDBPORT=2331`. Start the server first; `hw.py server` prints its command. The
gdb script breaks on `hardfault_c` and blocks `load`. It adds `faults` (CFSR/HFSR/MMFAR/BFAR), `rh`/`rhj`
(reset + halt, OpenOCD / J-Link) and `rtt` (OpenOCD RTT server on :19021).

Release builds (`make`, `make HW=1`) are unchanged: `DBG()` compiles to nothing, so there is no RTT block.

## 6. First connection, per radio (read-only first, always in this order)

Do this once per radio (four radios = four UIDs = four backups). The examples use the D168; for the D890 use
`--radio d890`.

1. Dummy load on the antenna, bench supply on, radio **off**, wires connected, J-Link plugged in.
2. **First contact by hand, so you see any dialog.** Power the radio on and run:
   ```bash
   JLinkExe -device AT32F403AVGT7 -if SWD -speed 1000
   ```
   For the D890 use `-device STM32U575ZI`; any STM32U575xI name has the same 2 MB flash. Type `connect`.
   **If J-Link ever asks to unsecure / unlock / erase the device: answer NO.** It would mass-erase the bootloader.
   Then type `exit`.
3. Read the chip state:
   ```bash
   python3 tools/hw/hw.py --radio d168 info
   ```
   It must report the right CPU, a UID, and protection *open* (RDP 0 / FAP off). If it says RDP 1 / FAP ON: stop.
   No backup or SWD flashing is possible on that unit; only the official USB updater works.
4. Back up the flash:
   ```bash
   python3 tools/hw/hw.py --radio d168 backup
   ```
   It must say `read twice identical: True, bootloader present: True ... VERIFIED`. `app == stock: True` means the
   radio runs stock V1.05 / V1.07. False is fine if it has another version, but note it.
5. Verify the backup against the chip:
   ```bash
   python3 tools/hw/hw.py --radio d168 verify-backup
   ```
   Then copy `hw_backups/` somewhere off this machine. It holds the only copy of each radio's bootloader (and, on
   the D890, its UID-bound activation key).
6. **RF calibration**, while the radio still runs stock: dump it over the stock USB PC link and keep it
   (`cfw/docs/CALIBRATION.md`; D168: `D168UV/cfw/docs/CALIBRATION.md`). Read-only; the cfw has no TX without it:
   ```bash
   python3 tools/hw/cal_dump.py --radio d168 --port /dev/ttyACM0 --tag <serial>
   ```
   ```bash
   python3 tools/hw/hw.py --radio d168 calram --compare cal_backups/cal_d168_<serial>.bin
   ```
   The second command is the SWD cross-check: the stock firmware's RAM copy must match the dump.
   "copy in use = NONE": stop, do not plan any TX with the cfw on that radio.
7. Check whether the bootloader checks the app (no probe needed, works on the backup):
   ```bash
   python3 tools/hw/hw.py --radio d168 bootinfo
   ```
   Verdict "CHECK LIKELY" (CRC / HASH unit, CRC table, app-end words): stop and look at the listing before any write.
   Also read the `info` watchdog lines: a hardware watchdog (D890 IWDG_SW = 0, D168 nWDT_ATO_EN = 0) needs a cfw that
   refreshes it (audit F3).
8. Only now, the first write: put back what is already there (proves the write path end to end):
   ```bash
   python3 tools/hw/hw.py --radio d168 recover
   ```
   This restores the app window from the backup you just made. The radio must boot exactly as before.
9. Flash the cfw:
   ```bash
   make HW=1 DEBUG=1
   ```
   Run that in the cfw dir first, with the emulator run passing. Then:
   ```bash
   python3 tools/hw/hw.py --radio d168 flash ../../D168UV/cfw/build_hw_dbg/cfw168.elf
   ```
   ```bash
   python3 tools/hw/hw.py --radio d168 log --elf ../../D168UV/cfw/build_hw_dbg/cfw168.elf -f
   ```
   The log should show `cfw D168UV boot`.
10. Codeplug (optional, cfw only): export the binary with `cfw_cps` (`.d8cp`, model of this radio), then
   ```bash
   python3 tools/hw/hw.py --radio d168 codeplug my.d8cp
   ```

## 7. Logs (RTT)

`make DEBUG=1` places a SEGGER-RTT-compatible control block `_SEGGER_RTT` in RAM: one up channel, 2 KB, which
drops data when full, so the radio never waits for the probe. Ways to read it:
- `hw.py --radio X log [--elf E] [-f]` polls memory through the active probe. With `--elf` it uses the symbol;
  without, it scans SRAM for the `SEGGER RTT` id. It works with J-Link and OpenOCD, needs no extra tool, and never
  halts the CPU. It writes only the 4-byte read pointer, inside SRAM.
- J-Link: `JLinkRTTViewerExe`. Or, while `JLinkGDBServerCLExe` runs, `nc localhost 19021`.
- OpenOCD: in GDB type `rtt` (from the gdb script), then `nc localhost 19021`.

The format was checked in the Renode emulator: the D890 `DEBUG=1` image boots and writes `cfw D890UV boot\n` into
the block, and the unit tests parse that captured RAM.

## 8. Recovery ladder

1. cfw crashes, hangs or misbehaves: `hw.py --radio X recover` restores the app window from this chip's first
   backup (`--stock` for the package image). The connect under reset gets in even when the firmware took the SWD pins.
2. SWD will not attach at all: check VTref (3.3 V?), NRST wiring and GND, and lower `--speed 500`. Make sure no
   side key is pressed or stuck.
3. No SWD (broken wires, protection set): use the **official USB updater**. D890: `D890UV V1.05 DMR official
   release 260521/` (update instructions PDF, `.CDD`). D168: `D168UV/pkg/D168UV_V1.07 FW` + `A READ FIRST -
   Update Instructions`. Both run on the untouched factory bootloader (update mode: key combo at power-on, as in
   the PDF).
4. Bootloader damaged: only possible if something outside these tools wrote it. Restore it by hand from
   `hw_backups/<radio>_<uid>/<time>/flash_full.bin` (first 48 KB D890 / 16 KB D168), with J-Link Commander and
   only the backup of **that** UID. This is deliberately not in `hw.py`. Ask before doing it.

## 9. RF tests with two radios

- Any radio running test firmware: antenna port into a **50 Ω dummy load** rated ≥10 W (or into attenuators),
  never an antenna.
- Radio to radio: TX radio → **30–40 dB power attenuator** (≥10 W rated) → step attenuator → RX radio. Never
  connect a TX port straight into another radio: 5 W = +37 dBm, and the RX front end wants well under 0 dBm. Aim
  for about −50 to −90 dBm at the receiver: 5 W needs ~90–120 dB total, 0.5 W ~80–110 dB. Start with the most
  attenuation and the lowest power setting.
- Simpler and also fine: each radio on its own dummy load, a metre apart. Leakage from the loads is usually
  enough for a receive test.
- `rf0 Signal`-style tests from the emulator do not replace this. Check new TX code in the emulator first
  (`make emu`, `txrx_loop.py`), then on hardware at low power into the load.
- Transmitting on air with modified firmware must follow your licence and local rules.

## 10. Files

| File | What |
|---|---|
| `tools/hw/hw.py` | the tool (both radios, J-Link + OpenOCD backends, guards) |
| `tools/hw/tests/test_hw.py`, `test_cal.py` | offline tests (`python3 -m pytest tools/hw/tests -q`) |
| `tools/hw/cal_dump.py`, `calmap.py` | RF calibration dump over the stock USB PC link (read-only: PROGRAM / 02 / R / END only), layout + decoder |
| `tools/hw/emu_cal_test.py` | `cal_dump.py` against the stock firmware in Renode (USB-CDC shim via CPU hooks) |
| `cfw/docs/CALIBRATION.md`, `D168UV/cfw/docs/CALIBRATION.md` | calibration: where, dump before flashing, RTT check after |
| `tools/hw/openocd_{d890,d168}_{stlink,jlink}.cfg`, `openocd_common.cfg` | OpenOCD configs (connect under reset, localhost only, no GDB flash writes). `openocd.cfg` = d890 + ST-Link (old name). |
| `tools/hw/gdb/{d890,d168}.gdb` | GDB setup used by `make gdb` |
| `cfw/hal/dbg.[ch]` | RTT log (shared by both cfw trees) |
| `cfw/.vscode/launch.json`, `D168UV/cfw/.vscode/launch.json` | Cortex-Debug attach configs (J-Link, OpenOCD) |
| `hw_backups/<radio>_<uid>/<time>/` | backups + `manifest.json` (created by `hw.py backup`) |
