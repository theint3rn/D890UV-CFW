# D890UV audio / modulation paths (from stock V1.05)

Found 2026-11-22 in the stock firmware (Ghidra decompile `../../decomp/src`, disassembly of
`D890UV_V1.05_20260521.CDD` where Ghidra dropped arguments). Same architecture as the D168UV
(`D168UV/cfw/docs/AUDIO_PATHS.md`). This corrects `FEASIBILITY.md` ("no DAC/SAI/I2S use, no MCU audio output"):
the MCU **does** have a TX audio output (TIM4_CH4 PWM on PB9) and uses both DAC1 channels (APC and RX tune).
The analog side (RC filter, coupling, pre-emphasis, where PB9 enters the modulator) still needs a scope.

## TX: MCU audio into the modulator -> PB9 = TIM4_CH4 PWM

| what | evidence |
|---|---|
| PB9 alternate function (AF2 = TIM4_CH4), push-pull, speed 3 | `FUN_0811a730`: `HAL_GPIO_Init(GPIOB, {0x200, mode 2, pull 0, speed 3, AF 2})` |
| PB9 back to analog (hi-Z) when the MCU does not play | `FUN_0811a6d4`: `{0x200, mode 3 = analog}`; called by `FUN_080d7a8c`, `FUN_08025320`, end of `FUN_080d7a00` |
| TIM4 PWM init, ch4, PWM mode 1 (OCMode 0x60) | `FUN_08062f04(ARR, PSC)` and `FUN_08114e84(ARR, PSC)` (handle `DAT_2001ec5c`, OC config `DAT_2001aea0`, channel 0xC = TIM_CHANNEL_4) |
| sample out = CCR4 | `FUN_08125854(v)`: `DAT_2001aea4 = v; HAL_TIM_PWM_ConfigChannel(TIM4, ch4)` |
| boot init | `FUN_08062f04(0xFF, 0)`: ARR 255, PSC 0 -> 468.75 kHz carrier, 8-bit duty (called from the timer set-up block `fw_08050000.c:5762`) |

Three ways the stock firmware drives it:

1. **Byte-table playback, 9600 samples/s (MDC-1200-style data, Ack/Call-alert/Status bursts).**
   `FUN_0813f0dc` / `FUN_0813f164` build the table: `FUN_0813f640(0x2580)` = encoder at **9600 Hz**,
   `FUN_0813f6a0(op, arg, unitID, extra...)` / `FUN_0813f768(op, arg, unitID)` = double / single packet
   (callers pass op 0x35 arg 0x89 etc. - the MDC-1200 encoder API shape), `FUN_0813f590(enc, DAT_200087bc, 0x1000)`
   = unsigned 8-bit samples, count in `DAT_2000c2fe`.
   Start `FUN_080d7c90`: state `DAT_2001afa8 = 4`, `FUN_0811a440(0x30D3)` = TIM5 ARR 12499 PSC 0 -> **9600 Hz**,
   PB9 AF, TIM4 counter on, TIM5 IRQ on (`FUN_0811a4c0(1)`).
   Per sample: `IRQ48_Handler` (TIM5, 0x08120E70) -> `FUN_080d7e54`: `CCR4 = table[i] >> 1` (0..127 of ARR 255);
   at the end TIM4 off, TIM5 IRQ off, state 0. TX side wrapper: `FUN_080d7a00` (waits while state 4, then PB9 analog).
