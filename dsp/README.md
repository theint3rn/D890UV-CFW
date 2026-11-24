# cfw/dsp — MCU-DSP demodulators and modulators

The D890UV has no I/Q path. The only receive input the MCU can see is ADC1 ch1, the FM discriminator / audio
of the RF chip (`docs/FEASIBILITY.md`). This library turns discriminator samples into symbols for every digital
mode cfw supports. Portable C99, float (Cortex-M33 FPU), no malloc, no libc beyond `<math.h>`.

API: `dsp_demod.h` — `dsp_init(&d, mode, fs_in, &opts)`, `dsp_process(&d, x, n, sym, soft, max)`,
`dsp_quality()`, `d.locked`. Input: samples proportional to frequency, any scale / DC / polarity.
Output: dibits (+3→01, +1→00, −1→10, −3→11) or GMSK bits ('1' = +dev) + a confidence per symbol.

## Chain

| Stage | What |
|---|---|
| Decimate | 48 kHz ADC → 24 kHz (pair average) |
| Inverse de-emphasis (option) | y = x + τ·dx/dt, for an audio path with 1st-order de-emphasis (750 µs default when enabled) |
| Matched filter | RRC α 0.2 span 8 (DMR, NXDN); integrate-and-dump 1 symbol (C4FM: P25, YSF — TX is RC + inverse sinc; and H-DQPSK); Gaussian BT 0.5 span 3 (D-STAR) |
| Timing | Gardner TED on the MF output, linear fractional interpolation, PI loop (kp 0.03, ki 0.0005, ±1 % clamp) — tracks ±100 ppm |
| Coarse AGC / DC | mean of |x − mean| and slow mean, until the first sync word |
| Sync correlator | normalised (Pearson) correlation of the last strobes with the mode's sync words (DMR 4 × 24, P25 24, YSF 20, NXDN 10, D-STAR 24 + 15 bit, P25P2 S-ISCH 20); threshold 0.90 (0.96 for < 20 symbols). On a hit: frame lock, polarity and a least-squares DC + level fit on the sync symbols |
| Tracking | decision-directed LMS on DC (frequency offset) and level (deviation error) after lock |
| Slicer | thresholds 0, ±2 (levels ±1, ±3) or 0 (GMSK); soft = distance to the nearest threshold; EVM → quality 0..100 |

Polarity: detected from the sync word, except DMR, whose voice and data sync words are exact inverses of each other
(so polarity cannot be told from them): DMR uses the configured polarity (cfw Menu > RX polarity). While locked,
sync words of the other polarity are ignored.

H-DQPSK (P25 Phase 2 outbound): the integral of the instantaneous frequency over one symbol is the phase step
(±π/4, ±3π/4), so integrate-and-dump of the discriminator gives a 4-level signal with the same 1:3 ratio and the
4FSK chain applies. The cost: FM-discriminator noise (clicks) and ISI, because the RRC-shaped complex symbols are
not matched-filtered before the phase is measured — see the comparison table (~13 dB at BER 1e-2, error floor
about 6e-3). The Phase 2 voice FEC still decoded ≥ 97 % of the voice frames at ≥ 10 dB in the test.

## Tests (`make test`)

`test/chansim.c` builds a realistic receiver input from the proto TX code's test calls (`../build_streams/*.sym`,
`tools/make_streams.sh`): transmitter pulse shaping per mode (RRC 0.2; C4FM RC 0.2 × inverse sinc by numeric
inverse Fourier; GMSK Gaussian BT 0.5 frequency pulse; H-DQPSK RRC 0.2 complex), FM modulation at 48 kHz complex
baseband, frequency offset, symbol clock error, deviation error, AWGN at a C/N in 12.5 kHz, a ±6.25 kHz IF filter,
the FM discriminator, optional 750 µs de-emphasis, and 12-bit ADC quantisation (2048 + 0.25 count/Hz).
`test/test_dsp.c` demodulates it, measures BER against the transmitted symbols (block re-alignment) and feeds the
symbols to the real cfw proto glue (`app/protos/*.c` + `proto/*`) to count decoded voice frames.
Pass: BER < 1e-3 at 20 dB C/N with all impairments (P25P2: ≥ 95 % of voice frames), and the de-emphasis case.
Run time ~10 s. `make waves` writes the emulator sample files (`*.adc16`, `*.adc8`).

