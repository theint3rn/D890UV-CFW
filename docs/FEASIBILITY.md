# D890UV custom firmware — hardware and vocoder feasibility

Date: 2026-11-18. Base: stock firmware V1.05 (Ghidra project `ghidra/D890UV.gpr`, lifted C in
`D890UV_src/src/lifted/f_<addr>.c`), SCT tool package in
`D890UV V1.05 DMR official release 260521/base band 3288 upgrade/SCT_Port 3288/`, emulator models
`emu/models/RfTransceiver.cs` and `emu/models/Baseband.cs`.

Target modes: DMR Tier III, P25 Phase 1 + Phase 2 with trunking, D-STAR, NXDN, YSF (C4FM).

**Short answer.** The RF hardware can carry every target mode **except P25 Phase 2 reception at full
quality** (H-DQPSK wants a coherent I/Q receiver; this radio has an FM discriminator). The hard part is
the **baseband path, not the RF**: the stock design sends all digital-mode TX audio through the SCT3288
baseband and an audio codec that the MCU only reaches through the SCT3288. The MCU has one slow RX audio
input (ADC1 ch1, sampled at 4.8 kHz × 8 bit for APRS) and no audio output of its own. So:

* DMR (Tier II now, Tier III maybe) is the low-risk path: the SCT3288 already does it.
* Every other mode needs either (a) an undocumented SCT3288 raw/PCM mode to work, or (b) a small
  hardware change (MCU ADC on the discriminator + MCU DAC/PWM on the modulator input).
* Vocoders: the SCT3288 exposes a **DVSI AMBE-3000-style packet interface**. If its rate table can be
  changed, it covers AMBE+2 (DMR, NXDN, YSF DN, P25 Ph2) and maybe D-STAR AMBE. IMBE (P25 Ph1, YSF VW)
  must run on the MCU (patents expired; open-source code exists, GPL).

Everything marked **[test]** below must be confirmed on real hardware or in a bench setup before it is
relied on. Nothing here needs a flash write to the real radio; tests can run from RAM over SWD.

---

## 1. RF transceivers

### 1.1 What the code shows

Details and the full register table: [`rf_chip.md`](rf_chip.md).

* Two identical chips on one 3-wire bus: SCK PB3, SDA PB4, CS# PB5 (chip 0) / PD3 (chip 1). Write
  `FUN_080e2f64(reg, val16, chip)`, read `FUN_080e2cac(reg, chip)`. Frame = `(reg<<1)|R` + 16 bits.
* Self-test `FUN_0801c8f4`: `0x50=1`, `0x58=0xF1F0`, then write `0x57=0xA888` and read back (100 tries).
  This is a **bus loop-back on a scratch register**, not a chip ID (init writes `0x57=0xA008`).
* Frequency: `FUN_08116674` writes a 32-bit word to **0x69 (high) / 0x68 (low)**, value most likely in Hz.
* Per-band tables at a 300 MHz threshold (`FUN_081155ac`), per-mode filter/AGC tables
  (`FUN_0810ea14(digital?, ?, chip)`), coefficient downloads (reg 0x3D, 80 writes from tables at
  `0x0814FF30`/`0x08150110`), I/Q-style gain/phase calibration pairs 0x1C–0x23 (`FUN_080c39e4`,
  `FUN_080c3d18`).
* CTCSS/DCS detector inside the chip (scan in `FUN_080d5bd4`, result regs 0x0B–0x0E).

### 1.2 Part number

**Not identified.** The 3-wire framing is the Beken/Auctus style, but:

| Candidate | Why not |
|---|---|
| BK4819 | frequency is 0x38/0x39 in 10 Hz units; never written here. Its 0x68/0x69 are tone-scan results. |
| AT1846S / RDA1846 | frequency is 0x29/0x2A in 1/16 kHz; here 0x29/0x2A get fixed init values. |
| BK4811B | PLL word in REG113/114. |
| BK4829 | no public register map found; cannot rule in or out. |

