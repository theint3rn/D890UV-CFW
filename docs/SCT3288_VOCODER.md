# SCT3288 vocoder and audio interface (for the cfw `SCT3288` backend)

Status 2026-11-24. Both radios (D890UV, D168UV) use the same chip and the same commands. Sources: stock V1.05 (D890UV)
and V1.07 (D168UV) code, their frames in the emulator logs, the SCT_Port command files
(`tools/sct3288/sct3288_commands.md`), and the public CT3258 Packet Interface, CT3258 / SCT3258 datasheets and
CT3258 Analog Setup (PDFs searched, voice sections read 2026-11-24, `peripheral_fw/sct3258_pdfs/`, keep out of public repos). They describe
the **older CT3258 / SCT3258**, not the SCT3288: matching IDs are strong hints, not proof. The baseband firmware was **not** decrypted.

Marks: **stock** = the stock firmware does exactly this; **doc** = those public documents; **guess** = combination not seen
anywhere; only a real radio confirms it.

**Correction of the 2026-11-20 version:** it said the stock MCU never exchanges PCM or vocoder frames with the chip, and
that the interface was AMBE-3000 packets (TYPE 0x80..0x82). Both were wrong. TYPE bit 7 means "pass to an *external* AMBE
chip" (doc), and the stock firmware moves both frames and PCM with TYPE 0x00 commands 0x01, 0x00 and 0x3E (section 2).

## 1. Where the audio goes

```
mic ─► codec ─(I2S)─► SCT3288: encoder ─► channel frames ─► 4FSK modem ─► RF chip        (stock call)
spk ◄─ codec ◄─(I2S)─ SCT3288: decoder ◄─ channel frames ◄─ 4FSK modem ◄─ RF chip
                          ▲  SPI2: commands, channel frames (ID 01), PCM (ID 00)
                       STM32 / AT32
```

* Microphone and speaker codec hang on the SCT3288, not on the MCU. cfw digital voice uses: TX mic → chip encodes →
  frames to the MCU → MCU's own FEC / modem; RX MCU frames → chip decodes → speaker.
* The codec type: both stock radios send `2D 04` (CODEC_SELECT 4). The public document lists 1 = TLV320AIC3204,
  2 = WM8758B, 3 = ES8338, others reserved, so 4 is unknown and the SCT_Port `TI_*` register tables probably do **not** fit. The cfw writes only the codec registers the
  stock firmware writes (0x08 speaker attenuation, 0x0F / 0x10 microphone, 0x0B at start-up).
* `AUDIO_PATHS.md` calls I2C device 0x35 register 0x0F "baseband codec audio". That device is the broadcast receiver
  (`drivers/bcast.c`), so that line looks wrong.

## 2. The stock voice interface

Frame: `84 A9 61 LenH LenL TYPE ID data… [pad 00 if Len odd]`, Len = bytes after TYPE.

| Command | Meaning | Evidence |
|---|---|---|
| `00 3E <io>` | VOCODER_IO_SET (name: doc), bit mask below | stock D890UV `FUN_080fffe4`, D168UV `FUN_08089c7a` |
| `05 18 03 03 00` | WORK_MODE duplex with audio loop 3 = vocoder loop (codec ↔ vocoder, no RF). `18 00 00 00` = idle | stock `FUN_080fad68`; SCT_Port ALOOP_VOCODER_LOOP |
| `05 1A 02` / `80` | PROCESS_MODE digital / analog | stock, doc |
| `00 10 0C` | VOCODER_SEL digital (doc: 4 AMBE+2, 16 analog). Stock boot sends 04, 10, 0C and waits up to 50 s | stock, doc |
| `00 01 24 <36 B>` | CHAN_D, 4 frames × 9 bytes (80 ms); `1B` + 27 B = 3 frames (60 ms, DMR). Both directions | stock `FUN_08146930` case 1, `FUN_080fb67c`, `FUN_080fe39c`; doc: 36 bytes per 80 ms |
| `00 01 <status>` from the chip | "send the next frames" | stock `FUN_08146930` (flag bit 1) |
| `00 01 FF <fmt>` | frame format: 00 for record / play, 06 for the encryption path (there the MCU works on 49 bits: 6 bytes + 1 bit, rest 0), 02 for another path | stock `FUN_080e724c`, `FUN_0800cfd4` |
| `00 00 00 A0 <320 B>` | PCM, 160 × s16 big endian, 8 kHz, 20 ms. Both directions | stock `FUN_080f851c` (to chip), `FUN_08146930` case 0 |
| `00 00 00` from the chip | "send the next PCM" | stock (flag bit 3) |
| `00 68` / `00 69` + u16 | digital mic / speaker gain (D168UV: 07D0 / 1F40; doc: value / 2048) | stock |
| `40 0F 90`, `40 10 1E` (D168UV 20) | microphone on; `0F B0`, `10 2E` = off | stock `FUN_080f2594` |
| `40 08 00..1F` | speaker attenuation, 1F = mute | stock `FUN_080f268c` |