Results (2026-11-20):

### BER vs C/N (12.5 kHz), discriminator demod at 24 kHz

Impairments in the `hard` rows: +1 kHz frequency offset, +100 ppm symbol clock, -10 % deviation. Cell = bit error rate (after the first 800 symbols = acquisition) / voice frames decoded by the cfw proto glue, in % of the frames decoded from the clean symbols (above 100 % = a duplicate late-entry frame).

| Mode | channel | 6 dB | 8 dB | 10 dB | 12 dB | 14 dB | 16 dB | 20 dB | 30 dB |
|---|---|---|---|---|---|---|---|---|---|
| DMR | plain | 3.3e-02 / 100% | 8.1e-03 / 100% | 1.6e-03 / 100% | 4.8e-04 / 100% | 6.9e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| DMR | hard | 5.2e-02 / 80% | 1.6e-02 / 80% | 3.1e-03 / 100% | 8.9e-04 / 100% | 6.9e-05 / 100% | 3.4e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25 | plain | 8.8e-02 / 58% | 3.2e-02 / 91% | 1.3e-02 / 100% | 3.5e-03 / 100% | 6.9e-04 / 100% | 2.7e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25 | hard | 1.5e-01 / 41% | 4.1e-02 / 83% | 3.8e-02 / 100% | 9.8e-03 / 100% | 5.9e-03 / 91% | 4.5e-04 / 100% | 1.4e-04 / 100% | 0.0e+00 / 100% |
| YSF | plain | 3.0e-02 / 100% | 8.2e-03 / 100% | 1.7e-03 / 100% | 3.1e-04 / 100% | 4.8e-05 / 100% | 2.4e-05 / 100% | 1.6e-03 / 100% | 0.0e+00 / 100% |
| YSF | hard | 4.6e-02 / 101% | 1.2e-02 / 100% | 3.6e-03 / 100% | 7.3e-04 / 100% | 6.0e-05 / 100% | 1.2e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| NXDN48 | plain | 1.6e-02 / 92% | 1.4e-03 / 100% | 1.0e-03 / 100% | 9.0e-04 / 100% | 8.4e-04 / 100% | 8.4e-04 / 100% | 8.4e-04 / 100% | 8.4e-04 / 100% |
| NXDN48 | hard | 2.1e-02 / 92% | 2.6e-03 / 100% | 1.1e-03 / 100% | 8.4e-04 / 100% | 9.5e-04 / 100% | 8.4e-04 / 100% | 8.4e-04 / 100% | 8.4e-04 / 100% |
| NXDN96 | plain | 2.3e-02 | 3.1e-03 | 1.6e-03 | 8.4e-04 | 6.9e-04 | 9.0e-04 | 8.4e-04 | 8.4e-04 |
| NXDN96 | hard | 2.7e-02 | 6.2e-03 | 1.6e-03 | 1.0e-03 | 9.0e-04 | 9.0e-04 | 8.4e-04 | 7.9e-04 |
| DSTAR | plain | 3.5e-02 / 97% | 1.0e-02 / 100% | 1.2e-03 / 100% | 1.8e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| DSTAR | hard | 5.3e-02 / 100% | 1.7e-02 / 98% | 3.1e-03 / 100% | 4.2e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25P2 | plain | 9.5e-02 / 90% | 5.5e-02 / 94% | 3.3e-02 / 100% | 2.2e-02 / 98% | 1.7e-02 / 100% | 1.3e-02 / 100% | 9.7e-03 / 100% | 6.0e-03 / 100% |
| P25P2 | hard | 9.4e-02 / 87% | 5.6e-02 / 94% | 3.4e-02 / 97% | 2.3e-02 / 97% | 1.7e-02 / 100% | 1.3e-02 / 100% | 1.0e-02 / 98% | 7.2e-03 / 100% |

### Audio path with 750 us de-emphasis (DMR, 20 dB, plain channel)

| receiver | BER |
|---|---|
| no compensation | 3.8e-01 |
| inverse de-emphasis 750 us | 0.0e+00 |

### P25 Phase 2 H-DQPSK: discriminator integrate-and-dump vs coherent differential detector

Coherent reference: matched RRC filter on the complex signal, genie timing, ideal AFC (best case).

