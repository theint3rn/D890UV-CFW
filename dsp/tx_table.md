### Digital TX end to end: 440 Hz test tone, 3 s PTT

Chain: 440 Hz -> cfw `sine` TEST vocoder (not interoperable) -> proto TX framing -> cfw modulator (48 kHz) -> channel (`hard` = +1 kHz offset, +100 ppm, -10 % deviation) -> cfw demodulator -> proto RX -> vocoder -> PCM.
Cell: median over 0.25 s blocks of the recovered tone Hz / SINAD dB (60+ = numerically perfect sine) / % of blocks with 440+/-2 Hz and >= 20 dB / voice frames received of sent (concealed: FEC failed or >= 3 corrections). Codec alone: 439.86 Hz, 68 dB (the tone sits on the IMBE pitch grid: f0 = 16000/145.5, 4th harmonic).

| Mode | decoded call | clean 40 dB | 20 dB hard | 14 dB hard | 10 dB hard |
|---|---|---|---|---|---|
| DMR | TG91 < 9990201 TS1 | 439.9 Hz / 60+ dB / 100 % / 144 of 150 (0) | 439.9 Hz / 60+ dB / 100 % / 144 of 150 (0) | 439.9 Hz / 57 dB / 55 % / 144 of 150 (0) | 439.9 Hz / 58 dB / 77 % / 144 of 150 (28) |
| P25 | TG91 < 9990201 | 441.4 Hz / 27 dB / 100 % / 144 of 144 (0) | 441.4 Hz / 28 dB / 100 % / 144 of 144 (0) | 441.4 Hz / 26 dB / 100 % / 144 of 144 (0) | 441.4 Hz / 27 dB / 100 % / 144 of 144 (54) |
| P25P2 | TG91 < 9990201 S1 | 439.9 Hz / 60+ dB / 100 % / 180 of 180 (8) | 439.9 Hz / 60+ dB / 100 % / 172 of 180 (0) | 439.9 Hz / 60+ dB / 100 % / 176 of 180 (4) | 578.3 Hz / -42 dB / 9 % / 176 of 180 (4) |
| NXDN48 | TG91 < 28729 RAN1 | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (0) | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (0) | 439.9 Hz / 60+ dB / 100 % / 144 of 144 (0) | 500.5 Hz / -36 dB / 33 % / 144 of 144 (0) |
| DSTAR | N0CALL/CFW > CQCQCQ | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) | 439.9 Hz / 60+ dB / 100 % / 139 of 139 (0) |
| YSF | N0CALL > ALL | 439.9 Hz / 60+ dB / 100 % / 145 of 145 (0) | 439.9 Hz / 60+ dB / 100 % / 145 of 145 (0) | 439.9 Hz / 60 dB / 88 % / 145 of 145 (0) | 512.6 Hz / -40 dB / 33 % / 145 of 145 (0) |

### Modulators (cfw/dsp/dsp_mod.c): random symbols -> modulator -> channel -> cfw demodulator

| Mode | frequency pulse | peak dev Hz | 99% power BW kHz | BER 30 dB plain | BER 20 dB hard | BER 14 dB hard |
|---|---|---|---|---|---|---|
| DMR | RRC 0.2 | 3555 | 7.4 | 0.0e+00 | 2.7e-05 | 4.0e-04 |
| P25 | RC 0.2 x inverse sinc | 4170 | 8.0 | 0.0e+00 | 5.4e-05 | 2.4e-03 |
| YSF | RC 0.2 x inverse sinc | 6404 | 10.8 | 0.0e+00 | 0.0e+00 | 4.8e-04 |
| NXDN48 | RRC 0.2 | 1895 | 3.9 | 0.0e+00 | 0.0e+00 | 2.2e-04 |
| NXDN96 | RRC 0.2 | 4389 | 8.6 | 0.0e+00 | 0.0e+00 | 1.6e-04 |
| DSTAR | Gaussian BT 0.5 | 1200 | 4.9 | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| P25P2 | 1RC, CPM h = 1/4 | 4500 | 15.0 | 0.0e+00 | 0.0e+00 | 2.1e-04 |
