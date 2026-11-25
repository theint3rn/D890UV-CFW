# D890UV RF transceiver — register usage (firmware V1.05)

Status: **part number not confirmed.** This file lists what the stock firmware does with the chip, so a
custom firmware can drive it without a datasheet, and so the part can be matched later.

## Bus

Two identical chips, one 3-wire bus, bit-banged by the MCU:

| Signal | Pin | Note |
|---|---|---|
| SCK | PB3 | data valid on rising edge |
| SDA | PB4 | bidirectional |
| CS# chip 0 | PB5 | `param_3 == 0` in the driver |
| CS# chip 1 | PD3 | `param_3 != 0` |

Frame: CS# low, 8 bits `(reg << 1) | R`, then 16 data bits, MSB first.

| Function | Address | Role |
|---|---|---|
| write reg | `FUN_080e2f64(reg, value16, chip)` | 66+ call sites |
| read reg | `FUN_080e2cac(reg, chip)` → `FUN_080e2da4` 16-bit shift-in | |
| bit out | `FUN_080e2e44(byte)` | |
| init | `FUN_0805ffe8(chip)` | full power-up sequence (below) |
| self-test | `FUN_0801c8f4` | write 0x57 = 0xA888, read back, 100 tries; fail → error code 4 (`DAT_2001490a`) |
| set RF frequency | `FUN_08116674(freq_10Hz, chip)` | regs 0x69:0x68 |
| band (VHF/UHF) tables | `FUN_081155ac(freq, chip)` | threshold `30000000` (×10 Hz = 300 MHz) |
| CTCSS/DCS scan | `FUN_080d5bd4` | reads 0x0B–0x0E |

The emulator model is `emu/models/RfTransceiver.cs` (plain 128 × 16-bit register file, enough for the
self-test).

## Init sequence (`FUN_0805ffe8`)

```
0x50 = 0x0001            power/reset (also first write in the self-test)
0x58 = 0xF1F0            enable (also re-written before RX in FUN_080d5bd4)
0x40 = 0x70E6,0x60FF,0x50F4,0x40E8,0x30DC,0x20CD,0x10C3,0x00B9   8-entry table, index in bits 15:12
FUN_081155ac:  0x53 = 8-entry table (index 15:12), 0x42, 0x43, 0x49   (VHF/UHF + variant)
FUN_0802520c:  0x12 = 0x20D8, 0x13 = 0x00EE
FUN_080e2edc:  0x5B = ((n&7)<<13 | (n&8)<<9) + 0x85
0x41 = 0x711A  0x42 = 0x4A61  0x44 = 0x1490  0x45 = 0x8874  0x46 = 0x006E  0x47 = 0x27FE
0x49 = 0x0050, 0x1078, 0x7034, 0xB0CC     (sub-addressed: several writes to one reg)
0x4D = 0x0000  0x57 = 0xA008  0x5C = 0x0001  0x5D = 0x8611
0x5E = 0x0768  0x5F = 0x0768  0x60 = 0x0300  0x2A = 0x1081  0x30 = 0x5F6B
FUN_0810ea14:  0x11, 0x18, 0x19, 0x1A, 0x1B, 0x24–0x27, 0x2B, 0x32, 0x53 tables
0x19 = 0x0381
FUN_08116480:  0x41 / 0x48 (bandwidth-ish values 0x082E, 0x016E, 0x09EE, 0x0AB6, 0x9826)
FUN_08116674(43512500):  0x69/0x68 = frequency
0x59 = 0x221C  0x29 = 0x1090  0x56 = 0x0000  0x3C = 0x00FC
FUN_0805f640   (0x52 writes)
```

## Register summary (from all write sites in `src/lifted`)