| C/N | discriminator BER | coherent BER |
|---|---|---|
| 6 dB | 9.4e-02 | 1.6e-02 |
| 8 dB | 5.6e-02 | 3.5e-03 |
| 10 dB | 3.3e-02 | 2.4e-04 |
| 12 dB | 2.2e-02 | 0.0e+00 |
| 14 dB | 1.6e-02 | 0.0e+00 |
| 16 dB | 1.3e-02 | 0.0e+00 |
| 20 dB | 9.5e-03 | 0.0e+00 |
| 30 dB | 6.1e-03 | 0.0e+00 |

### Work per second of signal (host-independent count)

| Mode | MF taps | MAC/s at 24 kHz |
|---|---|---|
| DMR | 41 | 984000 |
| P25 | 5 | 120000 |
| YSF | 5 | 120000 |
| NXDN48 | 81 | 1944000 |
| NXDN96 | 41 | 984000 |
| DSTAR | 15 | 360000 |
| P25P2 | 4 | 96000 |


The 8e-4 floor of NXDN is the block re-alignment at the start of the 2400 sym/s stream, not a steady-state error
(a symbol-by-symbol check shows errors only in the first ~70 symbols). YSF 1.6e-3 at 20 dB plain is one block
around a call boundary (DG-ID change) — the hard row at 20 dB is error free.

MAC/s is the matched filter only; per-symbol work (Gardner, correlator: DMR checks 4 × 24 symbols every symbol) adds
about the same again. In the emulator the whole receive path (demod + proto decoder) measured 6–35 % of a 100 MIPS
CPU (see cfw/README.md); not measured on the radio.


## Modulators (`dsp_mod.c`, digital TX)

`dsp_mod_run()` turns symbols (pulled from the proto TX framing) into the instantaneous frequency at 48 kHz, i.e. the
baseband an FM transmitter needs; the firmware writes it to DAC1 ch2 (PA5) as `2048 + 0.25 count/Hz`.

| Mode | Frequency pulse | Notes |
|---|---|---|
| DMR, NXDN48/96 | root-raised cosine 0.2, ±4 symbols | levels ±1/±3 × outer/3 at the symbol instants |
| P25 Ph1, YSF | raised cosine 0.2 × inverse sinc (TIA-102.BAAA C4FM shaping), numeric inverse Fourier | |
| D-STAR | GMSK: rectangle ⊗ Gaussian BT 0.5, ±1200 Hz, '1' = + | |
| P25 Ph2 | 4-level CPM, h = 1/4, 1RC pulse confined to one symbol | phase step per symbol exactly ±π/4 / ±3π/4 — what the cfw discriminator H-DQPSK receiver measures. **Not** TIA-102.BBAB H-CPM (its partial-response pulse is not reproduced) and it carries **outbound**-format slots (the proto library has no inbound burst builder): a cfw-to-cfw test link, not interoperable, and with 1RC it occupies ~15 kHz (99 %), wider than a 12.5 kHz channel. |

## Test vocoder (`../voc/voc_sine.c`)

