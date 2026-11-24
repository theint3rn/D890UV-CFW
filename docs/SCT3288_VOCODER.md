# SCT3288 vocoder interface (for the cfw `SCT3288` backend)

Status 2026-11-20. Sources: stock V1.05 lifted code + Ghidra, the SCT_Port tool command files
(`D890UV V1.05 DMR official release 260521/base band 3288 upgrade/SCT_Port 3288/SCT_File/SCT_MAIN/CommandArray_*.xml`,
decoded with base64), and the emulator. The baseband firmware was **not** decrypted.
**Confidence:** what V1.05 does = **known**. The packet format = **known**, because the SCT_Port files hold the
exact bytes. How the chip answers vocoder packets = **unverified**; it must be measured (see §6).

## 1. Where the audio goes (stock V1.05)

```
mic ─► TI TLV320AIC3204 codec ─(I2S/PCM)─► SCT3288 (AMBE+2 vocoder + DMR modem) ─► RF chip
spk ◄─ TI TLV320AIC3204 codec ◄─(I2S/PCM)─ SCT3288                                ◄─ RF chip
                         ▲ I2C, bridged by the SCT3288 (packet type 0x40 "TI_*")
STM32U575 ──SPI2 (control only)──► SCT3288
```

* The SCT3288 handles voice from end to end. The stock MCU **never sends or receives PCM or vocoder frames** on SPI2.
  * `FUN_0802dff8` sends MCU commands. Their packet type comes from `FUN_080c43d4(t)`, which masks it to
    `t & 0x7f`. The only types V1.05 uses are 0x00, 0x05 (`DAT_2000ecaa`), 0x23 and 0x40.
  * No MCU wait (`FUN_080f83a4` / `FUN_0810c438` / `FUN_080dee40`) waits for a voice-sized message.
  * The receive dispatcher `FUN_08146930` handles only status IDs:
    00 01 15 17 1B 1E 26 27 43 4B 50 51 76 77 7B 7C 7D 7E 7F.
* The audio codec is a TI AIC3204. SCT_Port ships `AI3204I2C.dll`, and its commands with type 0x40 write codec
  registers: `40 00 01 <reg> <val> 00 00`, `40 1D 10` ADC→DAC loop, `40 47 80` tone start. Ten stock call sites use
  `FUN_080c43d4(0x40)`, so the MCU sets codec gain and routing through the SCT.
* Recording ("Record Switch" menu) and voice prompts exist in the menus. The traced code shows no path that moves
  audio data to the MCU for them. Either the SCT stores recordings itself (it has `RD/WR_FLASH_DATA` type 0x23/0x03
  commands), or they use a path not found yet. **Open**, and not needed for the cfw.
* USB passthrough: `FUN_0801f800` runs at boot. If `#` (key 12) is held, PA8 (PF3) is held low, and the check is
  repeated, it sets `DAT_2000f075 = 1`. While that flag is set, `FUN_080f335c` copies every SCT frame to USB
  (`FUN_0810cc1c`), so SCT_Port can talk to the chip **through the radio**. The setting `DAT_2000d74a` also sets the flag.
  Unverified: that USB→SCT works in this mode as well.

## 2. Packet format (SPI2)

Each packet is an AMBE-3000-style packet with the SPI sync bytes `84 A9` in front:

```
84 A9 | 61 | LenH LenL | TYPE | fields... | [00 pad if Len odd]
Len = number of bytes after TYPE (TYPE not counted). Fields are big endian.
```

The stock emulator notes call TYPE "B0". Types:

| TYPE | Use |
|---|---|
| 0x00 / 0x05 / 0x23 | DMR control stack (stock). 0x05 = per-channel/slot commands, 0x23 = info reads (0x32/0x33) |
| 0x03 | SCT flash / config writes (FLASH_*, `03 2D 04` CODEC_SELECT) |
| 0x40 | TI AIC3204 codec register access through the SCT |
| **0x80** | **vocoder control** (AMBE-3000 control packet) |
| **0x81** | **channel packet** = AMBE bits (AMBE-3000 type 0x01) |
| **0x82** | **speech packet** = PCM (AMBE-3000 type 0x02) |

