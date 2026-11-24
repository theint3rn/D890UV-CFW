# RF / TX calibration: read it from the stock radio before flashing the cfw

Every radio has its own factory RF calibration: TX (PA drive per power level and band) and RX (front-end tune points, squelch thresholds and their per-frequency corrections). The
custom firmware does not calibrate anything. It reads the stock calibration block **in place** at every boot and
uses its values exactly like the stock firmware does. This page says where the block is, how to read and keep a
copy of it from the radio **while it still runs the stock firmware**, and how to check that the cfw uses the same
data after flashing.

Everything here only **reads**. Nothing writes the NAND: not the cfw, not `cal_dump.py`, not `hw.py`.

## 1. Where it lives and why the cfw needs it

| | D890UV (STM32U575) |
|---|---|
| Storage | external SPI-NAND (not the MCU flash), two copies |
| Copy A / copy B | `0x04F00000` / `0x04F40000` (B = A + one 256 KB NAND block) |
| Valid copy | ends with `55 55 AA AA` at +`0x3FFFC` **and** its "invalid" marker at +`0x3FDF8` is not `AA AA 55 55` |
| Stock loader | `FUN_080e0440` (stock V1.05): copy A if valid, else B (even an invalid B) |
| cfw loader | `cfw/drivers/dac.c` `dac_cal_load()`: copy A if valid, else B if valid, else **none** |
| Values the cfw uses | +0x000 UHF: APC cal per level at +0x02..+0x05, RX tune points u16 at +0x18/+0x1A/+0x1C (400 / 460 / 520 MHz). +0x100 VHF: APC cal at +0x102..+0x105, tune points at +0x11E/+0x120/+0x122 (136 / 174 / 225 MHz). APC tables one byte per MHz: UHF levels 0-3 at +0xC00/+0xD00/+0xE00/+0xF00 (121 bytes, 400-520 MHz, reference index 40); VHF at +0x1000..+0x1300 (45 bytes, reference index 9). Stock reads more (up to +0x1C0D: deviation, tones, ...), see `tools/hw/calmap.py` |

Flashing the cfw rewrites only the MCU's internal flash. The NAND, and the calibration in it, stay as they are.
But the cfw depends on that block:
- **No valid copy = no TX.** The cfw sets the PA drive (APC DAC) to 0 and refuses PTT (pre-flash audit F5). Without
  calibration a stock-formula APC value can wrap to almost full scale.
- With a valid copy, the APC value is the stock formula, clamped to the highest value the stock tables give in that
  band (F5). The RX tune voltage is interpolated from the tune points.

So read, check and keep a copy **before** the first flash. Then you know what the radio had, and you can check that
the cfw sees the same thing.

## 1b. RX calibration and the other sub-areas

The RX calibration is in the same block as the TX calibration, so the dump in section 2 already contains it:
`cal_dump.py` saves the whole block, and the `.json` lists every sub-area the stock loader reads (`areas`: offset,
size, role, cfw use, data) and decodes the squelch calibration (`decoded.squelch`). Nothing extra to read.

What the RX values do:
- **RX front-end tune points.** Three points per band. The stock code interpolates them into a tuning voltage per
  frequency, which tunes the receiver's input filter onto the channel. Wrong or missing points cost sensitivity
  (deaf radio), especially away from the band centre. D890: UHF tune points drive DAC2 (PA5), VHF tune points go to RF chip reg 0x5C (stock `FUN_0811c8f0` / `FUN_0811cadc`). The cfw uses them (`dac_rx_tune`).
- **Squelch threshold.** Stock `FUN_080e373c` writes RF reg 0x49 = base (UHF +0x10, VHF +0x116, 200-300 MHz +0x124; RF chip 1: +0x12 / +0x118 / +0x126) + level offset (0 / 7 / 13 / 19 / 23 for levels 1-5, stock table 0x0814AE05) + correction table[reference] - table[frequency] (UHF +0x1400 in 5 MHz steps, VHF +0x1500 in 2 MHz steps; chip 1: +0x1600 / +0x1700), minus a temperature term (`FUN_0804a988`). The squelch opens when the RSSI (reg 0x02) reaches that threshold and closes 12 below it (`FUN_080d22f0`). This is the "squelch cal" the emulator NAND did not have.
  The cfw uses this calibration for RF chip 0 (2026-11-23): `drivers/dac.c dac_sql_threshold()` computes the stock
  threshold, `drivers/rf.c` writes it to reg 0x49 on every frequency change, `app/radio.c` opens / closes as
  `FUN_080d22f0` does (3 polls; close below threshold - 12, min 0x46, or noise > 0x50). Without a NAND block the values
  come from `drivers/cal_default.c`. Not ported: the temperature term (stock subtracts it only below 0 degC) and the
  special-mode branch that uses the base alone. Test: `tools/sql_test.py`.