| Reg | Writes | Writers | Probable role |
|---|---|---|---|
| 0x0B–0x0E | read only | `FUN_080d5bd4` | tone scan result. 0x0E bits 9:8 = type (1/3 CTCSS, 2 DCS); 0x0D[12:0] = CTCSS freq (`×25000>>19`); 0x0C[11:0] + 0x0B[10:0] = 23-bit DCS word |
| 0x10 | 6 | `FUN_08115c34`, `FUN_0811a6fc` | `0xA380 \| n`: gain index |
| 0x11, 0x18 | 20 | `FUN_080c1ab0`, `FUN_0810ea14`, … | gain / mode |
| 0x12, 0x13 | 1 each | `FUN_0802520c` | fixed at init |
| 0x14, 0x15 | var | `FUN_081162b8`, `FUN_08116318` | |
| 0x16, 0x17 | var | `FUN_080e23b8`, `FUN_080c1ab0` | |
| 0x1C/0x1D = 0x1E/0x1F | 13 | `FUN_080c39e4` | pairs `0x80xx`/`0x3Exx`, same value to both pairs, table per channel: looks like **I/Q gain/phase (image) calibration** |
| 0x20/0x21 = 0x22/0x23 | 13 | `FUN_080c3d18` | second such pair set (`0x8Cxx`/`0x39xx`) |
| 0x24–0x27, 0x2B, 0x32, 0x53 | 4–36 | `FUN_0810ea14` (mode switch) | filter / AGC tables, different per mode |
| 0x29, 0x2A | init, `FUN_08114b48` | 0x2A = `(a<<12) \| b \| 0x80` | |
| 0x2D, 0x2E, 0x2F | 19 / 42 / 12 | `FUN_080e2b74`, `FUN_080e3188`, `FUN_080e3358`, `FUN_080e3d98`, … | 0x2E takes values with a sub-address in bits 15:13 (`0x8000\|…`, `0xA000\|…`, `0xE000\|…`, table `DAT_081580f6`/`DAT_08158090`): **indirect bank / tone generator** (TX tone, DTMF). 0x2F = `0xN A0F` level steps |
| 0x30 | 6 | init, `FUN_080e4154`, `FUN_0811c83c` | `0x5F00 + n` |
| 0x31 | 5 | `FUN_08010c1c`, `FUN_080e3c20`, `FUN_0811affc` | `n \| 0x1000` |
| 0x3C | 16 | `FUN_080e4870`, `FUN_08115fe0`, … | 0xE0/0xE1/0xFC/0xFD: mode bits (RX/TX enable?) |
| 0x3D | 80 | `FUN_080e4870`, `FUN_08115fe0` | table loads, tables `DAT_0814ff30` / `DAT_08150110` stride 0x20, values with bits 13:12 as sub-index: **filter coefficient download** |
| 0x40 | 8 | init | 8-entry table, index 15:12 |
| 0x48 | 18 | `FUN_08116480`, `FUN_08010c1c`, `FUN_08019b78` | channel bandwidth / deviation (wide/narrow, `(*(ch+8)&3)`) |
| 0x52 | 4 | `FUN_0805f640`, `FUN_08010b2c`, `FUN_08019ab4` | `a<<12 \| b \| 0x40` |
| 0x54, 0x55 | read | `FUN_080d5a50` | status (RSSI/noise?) |
| 0x56 | 27 | many | 0/1/2, 0x60C2, 0x7082, 0xE042: **RX/TX/idle state** (toggled 0→1 to start RX in `FUN_080d5bd4`) |
| 0x57 | 3 | init 0xA008, self-test 0xA888 | scratch/test register; read-back = bus check, not a chip ID |
| 0x58 | 9 | init, `FUN_080d5bd4`, `FUN_080e35c0` | 0xF1F0 / 0x01F0: block enables |
| 0x5B | 1 | `FUN_080e2edc` | 4-bit field cached in `DAT_20000e6c/6d` (per chip) |
| 0x5C | 8 | `FUN_0801a35c`, `FUN_08014460`, `FUN_08115e64` | `cal << 4 \| 0x1000/0x1002` (cal from `DAT_20001d00` / `DAT_20003054`) |
| 0x5E–0x60 | init, `FUN_080e35c0`, `FUN_080e3c68` | 0x0468/0x0569/0x0768, 0x0300–0x0309 | |
| 0x66 | 2 | `FUN_080d5a50` | 0xAA04 / 0xAA05 |
| 0x68, 0x69 | | `FUN_08116674`, `FUN_080d5bd4` | **RF frequency, 32 bit, 0x69 = high half.** Caller passes 10 Hz units, the driver converts it (`FUN_08047e10`, offset/shift) and multiplies by 10 → value is most likely in **Hz** (435.125 MHz → 0x19EF_7A68) |

