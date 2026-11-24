### BER vs C/N (12.5 kHz), discriminator demod at 24 kHz

Impairments in the `hard` rows: +1 kHz frequency offset, +100 ppm symbol clock, -10 % deviation. Cell = bit error rate (after the first 800 symbols = acquisition) / voice frames decoded by the cfw proto glue, in % of the frames decoded from the clean symbols (above 100 % = a duplicate late-entry frame).

| Mode | channel | 6 dB | 8 dB | 10 dB | 12 dB | 14 dB | 16 dB | 20 dB | 30 dB |
|---|---|---|---|---|---|---|---|---|---|
| DMR | plain | 3.3e-02 / 100% | 1.0e-02 / 100% | 1.8e-03 / 100% | 5.5e-04 / 100% | 6.9e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| DMR | hard | 5.1e-02 / 80% | 1.7e-02 / 100% | 3.4e-03 / 100% | 8.9e-04 / 100% | 2.4e-04 / 100% | 3.4e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25 | plain | 6.6e-02 / 91% | 2.5e-02 / 91% | 1.0e-02 / 100% | 2.6e-03 / 100% | 7.5e-04 / 100% | 5.1e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25 | hard | 8.4e-02 / 58% | 3.5e-02 / 75% | 2.0e-02 / 100% | 5.1e-03 / 100% | 3.8e-03 / 100% | 1.7e-04 / 100% | 6.9e-05 / 100% | 0.0e+00 / 100% |
| YSF | plain | 2.6e-02 / 100% | 7.5e-03 / 100% | 1.4e-03 / 100% | 3.5e-04 / 100% | 3.6e-05 / 100% | 2.4e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| YSF | hard | 3.7e-02 / 100% | 9.8e-03 / 100% | 2.9e-03 / 100% | 4.1e-04 / 100% | 6.0e-05 / 100% | 1.2e-05 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| NXDN48 | plain | 1.7e-02 / 92% | 1.4e-03 / 100% | 1.1e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| NXDN48 | hard | 2.1e-02 / 92% | 2.4e-03 / 100% | 3.2e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| NXDN96 | plain | 2.4e-02 | 4.4e-03 | 8.4e-04 | 1.1e-03 | 0.0e+00 | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| NXDN96 | hard | 2.6e-02 | 6.4e-03 | 7.9e-04 | 5.3e-05 | 0.0e+00 | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| DSTAR | plain | 3.2e-02 / 100% | 9.5e-03 / 100% | 1.1e-03 / 100% | 1.8e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| DSTAR | hard | 5.0e-02 / 99% | 1.5e-02 / 100% | 2.9e-03 / 100% | 4.2e-04 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% | 0.0e+00 / 100% |
| P25P2 | plain | 9.4e-02 / 90% | 5.5e-02 / 94% | 3.3e-02 / 100% | 2.2e-02 / 98% | 1.7e-02 / 100% | 1.3e-02 / 100% | 9.7e-03 / 100% | 6.0e-03 / 100% |
| P25P2 | hard | 9.4e-02 / 84% | 5.6e-02 / 94% | 3.4e-02 / 97% | 2.3e-02 / 97% | 1.7e-02 / 100% | 1.3e-02 / 100% | 1.0e-02 / 98% | 7.2e-03 / 100% |

### Audio path with 750 us de-emphasis (DMR, 20 dB, plain channel)

| receiver | BER |
|---|---|
| no compensation | 3.4e-01 |
| inverse de-emphasis 750 us | 0.0e+00 |

### P25 Phase 2 H-DQPSK: discriminator integrate-and-dump vs coherent differential detector

Coherent reference: matched RRC filter on the complex signal, genie timing, ideal AFC (best case).

| C/N | discriminator BER | coherent BER |
|---|---|---|
| 6 dB | 9.5e-02 | 1.6e-02 |
| 8 dB | 5.5e-02 | 3.5e-03 |
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