VOCODER_IO_SET bits, worked out from the stock mode table (D890UV 0x08150FC8 = D168UV 0x080E2F14, identical) and what the
stock handlers do in each mode:

| Bit | Meaning | Stock mode that shows it |
|---|---|---|
| 01 | microphone PCM → MCU | 0xD, 0x15 (recording) |
| 02 | decoded PCM → MCU | 9 = 0E, 0x14 = 02 (recording received audio) |
| 04 | received (demodulated) frames → MCU | 7 = 0C (decrypt) |
| 08 | MCU frames → decoder → speaker | 1 = 08 (play), 7 = 0C |
| 10 | encoder frames → MCU | 5 = 10 (record) |
| 20 | encoder frames → MCU **and** modulator (doc, bits 5:4 = 10; 01 = MCU only). With 40 the modulator takes the MCU frames | 6 = 60 (encrypt) |
| 40 | MCU frames → modulator | 6 = 60, 2 = 40 |
| 80 | MCU PCM → chip | 0xB, 0xF, 0x18 = 80; 10 = E0 |

Doc check (Packet Interface 1.2.44): bits 04, 08, 10/20, 40 match the stock reading above; its examples are local record 20,
remote record 04, local play 08, remote play 40. Bits **01, 02 and 80 are "Reserved" there** (PCM), and it says the command
and CHAN_D are "only used in DPMR mode" on that chip; their use in DMR/vocoder loop rests on the stock firmware only. There
is no command 0x00 (PCM) in that document. `40 ..` = TYPE bit 6 "codec bypass": the bytes after TYPE go straight to the
codec. Answer times: 40 ms normal, CHAN_D 80 ms, VOCODER_SEL 1-3 s, CODEC_SELECT 1 s (doc). VOCODER_SEL 0C is not listed
(4 = AMBE+2, 16 = analog, others reserved). `05 79` = DMR_CALL_OPTION and `00 70` = equaliser coefficients (doc names), so
neither is a vocoder command. The documents list no 4400 bps (IMBE) or 2400 bps-FEC D-STAR rate, only the 3600 bps class.

In analog process mode the stock firmware uses 42 for PCM playback, so bits 02 / 40 mean something else there.

Stock command order the cfw copies (mode 1 = play, mode 5 = record): `01 FF 00`, `19 00`, `18 00 00 00`, gain + codec,
`1A 02`, `18 03 03 00`, `3E io`; end: `3E 00`, `18 00 00 00`, codec off.

## 3. cfw backend (`hal/voc_sct.c`, `hal/voice.c`)

| Mode | What | IO | Status |
|---|---|---|---|
| **A** | TX: mic → chip → frames → MCU (`SCT_ENC_MIC`); RX: frames → chip → speaker (`SCT_DEC`) | 10 / 08 | stock modes 5 and 1 |
| **B** | TX from MCU PCM (test tone, or when the mic path failed: `SCT_ENC_PCM`) | 90 | guess (80 + 10) |
| PCM | mic PCM for the software IMBE coder (`SCT_MIC_PCM`); MCU audio to the speaker (`SCT_PLAY_PCM`) | 01 / 80 | stock modes 0xD and 0xF. In mode 0xF the audio may pass through encoder + decoder |
| **C** | external AMBE packets (80 / 81 / 82), if PRODID answers | - | needs an AMBE chip the radio probably does not have |
| sine | own test codec, not interoperable | - | fallback |