Best guess: a newer low-IF/zero-IF single-chip FM/4FSK transceiver with internal DSP (Hz frequency word,
image calibration, filter downloads), built for Chinese DMR radios. Two chips → one per band/RX path; the
AMSAT report of full-duplex satellite use fits a separate RX and TX chip. **[test]** read the shield
marking, or read regs 0x00–0x0F over SWD (read-only).

### 1.3 Modulation and what reaches the MCU

| Signal | Can the RF chip do it? | Evidence / reasoning |
|---|---|---|
| FM / CTCSS / DCS / DTMF | yes | stock features; tone bank reg 0x2E, decoder 0x0B–0x0E |
| 2FSK/4FSK 4800 sym/s, ±1.9 kHz (DMR) | yes | stock DMR works; baseband from SCT3288 → FM modulator |
| C4FM 4800 sym/s ±1.8 kHz (P25 Ph1, YSF) | yes (RF level) | same symbol rate and deviation as DMR, different pulse shape (made by the baseband source) |
| NXDN 4FSK 2400/4800 sym/s | yes (RF level) | slower/same rate; SCT3288 already does dPMR = 4FSK 2400 sym/s, RRC 0.2 like NXDN 4800 |
| GMSK 4800 bit/s, BT 0.5 (D-STAR) | yes (RF level) | 2-level FM, within modulator bandwidth |
| H-CPM TX (P25 Ph2 inbound, 6000 sym/s) | probably | CPM is constant-envelope FM; needs a DC-coupled modulator with ≥ ±3 kHz linear range **[test]** |
| H-DQPSK RX (P25 Ph2 outbound, 6000 sym/s) | degraded | not constant-envelope; a discriminator loses amplitude info. Integrate-and-dump of the discriminator over each symbol recovers the phase step (DQPSK = phase difference), with ISI loss. Scanner-grade at best. |
| Raw I/Q to the MCU | **no** | no register write in V1.05 enables a digital I/Q/IF port; no SPI/SAI/DCMI peripheral of the MCU is wired to the RF chips (only the bit-banged control bus). |

**RX audio into the MCU** — one path:

* `ADC1` scan + GPDMA → `0x20001D34` (ch1 = audio, ch2 = battery). Timer TIM6 IRQ (`IRQ49_Handler`
  0x08120EC0) calls `FUN_080d7d70`, which takes `DAT_20001d34 >> 4` (8 bit) into a 4-byte buffer and feeds
  `FUN_0813f3d8`, a zero-crossing/correlator **AFSK-1200 (APRS) demodulator** (state built by
  `FUN_0813f2f0(0x12C0 = 4800)`; packets read by `FUN_08020b40`).
* TIM6 setup `FUN_08063048(ARR, PSC)`: the APRS start `FUN_080d7c30` uses `(249, 99)` → 120 MHz / 100 /
  250 = **4800 Hz sample rate**. The ADC itself can do > 1 MS/s at 14 bit, so a custom firmware can sample
  at 24–48 kHz on the same pin.
* Unknown **[test]**: is ADC1 ch1 the flat, DC-coupled **discriminator** output, or de-emphasised,
  band-limited speaker audio? 4FSK/C4FM/GMSK demod needs the discriminator (flat to ≥ 3 kHz, ideally DC).
  APRS works with either, so the stock code proves nothing. Probe it with a scope while receiving a 4FSK
  signal.

**TX audio from the MCU** — **corrected 2026-11-22, see `AUDIO_PATHS.md`**: there is one. Stock plays MDC-style
data (9600 samples/s) and APRS AFSK 1200 (76.8 kHz PWM + GPDMA) as **TIM4_CH4 PWM on PB9**, uses DAC1 ch1 (PA4) as TX
power control (APC) and DAC1 ch2 (PA5) as the UHF RX front-end tune; the RX decoder on ADC1 ch1 is the MDC-style
signalling decoder (4800 Hz), not APRS. The original (wrong) analysis follows:

* No DAC (`0x46021800`), SAI (`0x40015400`) or I2S use anywhere in the lifted code.
* APRS TX: no 1200/2200 Hz constant found in the MCU code; AFSK is most likely made by the SCT3288 or
  the RF chip's tone bank (reg 0x2E). **[test]**