- **RSSI -> dBm.** The cfw uses a fixed conversion. No stock RSSI calibration has been identified (the squelch
  tables above are the closest thing).

| Offset | Size | Kind | What (stock users) | cfw today |
|---|---|---|---|---|
| +0x0 | 0x2A | tx+rx | UHF struct: APC cal per level +0x02..+0x05 (FUN_0811a75c), squelch base u16 +0x10 (RF chip 0) / +0x12 (chip 1) (FUN_080e373c -> reg 0x49), RX tune points u16 +0x18/+0x1A/+0x1C 400/460/520 MHz (FUN_08049fc4); +0x28 = a mode byte the loader zeroes if > 0x12 | APC, tune, squelch (chip 0): used |
| +0x100 | 0x2C | tx+rx | VHF struct: APC cal +0x102..+0x105, squelch base u16 +0x116 (chip 0) / +0x118 (chip 1), 200-300 MHz squelch base +0x124 / +0x126, RX tune points +0x11E/+0x120/+0x122 136/174/225 MHz (FUN_0804a0a0, VHF tune goes to RF reg 0x5C) | APC, tune, squelch (chip 0): used |
| +0x200 | 0xA | ? | RAM 0x20013484 (many users) | not used |
| +0x300 | 0x21 | ? | RAM 0x200151AE (FUN_0803f208, FUN_080613a0, FUN_08117b3c) | not used |
| +0x400 | 0x27 | ? | RAM 0x200151E1; floating-point pair math in FUN_0803c020 (+0x0A/+0x0E u16 pairs); loader forces +0x06/+0x07 = 01 F4 | not used |
| +0x500 | 0x27 | ? | RAM 0x20015208, as +0x400 for the other band | not used |
| +0x600 | 0x8 | ? | RAM 0x20014EEE (FUN_080e4370, FUN_0811a588) | not used |
| +0x700 | 0x12 | ? | RAM 0x200151CF (FUN_0803c020, TX path FUN_08117b3c..) | not used |
| +0x800 | 0xC | ? | RAM 0x2001AE3C | not used |
| +0x900 | 0xC | ? | RAM 0x2001AE56 | not used |
| +0xA00 | 0xC | ? | RAM 0x2001AE48 | not used |
| +0xB00 | 0xC | ? | RAM 0x2001AE62 | not used |
| +0xC00 | 0x400 | tx | UHF APC tables, 4 levels x 0x100, 121 bytes each, 1 MHz steps 400-520 MHz, ref index 40 | used |
| +0x1000 | 0x400 | tx | VHF APC tables, 4 levels x 0x100, 45 bytes (136-174 MHz, 220-225 MHz from index 39), ref index 9 | used |
| +0x1400 | 0x19 | rx | squelch frequency correction, UHF, RF chip 0: 25 bytes, 5 MHz steps from 400 MHz, ref index 8 (440 MHz) (FUN_080e373c) | used |
| +0x1500 | 0x19 | rx | squelch frequency correction, VHF, chip 0: 2 MHz steps from 136 MHz, ref index 5 (146 MHz); index 20 = 200-300 MHz value | used |
| +0x1600 | 0x19 | rx | as +0x1400 for RF chip 1 | NOT used (cfw drives chip 0 only) |
| +0x1700 | 0x19 | rx | as +0x1500 for RF chip 1 | NOT used |
| +0x1800 | 0x1C | ? | RAM 0x20015302 (FUN_0803c020, FUN_08127794) | not used |
| +0x1C00 | 0xE | ? | RAM 0x2001446E | not used |