* Probe, once after the baseband is up: `23 32`, `20 10`, `20 1A`, `3E 00`, `80 30`; 200 ms each, nothing blocks. The result
  shows on the screens behind "BB" / "Baseband" (`voc A`, `voc B no mic`, `voc no RX`, `voc no TX`, `voc failed`,
  `voc C ext`, `no vocoder`) and, with `DEBUG=1`, on RTT with the answer bytes.
* A session that does not come up, delivers nothing or takes nothing (800 ms) is closed; after the second time it is not
  used again until the next start. The backend then takes the next mode.
* Frames: the chip's 72 bits with FEC ↔ the 49 bits of the protocol glue (`proto/dmr_nxdn/common/ambe_fec.c`). If 8 frames in
  a row look like raw 49-bit frames the backend switches for receive-from-chip. Gaps and lost frames: the AMBE+2 silence
  frame `F801A99F8CE080` (= the DMR silence frame `B9E881526173002A6B`, checked with the FEC code).
* `g_sct_tap` (0 in HW builds): IO 0A instead of 08, so decoded PCM also comes back for the tone meter.
  `g_sct_force`: 1 chip only, 2 PCM instead of the mic, 3 external AMBE.
* Start-up fix: the cfw sent `2D` without its codec byte, no codec register 0x0B, and no VOCODER_SEL, then waited for an
  ID 10 that the chip only sends as the answer to VOCODER_SEL. It now sends the stock values. The emulator had hidden this.

### Allow-list (`hal/bb_allow.h`, enforced in `bb_send()` on both radios)
25 of 65536 TYPE / ID pairs can pass, with checked lengths and values. WORK_MODE passes only idle and `03 03`. Flash, boot,
program, activation, reset, memory read / write (3F), low power, call and transmit commands, and anything unknown are
refused and counted (`bb_refused()`). `tools/bb_allow_test.c` runs the whole SCT_Port dictionary through it.

## 4. Mode × chip

| Mode | Rate | Chip | Evidence | cfw uses |
|---|---|---|---|---|
| DMR | AMBE+2 3600x2450 | **confirmed in stock** (frames on SPI in record / play / encryption); cfw use **likely** | section 2 | A / B |
| NXDN EHR (6.25 / 12.5) | the same | **likely** | same codec and FEC layout (NXDN TS 1-A vectors match `ambe_fec.c`); the chip needs no NXDN modem for it | A / B |
| YSF DN | the same, own FEC | **likely** | 49-bit frames, FEC changed on the MCU | A / B |
| P25 Phase 2 | the same, own FEC | **likely** | as YSF DN. Pulls 18 frames at once, about 0.5 s of buffer | A / B |
| dPMR | the same | **likely** (native mode of the chip, 80 ms packets) | doc | no cfw dPMR |
| D-STAR | AMBE 3600x2400 | **no** (internal) | VOCODER_SEL has AMBE+2 and analog only (doc). CMODE has an "AMBE compatibility" bit of unknown meaning | C if an external chip answers, else sine |
| P25 Phase 1, NXDN EFR, YSF VW | IMBE 7200x4400 | **no** | no full-rate mode in any command list (doc, SCT_Port); chip frames are 72 bits | software IMBE; mic and speaker through the chip as PCM |

## 5. What else the chip offers

| Feature | Command | Used? |
|---|---|---|
| Mic / speaker digital gain | 68 / 69 | yes, D168UV stock values (no D890UV calibration to read) |
| Codec mic gain, speaker attenuation, mute | 40 0F / 10 / 08 | yes; D168UV Vol Level included |
| Mic PCM to the MCU / MCU PCM to the speaker | 3E 01 / 3E 80 | yes, IMBE modes |
| VOX energy | 3D | no: both radios have their own VOX path |
| Audio AGC, equaliser, gain set | 73, 70, 6B | no: stock values come from calibration, fields not understood; wrong values only damage audio |
| Echo canceller | 80 05 | no: external AMBE chip only |
| Noise suppression / AGC / tone detect in the vocoder | 02 (CMODE) | no: bit meanings partly guessed; stock values 08, 09, 0B, 18 |
| Tone and alert generation | 1F (LOCAL_TONE), 08 (VOCODER_TONE) | no: MCU PWM tones already work, and these can reach the transmit path |
| DTMF / 5-tone / MSK | 0A, 3C, 01 FE, 71, 72 | no: the MCU coders are tested against the stock decoder |
| Scrambler, AES key load | 2A, 01 FF 83 | no: cfw encryption runs on the MCU and is tested against vectors |
| dPMR stack, hardware 4FSK modem | 60.., 43, 18 02 | no: transmits on its own (TX line PD6), bypasses the cfw's TX safety; a job for its own hardware session |
| Codec TI tables (beep, AGC 56) | 40 3C.. | no: codec type 4 is not a TI part as far as known |

