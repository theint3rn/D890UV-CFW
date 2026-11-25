# RF registers and GPIO: stock firmware against the cfw (2026-11-24)

Method: `tools/rf_compare.py` logs every RF register write (hook on the stock write routine, D890UV V1.05 `FUN_080e2f64`, D168UV
V1.07 `FUN_0807bc60`, and on the cfw's `rf_write`) and reads the GPIO mode / output registers in each state, in the emulators.
States: receive, receive with a signal, squelch closed, transmit (FM), receive after transmit, on 146.52 / 446.0 MHz (D890UV 12.5 and 25 kHz),
digital receive on 145.51 / 441.0 MHz, FM broadcast, AM air band (D890UV). The final register contents per chip are compared
(registers 0x40, 0x49, 0x52, 0x53, 0x2E per sub-register in the top nibble). Frequency registers 0x68 / 0x69 are left out.
Limits: the emulator's RF chip is a register file, so this shows what the stock firmware WRITES, not what the chip does. Nothing here was
measured on a radio. Squelch change is covered by `tools/sql_test.py` (register 0x49 against the stock formula, all levels);
digital transmit was not traced (the stock one needs the real baseband). D168UV: 12.5 kHz FM and the squelch-open audio state not traced
(the stock VFO is 25 kHz; the menu was open), FM broadcast compared on 435 MHz digital only.

"fixed" = the cfw was wrong before 2026-11-24 and now equals the stock trace. "same" = equal before and after.

## D890UV, RF chip 0 (CS PB5)

| What | Stock | cfw | State |
|---|---|---|---|
| 0x56 receiver on | 1 (chip 1 AM: 6) | was 0 in every receive state | fixed |
| 0x58, 0x5F, 0x60 | F1F0, 0468 / 0569 (split 390 MHz), 0309 | not written after power on | fixed |
| 0x52 switches 0, 6, 7, 4 (receive) | VHF 0 1 1 0, UHF 1 0 0 1 | not set before the first TX, then UHF values on both bands | fixed |
| 0x52 switches 0, 6, 5, 3 (transmit) | 0, 0, 1 only 200-300 MHz, 3 = UHF | 3 set on both bands | fixed |
| 0x53 band table (`FUN_081155ac`), 0x42, 0x49[7] | 53[4] 467F / 463D, 53[2] 263A / 2637 | VHF table only, 0x42 / 0x49[7] from power on | fixed |
| FM filter set 0x1C-0x27, 0x32 | analog set on 12.5 and 25 kHz | 12.5 kHz FM got the digital set | fixed |
| 0x48 bandwidth | FM 082E, 25 kHz 9826, digital 016E | 082E always | fixed |
| 0x2A, 0x10, 0x11, 0x18, 0x16, 0x3F | FM 1081 A3A3 041A 0068, digital 108C A3E0 0000 0068, 0000, 3232 | 1081, not written, 041A, 006E, not written | fixed |
| 0x30 | FM 5F77 (5F74 with the calibration byte `DAT_200151c4` = 'f'), digital 5F75 | 5F77 always | fixed for digital; FM: unknown (byte not read) |
| 0x5C | VHF tune, UHF 1000 | UHF left at 0001 | fixed |
| 0x5E, 0x60 in transmit | 0468 0306 / 0569 0309 | 0768, 0300 | fixed |
| 0x31 (CTCSS) | not written without a tone | 0000 | intended |
| transmit: 0x11, 0x18, 0x27, 0x2B, 0x2D, 0x2E, 0x2F, 0x30, 0x31 | 0000, 0068, A300, 708D, 3205, C048, 0A0F, 5F74, tone | 041A, 006E, 199C, F08D, -, -, -, 5F77, tone | **bench** |
| AM air: chip 0 | receiver off (0x56 = 0) | stays on (FM Monitor) | intended |
| 0x49[0] squelch | calibrated | calibrated, same formula | same |

## D890UV, RF chip 1 (CS PD3)

| What | Stock | cfw | State |
|---|---|---|---|
| 0x52 switches 1 / 2 (speaker audio) | FM signal: 2 on; digital call: 1 on; broadcast: both off | written to chip 0; digital: both off; broadcast: 2 on | fixed |
| switch 1 on an idle digital channel | off until a call | on while the channel is digital | intended (baseband audio open) |
| switch 2 with squelch Off | follows the squelch | on | intended |
| 0x52 switch 0 | 1 in receive (0 with AM) | 0 | unknown: probably the MCU audio input (PC0); the cfw demodulates there, so 0 is kept; **bench** |
| FM broadcast: switches 5 / 4 | 1 / 0 | not set | fixed |
| AM: switches 5, 7, 4; 0x56 = 6 | 1 1 1; 6 | same | same |
| AM: 0x5C | ends at 1002 | 0002 | fixed |
| idle: 0x56, 0x30 | 0, 5F6B | 5F00 in 0x30 | fixed |
| 0x5C idle | 1002 (tuned to the B channel) | 0001 | intended (no second receiver in the cfw) |

## D890UV, GPIO

| Pin | Stock | cfw | State |
|---|---|---|---|
| PC6, PD8, PD9, PD10 (baseband power / reset, `FUN_080d6f5c`) | 1 1 0 0, PD8 pulsed at power on | PC6 not driven, PD8 low, **PD9 high** | fixed |
| PC12 amplifier, PC7 | on with audio, off in transmit; PC7 0 | same | same |
| PA1, PE0, PD2, PD1, PD0 (modulation source `FUN_081140ec`) receive | 0 1 1 0 0 | not driven | fixed |
| the same in transmit (microphone) | PD2 0 | PD2 1 (= stock position 5, MCU tone) | **bench** |
| PC13 (`FUN_0810e010`) | 1, 0 with AM | not driven | fixed (meaning unknown) |
| PE1, PA15 transmit enables | 1 in transmit | same | same |
| PE3, PE4, PE5 LEDs | PE4 with a signal, PE5 in transmit | not driven | intended (no LED support) |
| PC3 power latch | 0 in the emulator (the bootloader sets it) | 1 | emulator only |
| PA11 / 12 USB, PD4-6, LCD / keypad bus PE7-15, I2C PB6 / 7 | | | not compared (other functions / moving) |

## D168UV, RF chip

| What | Stock | cfw | State |
|---|---|---|---|
| 0x56, 0x58, 0x5F, 0x60, 0x53, 0x42, 0x43, filter set | band and state values | same | same |
| 0x2A, 0x10, 0x11, 0x18 | FM 1084 A3A3 041A 006E, digital 108C A3E0 0000 006E | digital values on FM too | fixed |
| 0x30 | FM 5F65, digital 5F75 | by 12.5 / 25 kHz instead | fixed |
| 0x48 | FM 082E, 25 kHz 9826, digital 09FE | FM 082E on both widths | fixed |
| 0x52 switches 7 / 4 (receive path) | VHF 0 1, UHF 1 0; both off in transmit | stayed on in transmit | fixed |
| 0x52 switch 0 | 1 in transmit | 0 | fixed |
| 0x52 switch 5 after UHF transmit | stays 1 | 0 | intended |
| 0x52 switch 3 | 0 in transmit, 1 in receive | 1 (VOX switch) | **bench** |
| retune across the bands without a new set-up | always a full set-up | band registers not changed | fixed (both radios) |
| transmit: 0x11, 0x18, 0x27, 0x2A, 0x2B, 0x2D, 0x2E, 0x30, 0x31, 0x5B | 0000 0068 A300 1081 708D 3205 353B / C2B2 5F00 tone 0085 | 041A 006E 6080 1084 F08D 2215 - 5F65 tone B085 | **bench** |
| 0x52 switches 1, 2, 6 | 0 from power on | 0 | same |
| 0x49[0] squelch threshold | 00A7 / 0097 | 00C7 / 009F | intended: other NAND calibration image and squelch level in the two runs; the formula is checked by `tools/sql_test.py` |
| 0x31 | not written without a tone | 0000 | intended |

## D168UV, GPIO

| Pin | Stock | cfw | State |
|---|---|---|---|
| PE0, PC12 audio | muted in the traced states, as the squelch was closed | by mode | same rule (`FUN_08070a70`), open state not traced |
| PA7 amplifier | 0 with closed squelch | 1 in receive | **bench** (polarity unverified, `hal/board.h`) |
| PD1 microphone path | 1 in (microphone) transmit | 0 (MCU audio) | intended |
| PD7, PD8, PD9, PD10 baseband | 1 1 0 0 | same code; PD7 reads 0 in the emulator (set before its output mode) | emulator only |
| PE6, PC4 transmit enables | 1 in transmit | same | same |
| PA1 (output low), PC13 (input, pull-up) | | not driven / pull-down | unknown |
| LED PE3 in transmit | 1 | not driven | intended |

## Guards
* `emu/models/RfTransceiver.cs` (both radios): cfw runs get `rf0 StrictRx true`; a signal is seen only when the registers match a
  stock receive state, `rf0 Status` names the wrong one ("rx FAULT reg ..."). Transmit = reg 0x56 = 2.
* `tools/rfswitch_test.py` (D890UV): switch outputs of both chips in receive, transmit, broadcast and AM.
* `tools/rf_compare.py trace / table`: repeat this comparison (stock part needs your own AnyTone files).