2. **APRS / AX.25 AFSK 1200, DMA, 76.8 kHz.** `FUN_080195a0` (state machine case 0xE in `fw_08010000.c:6139`):
   `FUN_08114e84(0x619, 0)` = TIM4 ARR 1561 PSC 0 -> **76.82 kHz** PWM; `FUN_0805c6cc` -> `HAL_TIM_PWM_Start_DMA(TIM4,
   ch4, DAT_20004332, 0x100)` with GPDMA1 **channel 0** in linked-list circular mode (`FUN_0805d0b4`: instance
   0x40020050, `hdma` linked to TIM4 `hdma[CC4]`, IRQ 29). Ping-pong halves of 64 samples (`DAT_20004332` /
   `DAT_200043B2`) refilled from the half/complete callbacks `FUN_08056db8` / `FUN_08056d24`:
   `FUN_08122054` = next bit with **HDLC bit stuffing** (`DAT_20002bf3 < 5`), LSB first; `FUN_081221fc` -> `FUN_08122298`
   on a 0 bit = **NRZI**; `FUN_0810e6c8(bit)` writes 64 samples from a 360-entry sine (0x081502F0, values 0..1000 of
   ARR 1561) with per-sample phase tables 0x0814A03A (360 deg / 64 samples = **1200 Hz**) and 0x08149FB0
   (660 deg = **2200 Hz**), phase kept in `DAT_20013282` (continuous phase). 64 samples per bit = 1200 bd.
   End: `FUN_08122038` sets `DAT_20000c51`; `FUN_0804c0f8` stops TIM4 and re-inits it with `FUN_08062f04(0xFF, 0)`.
3. **Tone** (`DAT_2001d76e != 0`): `IRQ48_Handler` -> `FUN_0812587c`, 16 samples per cycle from the raised-cosine
   table 0x081505C0 (0..180), paced by TIM5.

While the MCU plays (both `FUN_080d7a8c` and `FUN_080195a0`) the speaker router `FUN_08118df8(2, ..)` switches every
RX audio source off (see "Audio routing"). No dedicated microphone-mute GPIO was found (the D168 uses PD1); the
mic path most likely goes through the SCT3288 codec / RF chip and is idle during these bursts. **[scope]**

Digital-mode (4FSK / C4FM / GMSK) TX through PB9 needs a bench test first: the stock firmware only sends AFSK and
tones through it, so the RC filter, coupling and pre-emphasis after PB9 are unknown (dummy load + service monitor).

## RX: discriminator / RX audio -> PC0 (ADC1_IN1)

| what | evidence |
|---|---|
| ADC1 regular scan, rank 1 = **channel 1** (PC0), then ch2 (battery), ch3, ch12, temperature (ch19) | `FUN_0805c4ac`: `HAL_ADC_ConfigChannel` with 0x04000002/rank 6, 0x08000004/0xC, 0x0C000008/0x12, 0x32001000/0x18, 0xCE080000/0x100; continuous, DMA -> `DAT_20001d34` (5 halfwords) |
| sample clock **TIM6 4800 Hz** | `FUN_08063048(0xF9, 99)` from `FUN_080d7c30`: ARR 249, PSC 99 -> 120 MHz / 25000 |
| per sample | `IRQ49_Handler` (TIM6, 0x08120EC0) -> `FUN_080d7d70` (only while not transmitting, `DAT_2001afc3 == 0`): `DAT_20001d34 >> 4` (8 bit) buffered 4 at a time into `FUN_0813f3d8` |
| decoder | `FUN_0813f3d8(DAT_2001f7e0, samples, 4)`, decoder made by `FUN_0813f2f0(0x12C0)` = **4800 Hz**: the MDC-style signalling decoder (counterpart of the 9600 Hz encoder above). `FEASIBILITY.md` called it an AFSK-1200 APRS demodulator; it is the in-band signalling decoder. |

So the cfw's RX input (ADC1 ch1, scan slot 0) was right. Flat / DC-coupled or de-emphasised: **unknown, scope it**
(needed for 4FSK/C4FM/GMSK demodulation).

## DAC1: power control (PA4) and RX tune (PA5)

`FUN_08061f5c` (DAC init): PA4 + PA5 analog (mask 0x30), DAC1 both channels, no trigger, ch1 starts
at 0 (`FUN_0801694c(0)`).