Kind: tx / rx = identified. "?" = read by the stock loader and used by the stock code named, meaning not identified
yet. The dump keeps these bytes too, so nothing is lost.

### Not used by the cfw yet (stock RX calibration)

- Squelch of RF chip 1: bases +0x12 / +0x118 / +0x126 and tables +0x1600 / +0x1700 (the cfw drives chip 0 only;
  chip 0's squelch calibration is used, see above). Temperature term `FUN_0804a988` not ported.
- Unidentified sub-areas (`?` above): +0x200, +0x300, +0x400 / +0x500 (u16 pairs with floating-point math in `FUN_0803c020`: possibly reference-oscillator / frequency correction), +0x600, +0x700, +0x800..+0xB00, +0x1800, +0x1C00. These need analysis before the cfw can use them.

### Other NAND blocks near the calibration

- `0x04E80000`: **partial calibration copy.** On `END` after a stock CPS *write* session, the stock firmware copies
  the first 0x800 bytes of the copy in use here (`FUN_0812ed60` -> `FUN_0813a3c8`). `cal_dump.py` reads it and
  reports whether it equals the copy in use. It is an extra reference only; the cfw does not read it.
- `0x03480000` / `0x034C0000` (loader `FUN_080e0750`, reads +0x1900..+0x3022): **settings written by the stock
  CPS**. They are in the emulator's CPS codeplug capture and absent from the factory-formatted image, so this is not
  calibration. Only the markers are recorded.
- `0x04F80000` / `0x04FC0000`: radio stun / kill state (`FUN_0813a064`). Not calibration; only the markers are
  recorded.

### The stock firmware rewrites the calibration itself

The block is not read-only for the stock firmware:
- The calibration menu and the PC calibration commands save through `FUN_0813a0f0`. It erases the copy in use and
  rewrites it from RAM.
- A data-marker failure at boot leads to `FUN_080627ec`, which erases **both** copies and writes built-in defaults.
  That would replace the factory values with generic ones.

So dump it while the radio is healthy. Never let a radio reach "factory reset" before you have a dump.

## 2. Before flashing: dump the calibration from the stock radio

1. Charged battery or bench supply, **dummy load on the antenna**. Radio on, normal mode (not the update /
   bootloader key combination). The stock CPS must be closed.
2. Connect the PC link the stock CPS uses. On both radios the stock firmware's PC link is **USB CDC**: the PC
   reply goes out on the USB endpoint (D890 `FUN_0801b7b8`, D168 `FUN_0808c63c`). On Linux it shows up as
   `/dev/ttyACM0`, on Windows as `COMx`.
3. Run, from `D890UV_src/`:
   ```bash
   python3 tools/hw/cal_dump.py --radio d890 --port /dev/ttyACM0 --tag <radio serial number>
   ```
   It sends only `PROGRAM`, `02` (ident), `R` (read) and `END`. After a read-only session the stock `END` writes
   nothing (D890 `FUN_0812ed60`, read state '+'). The radio leaves PC mode / restarts afterwards; that is normal. The one exception is a radio that was remotely stunned: it then
   re-saves its stun state, as it does after any stock CPS read. Its single send path refuses every other frame,
   `W` writes included (`check_frame`, tested). By default it reads both copies, whole blocks: 2 × 256 KB in
   16-byte requests, as the stock CPS does. Add `--quick` to read only the used part (0x2000 bytes) plus the
   markers.
4. Output goes to `cal_backups/`:
   - `cal_d890_<tag>.bin`: the copy in use.
   - `cal_d890_<tag>_B.bin`: what the radio serves for copy B.
   - `cal_d890_<tag>.json`: markers, the copy in use, CRC-32, SHA-256, and the decoded values.
5. Check the printed summary:
   - `copy in use = A` (or `A or B`, see below). **Not** `NONE`.
   - `TX possible with the cfw: True`, and for each band you will use, `usable True`.
   - Tune points are plausible: not 0, not 0xFFFF, rising with frequency.
   - The squelch lines show non-blank bases (no `(blank?)`).
   - Write down the line `CRC-32 of the first 0x2000 bytes: ........`.
6. Run it a second time: the CRC and SHA-256 must be the same. Copy `cal_backups/` off this PC, next to the
   `hw_backups/` of the same radio.
7. Optional cross-check over SWD, while the stock firmware still runs and after `hw.py info` / `backup`
   (`docs/HARDWARE.md` section 6):
   ```bash
   python3 tools/hw/hw.py --radio d890 calram --compare cal_backups/cal_d890_<tag>.bin
   ```
   This reads the copy the stock firmware loaded into RAM, using SRAM reads only. The command refuses to run
   unless the app on the chip is stock V1.05. It must report `0 differ`. The bytes the stock loader changes after
   reading are skipped.

**How the stock `R` command serves the copies.** The handler reads the NAND at any address, with no range limit
(D890 `FUN_0812efdc`, D168 `FUN_080967cc` case 0xb). When the 256 KB block holding the address has no valid
marker, it silently reads the same offset one block higher. So reading copy A returns exactly the copy the firmware
uses, and its markers tell whether that copy is valid. The two possible results:
- `copy in use = A`: A and B read differently. A is valid and in use.
- `copy in use = A or B`: both read identically. Either A is valid and B is an identical copy, or A is invalid and
  the radio redirected to B. The bytes in use are the same either way, and so is the CRC.

## 3. After flashing: the cfw uses the same block

The cfw reads the block in place at boot (`dac_cal_load()`). It needs no copy, transfer or "import".

To check it, flash a `make HW=1 HW_NOTX=1 DEBUG=1` build first, then read the RTT log:
```bash
python3 tools/hw/hw.py --radio d890 log -f
```
The boot log should contain a line
```
cal: copy A crc32 1a2b3c4d span 0x2000 uhf 1 vhf 1
```
or `cal: no valid copy, TX off`. Compare it with the dump:
- The `crc32` must equal `crc32_in_use` in the `.json`.
- The copy must be A, or B when the dump said `A or B` and B is valid.

The CRC is CRC-32 (zlib / IEEE 802.3) over the first 0x2000 bytes of the copy in use, the same bytes
`calmap.crc()` hashes.

**Status 2026-11-23:** the line is in the cfw (`hal/safety.c cal_report()`, called by `dac_cal_load()` right after
choosing the copy; RTT in DEBUG=1 builds). The Info screen shows the same: `Cal A 1A2B3C4D U1 V1` or
`Cal none: TX off`. Emulator test: `tools/cal_crc_test.py` (CRC = `calmap.crc()` on `emu/images/synth.pages`, and
the no-block case). `hw.py calram` before flashing stays the cross-check on the stock firmware.

## 4. If the block is invalid or missing

- **Do not transmit** with this radio on the cfw. The cfw already refuses TX without a valid copy; do not work
  around that.
- Do not try to "repair" the NAND, and do not write a dump back. No tool here writes the NAND, on purpose: the
  block is per-radio factory data.
- Leave the stock firmware on the radio, or restore it (`hw.py --radio d890 recover`, or the official USB updater).
  Stock behaves as before: it reads copy B anyway.
- Re-run `cal_dump.py` once. A one-off read error shows up as a different CRC between two runs.
- A radio with no valid calibration needs a factory / dealer calibration with the stock tools. This project does not
  do that.

## 5. Tests (offline and emulator)

```bash
python3 -m pytest tools/hw/tests -q
```
These are offline tests. A fake stock radio implements the redirect rule; the tests cover the write refusal and
`calram` against a fake chip.

```bash
python3 tools/hw/emu_cal_test.py --radio d890
```
This runs the **stock** firmware in Renode. A USB-CDC shim, made of CPU hooks on the stock CDC receive / send
functions, feeds the stock command parser. Copy A is the emulator's synthetic block, copy B a changed copy. Each
dump must match the expected bytes exactly in three cases: both copies valid, A invalid (served from B), and both
invalid (none).

```bash
python3 cfw/tools/cal_crc_test.py --radio d890     # cfw boot report: copy + CRC-32 vs calmap.crc on synth.pages
python3 cfw/tools/sql_test.py --radio d890         # cfw squelch: reg 0x49 = stock threshold, open / close levels
```
These run the **cfw** in the emulator on `emu/images/synth.pages` (no AnyTone files needed).