Bytes taken from SCT_Port `CommandArray_p_ambe3000.xml` (the SPI tool adds `84 A9` in front of each):

| Name | Bytes (from 0x61) | Meaning (AMBE-3000 field) |
|---|---|---|
| AMBE_PKT_PRODID | `61 00 01 80 30` | product ID string |
| AMBE_PKT_VERSTRING | `61 00 01 80 31` | version string |
| AMBE_PKT_RESET | `61 00 01 80 33` | reset vocoder |
| AMBE_PKT_RESETSOFTCFG | `61 00 07 80 34 20 00 00 20 00 00` | reset + soft config (cfg0..2, mask) |
| AMBE_PKT_HALT | `61 00 01 80 35` | halt |
| AMBE_PKT_LOWP | `61 00 01 80 10` | low power |
| AMBE_PKT_ECMODE | `61 00 03 80 05 00 40` | echo-canceller mode word |
| AMBE_PKT_GETCFG | `61 00 01 80 36` | read config |
| AMBE_SPEECHD | `61 01 42 82 00 A0` + 320 B | SPCHD: 160 samples s16 BE, 8 kHz, 20 ms |
| AMBE_CHAND | `61 00 0B 81 01 48` + 9 B | CHAND: 0x48 = 72 bits (AMBE+2 rate 33 with FEC) |

Other vocoder and debug commands in SCT_Port. These use the DMR stack (type 00, ID 0x18 = loop/test mode):

| Name | Bytes | Note |
|---|---|---|
| VOCODER_INI | `61 00 02 00 12 00` | |
| VOCODER_SEL | `61 00 02 00 10 04` | reply `84 A9 61 00 0E 30 77 0D …` |
| READ_VOC_SEL | `61 00 01 20 10` | |
| VOCODER_CMODE | `61 00 02 00 02 08` | reply `… 20 3A 01` |
| VOCODER_TONE | `61 00 04 00 08 01 80 F3` | |
| VOC_ACTIVATION | `61 00 45 00 96 …` (69 B) | vocoder licence/activation; reply `00 17 06` |
| ALOOP_VOCODER_LOOP | `61 00 04 00 18 03 13 00` | analog-side loops: `03 02` decimator, `03 05` packet, `03 06` 4FSK, `03 11` codec |
| MLOOP_VOCODER_LOOP | `61 00 04 00 18 03 50 00` | MCU-side loops: `10` codec, `20` 4FSK, `30` packet, `60` decimator |
| AMLOOP_VOCODER_4FSK_LOOP | `61 00 04 00 18 03 23 00` | |
| DEBUG_SPEECH_HPI | `61 00 04 00 18 00 00 07` | probably moves speech to the host port (HPI) |
| Set_HPIMODE / Reset_SCT325x_HPI_MODE | `61 00 02 05 A3 00` / `05 A0 00` / `05 A0 01` | host-port mode |

Rate selection uses the AMBE-3000 fields, **unverified on the SCT3288**:
* `80 09 21` RATET 33 = AMBE+2 3600/2450 with FEC, 72 bits per 20 ms (DMR, NXDN, YSF VW/DN-half, dPMR).
* `80 09 22` RATET 34 = 2450 without FEC, 49 bits (P25 Phase 2 uses the 2450 core plus its own FEC).
* D-STAR AMBE 3600x2400: RATEP `80 0A 01 30 07 63 40 00 00 00 00 00 00 48`.
* Full-rate IMBE (P25 Phase 1) is **not** an AMBE-3000 rate. Do IMBE on the MCU (mbelib-style) unless measurement
  shows otherwise.

## 3. Expected replies (AMBE-3000 convention)

* Control: the reply has the same TYPE (0x80). Each field ID is echoed, followed by a status byte (00 = ok). PRODID
  and VERSTRING reply with the field ID and a NUL-terminated string. GETCFG replies with config bytes.