### PA4 = DAC1 ch1 = TX power control (APC)
* `FUN_0801694c(v)`: `HAL_DAC_SetValue(DAC1, ch1, 12R, v << 4)`.
* Baseband TX-state line **PD6** (EXTI6 rising/falling, `FUN_08061770`, IRQ 17): `IRQ17_Handler` (0x0803077C)
  rising -> **PE1 = 1, PA15 = 1, DAC1 ch1 = APC** (`DAT_2001afc0`); falling -> PA15 = 0, PE1 = 0, DAC1 ch1 = 0
  (levels checked in the disassembly at 0x08030826..0x08030858 / 0x080308FE..0x08030926). These are the TX enables
  the cfw called `RF_AUX0/1`.
* APC value `FUN_0811a75c(on)` (TX frequency `DAT_2000432c` in 10 Hz, power level `DAT_2000430d` 0..3, 0 = low):
  `APC = table[level][MHz index] + cal[level] - table[level][reference]` (u16 arithmetic), written immediately if
  transmitting. Tables and cal from the RF calibration block (below):
  * UHF (>= 300 MHz): index = (f - 400 MHz) / 1 MHz (0..120), reference index 40 (440 MHz),
    tables NAND +0xC00/+0xD00/+0xE00/+0xF00 (levels 0..3, 0x79 bytes), cal = block0 +0x02..+0x05.
  * VHF: index = (f - 136 MHz) / 1 MHz below 174 MHz, else (f - 220 MHz) / 1 MHz + 39; reference index 9 (145 MHz);
    tables +0x1000/+0x1100/+0x1200/+0x1300 (0x2D bytes), cal = block1 (+0x100) +0x02..+0x05.
  * 200-300 MHz forces level 0; `DAT_20014a27` (a power-limit flag) lowers level 3 to 2.
  * Factory test modes (`DAT_2001ae3b`) use the cal byte alone - not ported.

### PA5 = DAC1 ch2 = UHF RX front-end tune voltage
* `FUN_08010bf0(v)`: `HAL_DAC_SetValue(DAC1, ch2, 12R, v << 4)`.
* `FUN_0811c8f0(f)` (f >= 300 MHz): index = (f - 400 MHz) / 10 MHz; < 12: `t[i] + (u16)((t[i+1]-t[i]) * MHz digit) / 10`,
  else t[12]. Table `DAT_2000d928[13]` built by `FUN_08049fc4` from three u16 cal points (block0 +0x18 / +0x1A / +0x1C
  = 400 / 460 / 520 MHz): points 1..5 = previous + (u8)((p1 - p0) / 6), 7..11 = previous + (u8)((p2 - p1) / 6)
  (the step is truncated to a byte, as stock).
* Called on every frequency change by `FUN_08117034` (RX on the A/B frequency `DAT_2000c240/44`; during TX on the TX
  frequency).
* **VHF tune is not a DAC**: `FUN_0811cadc` (f < 300 MHz) -> `FUN_0801a35c(v)` = RF chip 0 **register 0x5C =
  (v & 0xFF) << 4 | 0x1000**. Table `DAT_2000d942[45]` from `FUN_0804a0a0`: cal points block1 +0x1E / +0x20 / +0x22
  (136 / 174 / 225 MHz), 1 MHz steps 136..174 (step (p1-p0)/38), and 220..225 MHz counted back from p2 by
  (p2-p1)/6; lookup by the 100 kHz digit.

### RF calibration block
`FUN_080e0440`: NAND **0x04F00000** (A) or 0x04F40000 (B, used when A is not valid: `FUN_08023ea8` = valid marker
`55 55 AA AA` and no invalid marker). block0 = +0x000 (0x2A bytes -> `DAT_200152ac`), block1 = +0x100 (0x2C bytes ->
`DAT_200152d6`), APC tables +0xC00..+0x1300. The emulator NAND image has this block erased (all 0xFF): stock then
computes APC = 0xFF (DAC 0x0FF0, the level the cfw-emu trace saw) and tune 0xFFFF.