Own harmonic sinusoidal coder (no DVSI, mbelib, OP25 or Codec2 code). It fills the 49-bit AMBE+2 / AMBE payload and
the 88-bit IMBE payload; pitch on the IMBE grid f0 = 16000/(b0+39.5), all-voiced harmonics, per-harmonic log
amplitudes with whatever bits the frame leaves. **TEST ONLY — NOT INTEROPERABLE:** a real radio cannot decode it, and
it is not TIA-102.BABA bit-exact (the standard's quantiser tables are not available to this project). A 440 Hz tone:
439.86 Hz out (4th harmonic of f0 = 109.97 Hz), SINAD 68 dB through the codec alone. The receiver conceals frames whose
FEC failed or needed ≥ 3 corrections (repeat last frame, mute after 3).

`make test` also runs `test/test_tx.c` (results below, `tx_table.md`):

### Digital TX end to end: 440 Hz test tone, 3 s PTT

Chain: 440 Hz -> cfw `sine` TEST vocoder (not interoperable) -> proto TX framing -> cfw modulator (48 kHz) -> channel (`hard` = +1 kHz offset, +100 ppm, -10 % deviation) -> cfw demodulator -> proto RX -> vocoder -> PCM.
Cell: median over 0.25 s blocks of the recovered tone Hz / SINAD dB (60+ = numerically perfect sine) / % of blocks with 440+/-2 Hz and >= 20 dB / voice frames received of sent (concealed: FEC failed or >= 3 corrections). Codec alone: 439.86 Hz, 68 dB (the tone sits on the IMBE pitch grid: f0 = 16000/145.5, 4th harmonic).

| Mode | decoded call | clean 40 dB | 20 dB hard | 14 dB hard | 10 dB hard |
|---|---|---|---|---|---|
| DMR | TG91 < 9990201 TS1 | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (1) | 439.9 Hz / 59 dB / 100 % / 144 of 144 (1) | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (1) | 439.9 Hz / 54 dB / 55 % / 144 of 144 (34) |
| P25 | TG91 < 9990201 | 439.9 Hz / 60 dB / 100 % / 126 of 144 (0) | 439.9 Hz / 60+ dB / 100 % / 135 of 144 (0) | 439.9 Hz / 57 dB / 100 % / 135 of 144 (1) | 444.0 Hz / -32 dB / 22 % / 144 of 144 (62) |
| P25P2 | TG91 < 9990201 S1 | 439.9 Hz / 60+ dB / 100 % / 172 of 180 (0) | 439.9 Hz / 60+ dB / 100 % / 164 of 180 (0) | 439.9 Hz / 60+ dB / 100 % / 176 of 180 (4) | 557.0 Hz / -43 dB / 9 % / 176 of 180 (4) |
| NXDN48 | TG91 < 28729 RAN1 | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (0) | 439.9 Hz / 60+ dB / 100 % / 136 of 144 (0) | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (0) | 439.9 Hz / 60+ dB / 77 % / 144 of 144 (0) |
| DSTAR | N0CALL/CFW > CQCQCQ | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 452.4 Hz / -21 dB / 12 % / 139 of 139 (0) |
| YSF | N0CALL > ALL | 439.9 Hz / 60+ dB / 100 % / 145 of 145 (0) | 439.9 Hz / 60+ dB / 100 % / 145 of 145 (0) | 439.9 Hz / 60+ dB / 88 % / 145 of 145 (0) | 532.7 Hz / -43 dB / 22 % / 145 of 145 (0) |

### Modulators (cfw/dsp/dsp_mod.c): random symbols -> modulator -> channel -> cfw demodulator

| Mode | frequency pulse | peak dev Hz | 99% power BW kHz | BER 30 dB plain | BER 20 dB hard | BER 14 dB hard |
|---|---|---|---|---|---|---|
| DMR | RRC 0.2 | 3555 | 7.4 | 0.0e+00 | 2.7e-05 | 3.0e-04 |
| P25 | RC 0.2 x inverse sinc | 4170 | 8.0 | 3.5e-04 | 5.4e-05 | 3.7e-03 |
| YSF | RC 0.2 x inverse sinc | 6404 | 10.8 | 0.0e+00 | 0.0e+00 | 5.4e-04 |
| NXDN48 | RRC 0.2 | 1895 | 3.9 | 0.0e+00 | 0.0e+00 | 1.1e-04 |
| NXDN96 | RRC 0.2 | 4389 | 8.6 | 0.0e+00 | 0.0e+00 | 1.6e-04 |
| DSTAR | Gaussian BT 0.5 | 1200 | 4.9 | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| P25P2 | 1RC, CPM h = 1/4 | 4500 | 15.0 | 0.0e+00 | 0.0e+00 | 2.1e-04 |

(The end-to-end runs use simplex framing: DMR direct-mode TS1 bursts with 30 ms unmodulated gaps, NXDN RDCH, D-STAR DIRECT.)

Reading the end-to-end table: the 440 Hz tone comes back at 439.9 Hz with ≥ 57 dB SINAD and 100 % good 0.25 s blocks in
every mode down to 14 dB C/N with +1 kHz offset, +100 ppm clock and −10 % deviation (YSF: 88 % at 14 dB). At 10 dB
the residual bit errors land in the test codec's unprotected bits (1 bit per harmonic → a spurious full-level harmonic),
so the tone breaks up except on NXDN; a real AMBE+2/IMBE quantiser is more forgiving. Frame counts below the sent
count are acquisition at the start of the call (P25: first LDU).