* Digital TX baseband comes from the SCT3288 side (its codec DAC). The MCU drives the codec only through
  the SCT3288 (B0 = 0x40 messages: `FUN_080e7e80`, `FUN_080f265c`, `FUN_080f2540`, `FUN_080f2594`,
  `FUN_080f26d4`, `FUN_080f2704`; codec register numbers 0x08, 0x0B, 0x0F, 0x10, 0x11). The SCT tool
  calls this "TI" (TLV320AIC3204 family, `AI3204I2C.dll`) and "WM" (Wolfson) register pass-through.

---

## 2. SCT3288 baseband

### 2.1 Link (known)

SPI2, CS# PB12, data-ready PD11, frame-pending PD7. Frame `84 A9 61 LenH LenL B0 ID data… [pad]`.
MCU queue `FUN_080122ec(id, ptr)` → sender `FUN_0802dff8` (length per ID from table `0x0815699D`, stride 2;
B0 in `DAT_20004620`, set by `FUN_080c43d4`). RX parser `FUN_0813af78`, wait `FUN_080dee40`. Boot
handshake as in `CLAUDE.md` (raw `74 A9 01 00 01 00`, `FUN_08100664`).

`B0` byte = message group. From the MCU code and the SCT tool XMLs:

| B0 | Group | MCU uses it | Evidence |
|---|---|---|---|
| 0x00 | modem/system control (work mode 0x18, vocoder select 0x10, gains, scrambler 0x2A, …) | yes (`FUN_080c43d4(0)`, 24 sites) | `CommandArray.xml` |
| 0x03 / 0x05 / 0x06 | protocol layer: 0x03 = dPMR fields (0x40–0x61), 0x05 = DMR fields (0x7E called/own ID…), 0x06 = ? | yes, chosen in `FUN_080f248c` from mode `DAT_20000ff0` (2 → 5, 3/4 → 6, else 3) | `CommandArray_p_dmr_control.xml`, `_layer2_3.xml` |
| 0x20 / 0x23 / 0x25 | read requests (0x23 = query, bit5 = "far"/reply flag) | yes (`FUN_080c43d4(0x23)` before 0x32/0x33) | `Q_PRO_VER 23 30`, … |
| 0x40 | audio codec register pass-through (TI/WM over the SCT3288's I²C) | yes (6 functions above) | `CommandArray_p_ti.xml`, `_wolfsong.xml` |
| 0x80 / 0x81 / 0x82 | **AMBE-3000 packet interface**: control / channel / speech | **no** (never used by V1.05) | `CommandArray_p_ambe3000.xml` |
| 0x05 0xA0–0xAF | boot, flash, HPI/SPI select, RF init, TX/RX start | boot only | `_program.xml` |

### 2.2 What it can do beyond DMR

From the command names in the SCT tool (`SCT_File/SCT_MAIN/*.xml`; parsed list of 862 commands):

* **dPMR** full stack (FS1–FS4 sync patterns, CCH, header, end, ARQ, colour code, IDs). dPMR is
  4FSK 2400 sym/s with RRC α 0.2 — the same physical layer as NXDN 4800 (frame sync and FEC differ).
* **Analog** features: CTCSS/DCS (`CTC_PARAM`), DTMF, five-tone, MSK (`ANALOG_FEATURE_*`, `MSK_1/2`),
  sub-audio, VOX, AGC, equaliser.
* **Vocoder**: `VOCODER_SEL 00 10 04`, `VOCODER_CMODE`, `VOCODER_INI`, `VOCODER_TONE`,
  `VOC_ACTIVATION` (per-chip vocoder licence key, 0x96), and an **AMBE-3000-compatible packet API**:

  | Command | Bytes | DVSI AMBE-3000 meaning |
  |---|---|---|
  | `AMBE_PKT_PRODID` | `61 00 01 80 30` | PKT_PRODID (0x30) |
  | `AMBE_PKT_VERSTRING` | `61 00 01 80 31` | PKT_VERSTRING (0x31) |
  | `AMBE_PKT_RESET` | `… 80 33` | PKT_RESET |
  | `AMBE_PKT_RESETSOFTCFG` | `… 80 34 20 00 00 20 00 00` | PKT_RESETSOFTCFG |
  | `AMBE_PKT_HALT` / `GETCFG` / `LOWP` / `ECMODE` | `80 35` / `80 36` / `80 10` / `80 05 00 40` | same IDs as DVSI |
  | `AMBE_CHAND` | `61 00 0B 81 01 48 <9 bytes>` | channel packet, 0x48 = **72 bits** (= 3600 bit/s AMBE+2 with FEC, DMR rate) |
  | `AMBE_SPEECHD` | `61 01 42 82 00 A0 <320 bytes>` | speech packet, 0xA0 = **160 samples** = 20 ms @ 8 kHz, 16 bit |

  So the chip can take **PCM in → AMBE+2 bits out and back**, the same way a DVSI AMBE-3000 / ThumbDV
  does. The tool does not list PKT_RATET (0x09) / PKT_RATEP (0x0A); whether the SCT core accepts them
  (needed for 2450-no-FEC "rate 34" or D-STAR "rate 0") is unknown **[test]**.
* **Work modes** (`00 18 a b c`): idle, RX, TX, duplex, BER test, plus loop-backs: codec, decimator,
  4FSK, packet, vocoder, vocoder→4FSK (`MLOOP_*`, `ALOOP_*`, `AMLOOP_*`), `DEBUG_SINEWAVE`, `DEBUG_FS1`,
  and **`DEBUG_SPEECH_HPI` (`00 18 00 00 07`) = speech samples over the host port**.
* Host port selectable: `Set_HPIMODE` / `Set_SPIMODE` (`05 A3`); SCT3250/SCT3252 reset variants → the
  SCT3288 is a member of the SCT325x dPMR/DMR baseband family.

Stock V1.05 sends these 0x00-group IDs (from all `FUN_080122ec` call sites): 0x01 0x02 0x06 0x08 0x09
0x0B 0x0F 0x10 0x11 0x15 0x18 0x19 0x1A 0x1C 0x1D 0x1F 0x20 0x21 0x26 0x2A 0x2B 0x2C 0x2F 0x30 0x32 0x33
0x39 0x3C 0x3D 0x3E 0x41 0x43 0x46 0x4C 0x68 0x69 0x6A 0x6B 0x6C 0x6F 0x70 0x73 0x74 0x75 0x77 0x78 0x7A.
Work-mode `0x18` is sent by `FUN_081003d0`, `MODEL_SEL 0x2B` by `FUN_080f38e4`.

### 2.3 Can the MCU bypass the DMR stack?

| Use | Status | How to test |
|---|---|---|
| **Vocoder only** (PCM ↔ AMBE+2 frames over SPI2) | **likely** — packet API exists, DVSI-style IDs | send `AMBE_PKT_PRODID`/`VERSTRING` with B0 0x80; then a `SPEECHD` packet in `ALOOP_VOCODER_LOOP`; check `CHAND` reply. Bandwidth: 320 B/20 ms PCM + 9 B channel = 16 kB/s each way, trivial on SPI2 |
| PCM to/from codec (MCU-made baseband via the codec DAC) | **maybe** — `DEBUG_SPEECH_HPI` | if the MCU can stream 8 kHz PCM to the codec DAC that feeds the modulator, the MCU can make **any** 4FSK/GMSK waveform… but 8 kHz is too slow for 4800 sym/s shaping (needs ≥ 24 kHz) and the analog TX path has HPF/pre-emphasis. Probably not usable for TX baseband. |
| Raw 4FSK symbols (dibits in/out, no DMR framing) | **unknown** — only loop-backs (`MLOOP_4FSK_LOOP`, `MLOOP_PACKET_LOOP`) are named | look for a "transparent"/BER mode (`WORK_MODE_BER 00 18 03 00 05`) that passes payload bits; sniff `PROTOCOL_OPTION 0x55`, `PROCESS_MODE 0x1A` and `REPORT_FIELD 0x1D` values |
| Change symbol rate / sync word (for P25, NXDN) | **partial** — dPMR mode already runs at 2400 sym/s with programmable FS1–FS4 sync words (`03 4C…4F`) | dPMR FS words are 24–48 bit and loadable → maybe NXDN sync; P25/YSF need 4800 sym/s with 48/40-bit sync — test if DMR-mode sync is settable |
| Reflash the SCT3288 with own code | no | its image is encrypted; out of scope (do not break it) |

---

## 3. Vocoders per mode

| Mode | Vocoder | Bits/20 ms (voice+FEC) |
|---|---|---|
| P25 Phase 1 | IMBE full-rate 4400 + 2800 FEC | 88 + 56 = 144 |
| YSF VW (voice wide / full-rate mode) | IMBE (same as P25 Ph1) | 144 |
| P25 Phase 2 | AMBE+2 half-rate 2450 + 1150 FEC | 72 |
| DMR | AMBE+2 2450 + 1150 FEC | 72 |
| NXDN | AMBE+2 2450 + 1150 (EHR) | 72 (NXDN FEC differs) |
| YSF DN | AMBE+2 2450 + 1150 | 72 (YSF FEC differs) |
| D-STAR | AMBE (2020) 2400 + 1200 FEC | 72 |

All AMBE+2 modes share the same **2450 bit/s voice core** (49 bits/frame); only the FEC and interleave
differ. A vocoder that exposes "rate 34" (2450, no FEC) plus MCU-side FEC covers DMR, NXDN, YSF DN and
P25 Ph2 with one engine.

### 3.1 Options

| Option | Covers | Licence / patents | CPU on M33 | Notes |
|---|---|---|---|---|
| **SCT3288 built-in AMBE+2** (packet API) | DMR for sure; other AMBE+2 modes if rate 34 / RATEP works; D-STAR only if the core has AMBE (non-+2) rates | per-chip vocoder key already in the radio (`VOC_ACTIVATION`); using the chip as shipped is fine | ~0 (SPI copy) | **best path** for all AMBE+2 modes **[test]** |
| **mbelib** (open-source decoder) | decode IMBE 7200x4400, AMBE+2 3600x2450, AMBE 3600x2400 (D-STAR) | ISC-style permissive licence; its README warns about DVSI patents. RX only. | decoder: float, sinusoidal synthesis; estimate 15–40 MHz per stream with table sin/cos **[bench]** | quality is "OK", worse than DVSI for AMBE+2 |
| **OP25 IMBE encoder** (Pavel Yazev fixed-point IMBE vocoder, in OP25 / "imbe_vocoder") | encode + decode IMBE (P25 Ph1, YSF VW) | **GPLv3** → whole firmware must be GPLv3-compatible when linked | fixed-point, written for DSPs; estimate 40–80 MHz per direction **[bench]** | only open IMBE encoder of usable quality |
| Open AMBE+2 / AMBE encoder | none of usable quality exists in the open | — | — | do **not** use code extracted from DVSI chips or other radios' firmware (e.g. MD-380 vocoder dumps): proprietary |
| **External DV dongle** (DVSI AMBE-3000R: ThumbDV / DV3000 style) | all AMBE, AMBE+2, IMBE-compatible rates | licensed per chip by DVSI | ~0 | needs a UART + power; only reachable via USART1 (programming cable) or UART4 (BT/accessory port, BT module would have to be removed). Dev/bench use, not a handheld solution. |

### 3.2 Patent status (not legal advice — check before shipping anything)

* **IMBE**: the core DVSI IMBE patents date from the early 1990s and have **expired**. Open
  implementations are used freely in OP25/SDR software.
* **AMBE (D-STAR AMBE-2020)**: early AMBE patents (mid-1990s filings) have expired by now; DVSI still
  sells it as a licensed product, and later improvements may be covered.
* **AMBE+2** (half-rate, tone handling, newer estimation methods): several DVSI patents were filed in the
  2000s–2010s; some are likely **still in force into the late 2020s/2030s**. Using the SCT3288's
  built-in (already licensed) vocoder or a DVSI chip avoids this. A software AMBE+2 **encoder** in the
  firmware is a patent risk and is not planned.
* mbelib itself is permissively licensed; the patent question is separate from the code licence.

### 3.3 CPU budget

STM32U575: Cortex-M33 with FPU (single) + DSP extension, 160 MHz max (stock runs 120 MHz,
`SystemCoreClock` 0x20000FF4; 160 MHz needs voltage range 1). Rough per-mode load:

| Load | MHz (estimate) |
|---|---|
| RTOS, UI, LCD, keypad | 5–10 |
| 48 kHz demod: FIR + symbol timing + slicer (4FSK/C4FM/GMSK) | 10–20 |
| FEC (Golay, Hamming, RS, trellis/Viterbi, BCH(63,16) for P25 NID) | 5–15 |
| AMBE+2 via SCT3288 | ~1 |
| IMBE decode (mbelib or Yazev) | 15–40 |
| IMBE encode (Yazev) | 40–80 |
| **P25 Ph1 TX worst case (encode + modulate)** | **~70–120** of 160 → fits, little margin |

Benchmark the IMBE encoder early (phase 1 of the plan); if it does not fit, P25 Ph1/YSF VW TX need a DV
dongle.

---

## 4. Flash and RAM budget (STM32U575, 2 MB / 768 KB)

**Never write 0x08000000–0x0800BFFF** (factory bootloader, incl. the activation key at 0x0800A000),
option bytes or RDP.

| Flash region | Size | Stock use | Custom FW |
|---|---|---|---|
| 0x08000000–0x0800BFFF | 48 KB | bootloader + activation key | **untouchable** |
| 0x0800C000–0x081A5007 | ~1.6 MB | stock app (code to ~0x081497E6, data, scatter table 0x081A42F8) | custom app |
| 0x081A5008–0x081E3FFF | 252 KB | free | custom app |
| 0x081E4000–0x081EFFFF | 48 KB | runtime table cache (copied from NAND 0x7C000) | keep or reuse |
| 0x081F0000–0x081FFFFF | 64 KB | no reference in lifted code | settings/journal |
| **App total 0x0800C000–0x081FFFFF** | **2000 KB** | | |

The custom app must keep its vector table at 0x0800C000 (bootloader jumps there). The activation-key
check (`FUN_080215c0`) is in the app, so a custom app does not need it.

Estimated custom image:

| Part | Flash KB | RAM KB |
|---|---|---|
| RTOS, HAL, drivers (LCD, keypad, NAND, USB CDC, GPS, BT, RF, SCT3288) | 120–180 | 40 |
| UI + menus (fonts/logo stay in NAND 0x40000–0x452800) | 100–200 | 20 |
| DSP modem (4FSK/C4FM/GMSK, 48 kHz, RRC/Gauss FIRs) | 20–30 | 30 (sample + symbol buffers) |
| FEC library | 20–30 | 10 |
| DMR Tier II/III (CSBK, TSCC, ALOHA, slot timing) | 60–100 | 20 |
| P25 Ph1 + trunking (TSBK, NAC, LC, registration/affiliation, grant follow) | 60–100 | 20 |
| P25 Ph2 (TDMA framing, MAC PDUs, H-CPM/H-DQPSK DSP) | 50–80 | 30 |
| NXDN, D-STAR, YSF | 30–60 each | 10 each |
| mbelib + IMBE codec (tables) | 80–120 | 20 |
| **Total** | **~700–1100** | **~250–300** |

Fits with ≥ 45 % flash spare. RAM: stock uses 0x20000000–0x20025FA0 (152 KB); the U575 has SRAM1
192 KB, SRAM2 64 KB, SRAM3 512 KB, SRAM4 16 KB, so ~300 KB is easy. Codeplugs, talkgroup/site lists and
contacts stay in the 512 MB SPI-NAND (stock layout: settings 0x03500000, contacts 0x07080000).

---

## 5. Mode matrix

Paths: **SCT** = SCT3288 does modem + vocoder; **SCT-voc** = SCT3288 as AMBE+2 vocoder only;
**MCU-DSP** = MCU does modem on ADC1 (RX) and needs a TX baseband source; **HW-mod** = needs a hardware
change (route discriminator to an ADC pin, add DAC/PWM → modulator input).

| Mode | RX | TX | Path | Blockers / risks |
|---|---|---|---|---|
| Analog FM | yes | yes | RF chip + SCT3288 analog | none (stock) |
| DMR Tier I/II | yes | yes | SCT | none (stock); custom FW must re-implement the SCT3288 control messages |
| **DMR Tier III** | likely | likely | SCT (+ MCU for CSBK/TSCC logic) | need SCT3288 to pass raw CSBK/control bursts and allow slot-timed random-access TX **[test]**; stock FW has no trunking, so this is unknown territory |
| dPMR | yes | yes | SCT | not one of the target modes, but free |
| **NXDN 4800 (2400 sym/s)** | maybe | maybe | SCT dPMR modem with NXDN sync **or** MCU-DSP + SCT-voc | dPMR modem must accept NXDN sync/frames (unlikely to be fully raw); else needs discriminator at ADC + TX baseband (HW-mod) |
| NXDN 9600 (4800 sym/s) | maybe | maybe | as above via DMR-rate modem | same |
| **P25 Phase 1 conventional** | yes* | yes* | MCU-DSP (C4FM) + MCU IMBE | *needs discriminator on ADC1 (HW-mod if ch1 is filtered audio) and a TX baseband source (HW-mod); IMBE encoder CPU **[bench]** |
| **P25 Phase 1 trunking** | yes* | yes* | as above | control-channel decode is cheap; TX ISP needs the same TX path; follows grants by retuning reg 0x68/0x69 |
| **P25 Phase 2** | degraded* | maybe* | MCU-DSP + SCT-voc (rate 34) | H-DQPSK RX via discriminator = reduced sensitivity; H-CPM TX needs a clean DC-coupled modulator with fine pulse shaping (HW-mod); AMBE+2 half-rate via SCT3288 if rate 34 works |
| **D-STAR** | yes* | yes* | MCU-DSP (GMSK) + vocoder | vocoder: SCT3288 only if it supports AMBE 2400 (RATEP) **[test]**; else mbelib RX only, TX only with a DV dongle |
| **YSF DN** | yes* | yes* | MCU-DSP (C4FM) + SCT-voc | same TX/RX path issue as P25 Ph1; AMBE+2 via SCT3288 rate 34 |
| **YSF VW** | yes* | yes* | MCU-DSP + MCU IMBE | as P25 Ph1 |
| APRS (AFSK 1200) | yes | yes | stock (ADC1 4.8 kHz) | none |

`*` = possible **only** after the TX/RX baseband question is solved (see phase 0).

### Biggest blockers, in order

1. **TX baseband source for non-DMR modes.** (2026-11-22: PB9 = TIM4_CH4 PWM is the stock MCU TX audio path and the
   cfw uses it - `AUDIO_PATHS.md`; its filtering / pre-emphasis still needs a bench test.) Originally: no MCU DAC/SAI path exists. Needs either an SCT3288 raw/PCM
   mode that reaches the modulator flat and DC-coupled, or a hardware mod (STM32U575 DAC1 on PA4/PA5 if
   free, or a PWM pin + RC filter into the RF chip's modulation input).
2. **RX discriminator access.** ADC1 ch1 is the only RX audio input; its filtering is unknown.
3. **Vocoder rates on the SCT3288.** Decides whether NXDN/YSF DN/P25 Ph2/D-STAR voice is free or needs
   a dongle.
4. P25 Ph2 H-DQPSK on a discriminator receiver (quality, not feasibility).

---

## 6. Recommended phased plan

**Phase 0 — answer the open questions (no flash writes; RAM code over SWD or the emulator)**
1. `hw.py backup` first (bootloader copy). Attach under reset (PA14 is also a key).
2. Read the RF chip marking; read RF regs 0x00–0x0F (read-only) for a chip ID.
3. Scope ADC1 ch1 (and the RF chip's audio pins) while receiving a DMR signal: is the 4FSK eye visible
   and flat? This decides RX for all MCU-DSP modes.
4. Over SPI2 from a RAM test stub: send `AMBE_PKT_PRODID` / `VERSTRING` (B0 0x80); try PKT_RATET 33/34
   and PKT_RATEP for D-STAR; run `ALOOP_VOCODER_LOOP` with a speech packet. Try `DEBUG_SPEECH_HPI`.
5. Look on the PCB for free DAC pins (PA4/PA5) and the RF chip modulation input; plan the HW-mod.
6. Model the findings in `emu/models/Baseband.cs` (AMBE packet replies) so the CFW can be developed in
   Renode.

**Phase 1 — platform + DMR** (owner: `cfw/` skeleton)
* Boot from 0x0800C000, drivers, UI, NAND codeplug, RF chip driver from `rf_chip.md`.
* Analog FM and DMR Tier II through the SCT3288 (re-use the message set listed in §2.2).
* Benchmark mbelib decode and the IMBE encoder on the U575 at 120 and 160 MHz.

**Phase 2 — receive-only digital modes** (owners: `proto/p25`, `proto/dmr_nxdn`, `proto/dstar_ysf`)
* MCU-DSP demod at 24–48 kHz on ADC1 (if phase 0 says the signal is usable): P25 Ph1 + trunking
  control-channel follow, YSF, NXDN, D-STAR.
* Voice: IMBE on the MCU; AMBE+2 through the SCT3288 vocoder; D-STAR via mbelib if the SCT3288 lacks AMBE.
* This gives a strong scanner even if TX never works.

**Phase 3 — DMR Tier III** (SCT-based)
* Only if phase 0/1 show the SCT3288 passes CSBKs and lets the MCU time TX bursts.

**Phase 4 — transmit for non-DMR modes** (needs the HW-mod or an SCT3288 raw mode)
* P25 Ph1 conventional → trunking (ISP: registration, affiliation, voice request), YSF, NXDN, D-STAR.
* Test **only into a dummy load** first; check deviation and spectrum against each standard.

**Phase 5 — P25 Phase 2**
* H-CPM TX and H-DQPSK RX (integrate-and-dump on the discriminator), AMBE+2 via the SCT3288. Expect
  reduced RX sensitivity; treat as best-effort.

---

## Evidence index

| Topic | Address / file |
|---|---|
| RF write / read / self-test / init / freq | `FUN_080e2f64`, `FUN_080e2cac`, `FUN_0801c8f4`, `FUN_0805ffe8`, `FUN_08116674` |
| RF band / mode tables | `FUN_081155ac`, `FUN_0810ea14`, `FUN_080c39e4`, `FUN_080c3d18` |
| RF tone scan | `FUN_080d5bd4` |
| APRS RX sample path | `IRQ49_Handler` 0x08120EC0 (TIM6), `FUN_080d7d70`, `FUN_0813f3d8`, `FUN_0813f2f0`, `FUN_08020b40`, TIM6 setup `FUN_08063048`, start `FUN_080d7c30` |
| ADC config | `FUN_0804c62c`, `FUN_0804d4f4`, `FUN_0804d82c` (ADC1 0x42028000, ADC4 0x46021000) |
| SCT3288 queue / send / B0 | `FUN_080122ec`, `FUN_0802dff8`, `FUN_080c43d4`, protocol group `FUN_080f248c`, work mode `FUN_081003d0`, model select `FUN_080f38e4` |
| Codec pass-through (B0 0x40) | `FUN_080e7e80`, `FUN_080f2540`, `FUN_080f2594`, `FUN_080f265c`, `FUN_080f26d4`, `FUN_080f2704` |
| SCT command names | `…/SCT_Port 3288/SCT_File/SCT_MAIN/CommandArray*.xml`, `1111111111.xml`, `UartContrlMcuCmd.xml` |
| Emulator models | `emu/models/RfTransceiver.cs`, `emu/models/Baseband.cs` |