## Other PWM: PB0 = TIM3_CH3 = LCD backlight
`FUN_08062e40(0xFFF, 3)` (ARR 4095, PSC 3), PB0 AF2 (`FUN_0811128c`), duty `FUN_0801a3b4(v)` with levels from
0x0814A90A (2550 / 2750 / 2850 / 2950 / 4095) and 0 = off (`FUN_08114730`).

## Audio routing (speaker)
`FUN_080c32e8(source, chip)` -> `FUN_08118df8(mask, chip)`: bit0 = RF reg 0x52 switch 1 (`FUN_080e3c00` ->
`FUN_08019ab4(1, v, chip)`: reg 0x52 = idx << 12 | 0x40 | v, written on change), bit2 = switch 2 (`FUN_080e3580`),
bit3 = PC7. Baseband codec audio: codec register 0x0F via `FUN_0811687c` (1 = off, 0 = on). PC12 = speaker amplifier
(`FUN_08118dbc` on, `FUN_08118d9c` off).
* source 0 (MCU tone, idle): codec off, mask 2 (switches 1 + 2 off, PC7 low), amplifier on.
* source 1 (analog receive, main chip): mask 4 (switch 2 on) - or 1 / 0 depending on channel state; codec off.
* digital receive: mask 0x10 (RF switches off), codec on.
* volume knob at step 0 (`DAT_2001d774 == 0`): `FUN_080252e0` = amplifier off + codec off, nothing is heard - the stock
  tone player `FUN_0801a6d0(id)` -> `FUN_080c32e8(0)` plays no tone then.
* Key/alert tones: `FUN_0801a6d0` -> `FUN_08025320` (stop the previous one: TIM5 off, CCR4 0, PB9 analog, TIM4 off),
  `FUN_0811cd44(id)`, `FUN_0811a730` (PB9 AF2 = TIM4_CH4), `FUN_08133210` (tone table, TIM5 sample clock on via
  `FUN_0811a4c0(1)`).

## Volume (knob + Vol Level)
* The knob is a plain potentiometer read by ADC1 ch3 (stock scan buffer `0x20001D38` = cfw ADC frame slot 2), four
  readings averaged, step = `FUN_0804bc50`: raw < 0x79 -> 0, raw < 0xF60 -> (raw - 0x78) / 0x112 + 1, else 15.
* Loudness = an I2C potentiometer on the bit-banged bus (SDA PB6, SCL PB7: `FUN_080585b4` start, `FUN_080583ac` stop,
  `FUN_080584d4` byte): `FUN_0806536c` writes device 0x5C (7-bit 0x2E) register 0x00 = value; value =
  table `0x0815153E`[Vol Level 0..8 (settings +0x3B, 0 = Indoor)][step 0..15] (`FUN_081137c0`, 9 x 16, 0x00..0x7F),
  resent on change (`FUN_0801ff48`). So the Vol Level caps all speaker audio, FM receive included.
* Other devices on that bus: 0x19 (0x32/0x33) LIS2DW12 accelerometer (man down), 0x35 (0x6A/0x6B) not identified.
* Note: stock USART1 (programming cable) is PA9 / PA10 (`FUN_08063358`: GPIOA pins 9/10, AF7); PB6 / PB7 are this bus.

## VOX
* The microphone is not read by the MCU. The accessory module on UART4 (PC10/PC11 AF8, 115200, `FUN_080635c8`; JieLi
  BT / noise-reduction board, frames `A5 5A cmd len data 06`) detects voice:
  * VOX on/off: cmd 0x12, 14 bytes from `0x2001446E` (`FUN_0806a60c` / `FUN_0806ae54` -> `FUN_08111908(0x12)`),
    byte 0 = 1 (0 only at power-off, `FUN_08034570`), byte 12 (`0x2001447A`) = VOX (settings +0x0C, clamped to 0/1 by
    the range check in `FUN_080d0000`); stock boot frame `A5 5A 12 0E 01 00 .. 00 06`.
  * voice: module status cmd 3, 8 bytes -> `0x20015516` (`FUN_080c93a0` case 3); byte 3 (`0x20015519`) != 0 = voice.
  * `FUN_08133618`: 3 polls with voice -> `DAT_2001d777` = VOX TX, event 0x18; released after VOX delay (settings
    +0x0D, `DAT_200148f1`) + 5 ticks without voice (`DAT_2001af6e`), event 0x19.