* Encode: the MCU sends 0x82 SPCHD, and 20 ms later the chip sends 0x81 `01 <bits> <data>`.
* Decode: the MCU sends 0x81 CHAND, and the chip sends 0x82 `00 A0 <320 B>`.
* Unknown: whether the SCT3288 needs a mode command first (`Set_HPIMODE`, `DEBUG_SPEECH_HPI`, or the
  `00 18 03 xx` loop modes) before 0x81/0x82 packets reach the host port instead of the I2S codec.
* Unknown: whether `00 18 03 50 00` (MLOOP_VOCODER) is the "PCM from MCU" mode.

## 4. Emulator model (`emu/models/Baseband.cs`)

This is a fake vocoder, deterministic and loopable. It is enough to develop the cfw backend and its framing.

* `80 30` → `80 30 "SCT3288-EMU" 00` and `80 31` → `80 31 "EMU-FAKE-VOCODER-1" 00`. Set them with
  `spi2 VocoderProdId "…"` / `spi2 VocoderVersion "…"`.
* `80 09 21|22` sets 72 or 49 bits and replies `80 09 00`. `80 0A` + 12 B selects 72 bits. Fields 33 34 35 36 05 0B 10
  are acked with `<id> 00`; GETCFG returns 6 zero bytes. Unknown fields are acked and the rest is skipped.
* Encode `82 00 n <s16be×n>` → `81 01 <bits> 'F' <freqH> <freqL> <peak/128> 00…` (9 or 7 bytes). The frequency is a
  big-endian 16-bit value in 0.25 Hz steps, taken from interpolated zero-crossing times. A 440 Hz tone encodes as
  `06 E0` (= 440.00 Hz), steady from frame to frame.
* Decode `81 01 <bits> 'F' fH fL a …` → `82 00 A0` + 160 samples of a sine at f/4 Hz with amplitude a×128. The phase
  carries across frames. Any other channel data decodes to silence.
* The reply is queued at once (the real chip takes about 20 ms). The frame log is `D890_BB_LOG`, tagged `(vocoder)`.
* `spi2 FromMcu "84 A9 61 00 01 80 30"` injects MCU bytes, so you can test without MCU code.
* Stock V1.05 never sends type ≥ 0x80. compare.py still shows identical screens.

## 5. cfw backend API (proposed)

```c
int  sct_voc_init(uint8_t rate);                 // 80 33 reset; 80 09 rate (33 or 34); returns 0 on "<id> 00"
int  sct_voc_prodid(char *buf, int n);           // 80 30
int  sct_voc_encode(const int16_t pcm[160], uint8_t *bits, int *nbits);   // 82 00 A0 … → 81 01 nbits data
int  sct_voc_decode(const uint8_t *bits, int nbits, int16_t pcm[160]);    // 81 01 nbits data → 82 00 A0 …
```

Use the stock SPI2 link: frame layout as in §2; CS# PB12; read while PD11 (byte ready) and PD7 (frame pending) are high;
dummy-write the last TX byte to clock each RX byte (`FUN_0810a674` / `FUN_0810a6dc` / `FUN_08103838`).

## 6. What must be measured on a real radio (phase 0)

No flash writes are needed. Either use the USB passthrough (§1: power on with `#` + PF3 held, then SCT_Port or a
script on the USB CDC port), or run a RAM stub over SWD.

1. Send `80 30` and `80 31`. Record the reply bytes. This tells whether the vocoder packet port exists and which
   strings it returns.
2. Send `80 09 21`, then one 0x82 SPCHD packet of a 1 kHz tone. Is there a 0x81 reply? How many bits? How long until it
   arrives? Repeat with `80 09 22` and with the D-STAR RATEP.
3. If nothing comes back: send `05 A3 00` (Set_HPIMODE) or `00 18 00 00 07` (DEBUG_SPEECH_HPI), then repeat step 2.
   Also try `00 18 03 50 00` (MLOOP_VOCODER).
4. Decode: send the 0x81 frames captured in step 2 back to the chip. Is there a 0x82 reply? Does it sound like the
   tone? Also check whether it plays on the speaker on its own, which would mean the vocoder is wired to I2S only.
5. Find the frame timing. Can the MCU stream 50 frames per second each way while a DMR call is idle?
6. Write down every unsolicited frame that appears during a stock DMR RX and TX call. Record over passthrough, with
   stock firmware.