## 6. Bench check on a real radio (`make DEBUG=1` only)

Start: RTT key `b`, or `set var g_sct_bench = 1` from the debugger. It sends no transmit command (the allow-list has none),
does not key the radio, stops when a transmission starts, and logs every frame the chip sends (time, TYPE, ID, bytes).

1. Identity reads, VOCODER_SEL / PROCESS_MODE reads, external PRODID / VERSTRING.
2. VOCODER_SEL 0C: time to the answer.
3. Mic → frames, 2 s (speak): packet count and interval, frame length, FEC errors, 72 or 49 bits.
4. Play the recorded frames, then silence (own voice from the speaker = mode A works).
5. 1 kHz PCM → encoder → frames (mode B).
6. Mic PCM, 2 s (peak level). 7. 1 kHz PCM → speaker.
8. External AMBE: rate 33 and one PCM packet (expected: no answer).

Open questions only the radio answers: whether IO 10 / 08 / 90 / 0A behave outside the stock state machine; pacing (does the ID
01 / 00 answer mean "next", as stock treats it); start-up delay of a session (cfw queue sizes assume ≤ 100 ms); whether the
vocoder loop leaks audio to the modulator (scope PB9 / RF chip audio input with the transmitter off); level of mic and speaker
with these gains; whether FM receive audio is affected by the codec settings; whether the chip needs commands `05 79`
and `00 70` that stock sends at start-up (not in the allow-list).

## 7. Emulator model (`emu/models/Baseband.cs`, `D168UV/emu/models/Baseband168.cs`)

A stand-in that follows section 2. **It proves framing and flow only, not what the real chip does.**

* Vocoder loop + IO bits, 20 ms clock, 4-frame packets, "next" answers paced by the play-out queue.
* "Codec": a tone description (`'F'`, frequency in 0.25 Hz, peak / 128) in 49 bits inside the real AMBE+2 72-bit FEC.
* `spi2 MicHz 700` / `MicAmp` = microphone stand-in (heard only while the cfw has switched the mic on);
  `spi2 SpeakerFile @out.raw` = what the speaker would play (s16 LE 8 kHz, only while not muted); `spi2 VoiceStatus`.
* `spi2 VoiceAnswer 0` = chip silent, `2` = refuses IO_SET: fallback tests. `spi2 ExtAmbe true` = the old AMBE-3000 packets
  (default off now). `spi2 FromMcu "84 A9 61 …"` injects MCU bytes.
* Tests: `emu/txrx_loop.py --mic` (tone through the mic stand-in), `--speaker` (measure the speaker stand-in, tap off).

## 8. Tests (emulator, 2026-11-24; default build, two cfw radios, 440 Hz)

| Test | D890UV | D168UV |
|---|---|---|
| mic stand-in → air → speaker stand-in (`--mic --speaker`, tap off = the HW setting) | DMR, NXDN, YSF DN, P25 Ph2: 440.0 Hz; NXDN96 EFR, P25 Ph1 (IMBE, PCM path): 441.4 Hz; 12 / 12 | the same without NXDN96; 10 / 10 |
| test tone through PCM → chip (`--mode all --simplex --rf-gate`) | 16 / 16 (FM, all digital modes; D-STAR = sine codec) | 14 / 14 |
| chip silent / refuses IO_SET, then PTT (`VoiceAnswer 0`, `2`) | no hang, falls back to `sine TEST` on the TX screen | not run (same code) |
| bench check (DEBUG build) | runs to the end, transmitter off, own voice and 1 kHz tone reach the speaker stand-in | not run (same code) |
| stock firmware | `compare.py`: 0 of 20480 pixels differ | boots, baseband init complete |

Builds: 8 per radio, no warnings; `make test-libs` passes (with `tools/bb_allow_test.sh`). The CC / NAC / RAN / DG-ID filter cases
were not repeated. The speaker SNR of the AMBE cases is limited by the stand-in codec.