* RF chip VOX registers are fixed at init on this radio (`FUN_0802520c`: reg 0x12 = 0x20D8, reg 0x13 = 0x00EE).

## GPS receiver
USART2 PA2/PA3 AF7 pull-down, 9600 (`FUN_08063498(0x2580)`). Stock sends `$CFGSYS,h00000001` + `$CFGMSG,0,n,x` for
n = 0,1,2,5,3,4,6,7,8,9 (x = 1 for 0 GGA, 4 RMC, 5 VTG). No GPS power switch: toggling Menu > GPS > GPS On/Off in the
stock emulator leaves every GPIO (A..I MODER / ODR) unchanged.

## Used by the cfw (2026-11-22)
* TX audio: `drivers/dac.c` = TIM4_CH4 PWM on PB9 at 48 kHz (ARR 2499), CCR4 reloaded every period by GPDMA1
  channel 1 (request 71 = tim4_upd, self-linked ring); PB9 analog (hi-Z) when idle, as stock `FUN_0811a6d4`.
  Analog FM test tone allowed on every build (`CFW_MCU_FM_TX`); digital modes need `CFW_DIGITAL_TX`
  (emulator builds, or `make HW=1 HW_DTX=1` - dummy load only).
* RX demod: unchanged, ADC1 ch1 (PC0) at 48 kHz (TIM15 trigger) - stock samples the same input at 4.8 kHz.
* APC: `dac_apc(tx)` = PE1 / PA15 + DAC1 ch1 = stock `FUN_0811a75c` value for the TX frequency (level 0 = low unless
  `CFW_APC_LEVEL` is set), from the NAND calibration block; without a valid block 0xFF (stock with an erased block).
* Tune: `dac_rx_tune(hz)` on every main-chip frequency change: UHF -> PA5 (stock table), VHF -> RF reg 0x5C;
  without calibration PA5 = mid-scale and reg 0x5C untouched.
* Emulator: `emu/models/Pwm.cs` ("pwm") replaces `timer4` and captures the PB9 duty as 12-bit samples;
  `emu/txrx_loop.py` captures from it.
* Speaker (2026-11-23, `drivers/audio.c`): FM receive = switch 2 on + amplifier, digital = switches off + amplifier,
  TX / idle = amplifier off; beeps = source 0 and nothing with the knob at 0; volume pot = stock table from the
  Vol Level and the knob (`drivers/i2c.c` bus). Codec register 0x0F and the BT-headset case are not driven.
* VOX (`drivers/btmod.c`): UART4 hello + cmd 0x12 (byte 12 = VOX on), module status cmd 3 byte 3 = voice.
* GPS: the stock `$CFGSYS` / `$CFGMSG` lines when GPS is on (stock setting `Gps`).

## Checked in the emulator (2026-11-22)
* Synthetic RF calibration block in a NAND copy (block A valid; UHF tune points 0x40/0x70/0xC0, VHF 0x30/0x58/0x90,
  APC cal 0x80 / 0x60, table byte = 0x30 + MHz index (UHF) / 0x20 + 2 * index (VHF)). Stock V1.05 RAM tables
  `DAT_2000d928[13]` / `DAT_2000d942[45]` and the cfw's `s_tu` / `s_tv` are **identical** (58 values).
* Stock PTT at 443.850 MHz: `DAT_2001afc0` = 0x83, DAC1 ch1 = 0x830 (= table[43] 0x5B + cal 0x80 - ref 0x58); stock RX
  449.840 MHz: PA5 = 0x670. The cfw (same formulas): RX 446.580 MHz -> PA5 0x640, TX 441.580 MHz -> APC 0x810, both as
  computed by hand from the stock code.