Registers with no constant-address access in V1.05 (3 sites write a variable register number:
`FUN_080c39b8`, `FUN_080e3188`, `FUN_080e3358`; 2 sites read one: `FUN_08049e10`): 0x00–0x0A, 0x0F, 0x28, 0x2C, 0x33–0x3B, 0x3E, 0x3F (one write, 0x3232),
0x4A–0x4C, 0x4E, 0x4F, 0x51, 0x5A, 0x61–0x65, 0x67, 0x6A–0x7F.

## Reg 0x52: switch outputs, one set per chip

`val = switch << 12 | state | 0x40`; `FUN_080e2f64(reg, val, chip)`, chip 0 = CS PB5, chip 1 = CS PD3 (V1.05 decompile and
emulator traces, 2026-11-24; nothing measured on hardware).

| Chip | Switches | Written by | Use |
|---|---|---|---|
| 1 | 1, 2 | `FUN_08019ab4` | speaker audio: 1 = baseband / digital, 2 = chip 0 analog (stock trace `1 52 2041` on an FM signal); both off for PC7 = chip 1 analog, broadcast, AM, tone |
| 1 | 5, 4 (broadcast: 1, 0); 5, 7, 4 (AM: 1 1 1); 0 | | receiver path of chip 1; 0 = 1 in receive, meaning unknown (bench) |
| 0 | 0, 6, 7, 4 | `FUN_08010b2c` | receive path: VHF 0 1 1 0, UHF 1 0 0 1 |
| 0 | 0, 6, 5, 3 | `FUN_08010b2c` | transmit: 0, 0, 5 only 200-300 MHz, 3 = UHF |

Before 2026-11-24 the cfw wrote switches 1 and 2 to chip 0. Test: `tools/rfswitch_test.py`; full comparison: `RF_COMPARE.md`.
The D168UV has one RF chip, its switches differ (`RF_COMPARE.md`).

## Comparison with known parts

| Part | Frequency registers | Bus | Match? |
|---|---|---|---|
| Beken BK4819 | 0x38 (low) / 0x39 (high), 10 Hz units | 3-wire, `reg` then 16 bit | **no**: 0x38/0x39 never written; tone-scan results are at 0x68/0x69 in the BK4819, here 0x68/0x69 are the frequency |
| Beken BK4829 | (no public datasheet found) | 3-wire | unknown |
| Auctus AT1846S / RDA1846 | 0x29 (high) / 0x2A (low), 1/16 kHz units | I²C or 3-wire | **no**: 0x29/0x2A get fixed values (0x1090 / 0x1081) at init |
| Beken BK4811B | REG113/REG114 = N_RF (PLL word, not Hz) | | no |

Conclusion: the frame format is the common Beken/Auctus 3-wire style, but the register map matches none of
the public maps. A frequency word in plain **Hz**, I/Q calibration pairs (0x1C–0x23) and big coefficient
downloads (0x3D, 0x2E) point to a **newer single-chip low-IF/zero-IF transceiver with an internal DSP**,
most likely a Chinese part made for DMR radios.

To identify it: (1) read the marking under the RF shield; (2) on real hardware, after `hw.py backup`, use a
**read-only** SWD session to call the read routine for regs 0x00–0x0F (chip-ID registers are usually there)
— no flash writes needed.

## What this means for modulation

* Analog FM, CTCSS/DCS and tones are generated **in the chip** (0x2E bank, 0x0B–0x0E tone decoder).
* DMR 4FSK TX works in the stock radio, so (inference) the chip's FM modulator takes a **DC-coupled
  baseband** from outside (from the SCT3288 / codec path), and RX gives a discriminator output good enough
  for 4800-baud 4FSK. The per-mode tables loaded by `FUN_0810ea14` (0x24–0x27, 0x2B, 0x32, 0x53) are the
  likely analog-vs-digital filter switch.
  Then C4FM (P25 Ph1, YSF), NXDN 4FSK and D-STAR GMSK are fine **at the RF level**: same deviation range,
  same or lower symbol rate.
* No register write in V1.05 suggests a digital I/Q or digital-baseband port to the MCU. The only RX
  audio that reaches the MCU is ADC1 channel 1 (see FEASIBILITY.md §1.3).