* Stock boots with `pwm` in place of `timer4` (OSRunning = 1, main screen).
* Two-radio matrix with TX on PB9: see `emu/README.md` "Simplex matrix".

## Signalling: DTMF / 2-tone / 5-tone / MDC1200 = QDC1200 (2026-11-23)
* **QDC1200 is MDC1200.** Both stock firmwares carry the same MDC1200 coder (the libmdc1200 structure: encoder at a
  given rate, single / double packet, 4-phase zero-crossing decoder), and their menus call it "QDC1200" (strings
  "QDC1200", "QDC1200 Set", "QDC Enc"); the D890UV CPS names its tables "MDC1200 Address Book / Encode", the D168UV CPS
  "QDC Address Book / QDC1200 Setting / QDC1200 Decode". Evidence:
  * D890UV V1.05: decoder init `FUN_0813f2f0(4800)`, per-sample `FUN_0813f3d8` (TIM6 IRQ49, ADC ch1 >> 4), bit / sync
    step `FUN_0813c16c`: `popcount(sync_hi ^ 0x07) + popcount(sync_lo ^ 0x092A446F) < 6` (or `> 34` = inverted),
    then 112 bits -> `FUN_0813bf48` (CRC). Encoder `FUN_0813f640(9600)`, double packet `FUN_0813f6a0(op, arg, unit,
    b0, b1, b2, b3)` (second 4-byte block after the first), single `FUN_0813f768`, samples `FUN_0813f590`.
  * D168UV V1.07: the same decoder `FUN_08011000` (4 phases), sync step `FUN_08013000` with
    `popcount(.. ^ 7)` and the constant at 0x08013130 = **0x092A446F**; encoder `FUN_080a108c(0x2580)` (9600 Hz),
    double `FUN_080a10d4` / single `FUN_080a1140`, wrappers `FUN_080a0f8c` / `FUN_080a0fe0`.
  * Message kinds the stock D890UV sends (`FUN_0810c51c`, kind `DAT_20014e44`; own id `DAT_20014e4a`):
    0 call alert = double `35 89 <target>` + `80 15 <own>`; 1 radio check `63 85 <target>`; 2 selective call =
    double + `81 0D` (`83 0D`); 3 / 4 `2B 00` / `2B 0C`; 5 emergency `00 80`; 6 remote monitor `31/71 81`. Acks
    (`FUN_08125058` cases): `03 00`, `0B 00`, `0B 0C`, `20 00`, double `35 89 .. A0 00 ..`.
  * Packet format = `emu/streams/gen_mdc1200.py` (preamble 0x55 x7, sync 07 09 2A 44 6F, CRC-16, rate 1/2 code,
    16 x 7 interleave, 1200 bd differential 1200 / 1800 Hz); the stock V1.05 decoder accepts it
    (`emu/streams/test_stock_mdc.py`).
* Channel decode type: stock D890UV channel byte +0x1A, value 4 = MDC1200 (`FUN_0811e99c`) = the CPS "Optional
  Signal" column (Off, DTMF, 2Tone, 5Tone, MDC1200).
* Stock TX path: MDC1200 through the PB9 PWM (above). For DTMF / 2-tone / 5-tone not shown: `rf_chip.md` points to a tone
  generator in the RF chip. The cfw uses the PWM for all of them.

### Used by the cfw (`app/sig.c`, `app/sig_dec.c`, `app/sig_mdc.c`, both radios)
* TX: the MCU makes the tones / FSK at 48 kHz as FM deviation into the PB9 PWM ring (`dspb_fm_set_source`):
  DTMF (697..1633 Hz, 1500 Hz deviation per tone, times from the DTMF Settings dialog), 2-tone
  (2Tone Settings), 5-tone (14 standards ZVEI1 .. CCITT, tone time from the entry,
  repeated digit -> repeat tone E), MDC1200 (stock op/arg as listed above; call alert as a double packet with the own
  id). PTT ID at the start / end of each FM transmission per channel (below); manual sends from Menu > Signalling; keypad
  digits while PTT is held send DTMF.
* RX: decoders on the 8 kHz FM audio (`aprs_rx_sample` tap, `sig_rx_sample`): DTMF Goertzel, single-tone frequency
  from zero crossings (280 Hz high pass removes CTCSS) for 2-tone / 5-tone, MDC1200 by a 1500 Hz complex-baseband FM
  discriminator with integrate-and-dump at 8 bit phases (zero-crossing counting as the stock decoder does failed at
  ~20 dB SNR in the host test; this one decodes at ~8 dB).
* Alert tone (stock Alert Tone table, Freq/Time in 10 ms, 5 alerts x 5 steps) on the speaker through the PWM
  (`dspb_fm_local`: receive decoding continues meanwhile): alert 1 = individual call, 2 = group call, 3 = selective
  call (DTMF / 2-tone / 5-tone to this radio).
* Checked: `tools/sig_test.py` (emulator, both radios): TX captures decoded by an independent Python decoder, RX of
  generated streams and of the other run's captures, and (D890UV, `--stock-mdc`) the **stock V1.05 decoder decodes the
  cfw's MDC1200 call alert** (op 0x35, arg 0x89, unit 0xBEEF).
* Setting dialogs (2026-11-24; codeplug section 10 tables 65-68, read with `stockdlg_*`, cached in `sig_cfg()`; absent = stock
  default). Meanings come from the CPS labels and the user manual (p. 34-36, programming guide 7.3), not from tracing the
  stock firmware; those marked (a) are assumptions:
  * DTMF: Transmitting Time (tone and pause), First Digit Time, Pretime and Time-Lapse (silence before / after the code), D
    Code Pause (a `D` in a code = pause of n s, 0 = tone D) (a), Self ID + Interval Character (`123*456` = call to 123 from
    456) (a), Group Code = wildcard for a Self ID digit (a), Decoding Response (None = no alert; both others = alert 3).
  * 2Tone: 1st / 2nd tone duration, Gap; an entry without a 2nd tone is sent as one tone of Long Tone Duration (a). The
    receiver accepts tones of >= 60 % of the durations (at most 300 ms).
  * 5Tone: Self ID (empty = the channel's 5Tone ID entry), Decode Standard (index in the CPS order of the standards (a)),
    Decode Time = length a tone needs (a; 20 ms minimum), Disp AnyID off = other IDs go into the heard list without a
    popup (a), Decoding Response, Pretime, Time-Lapse, PTT ID Pause Time = no new BOT within that time (a).
  * PTT ID: DTMF and 5Tone send the BOT / EOT codes of their dialogs (manual; before 2026-11-24 the cfw sent the encode list
    entry, which was wrong), 2Tone none. 5Tone BOT / EOT (R5tone columns) send the channel's 5Tone encode ID (manual).
    MDC1200 / QDC1200 PTT ID is the cfw's own behaviour.
  * 5Tone information codes: sequence = Information ID, alone or after the Self ID (a) -> call with its number and Decoding
    Response. The function option is never acted on.
  * Stored, not used: Auto Reset (no squelch by signalling here), Side Tone (would need the speaker path on during TX), DTMF
    PTT ID on/off and Pause (unit unknown), kill / stun, 5Tone Dec Units, First Delay and First Delay After Stop (meaning
    unclear), Time Of Decode Tone (D168UV), function option of the information codes.
* Not done: auto-acknowledge (ACK / Need To Answer, "& Respond" = beep only), remote kill / stun / monitor (stored only; a
  radio that disables itself over the air is not wanted in a test firmware).
* Assumptions of the lists: option texts matched by keyword (no stock export with real values seen); Rpga_Mdc, 2TONE Decode;
  alert 1 / 2 / 3 = individual / group / selective; D890UV own MDC id = the PTT ID entry; 5-tone frequency tables from memory.
