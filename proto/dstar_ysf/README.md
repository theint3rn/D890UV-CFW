# D-STAR + YSF protocol layers (`cfw/proto/dstar_ysf`)

Portable C11 libraries for the D890UV custom firmware. No hardware access, no `malloc`, all state in
caller-owned structs. Vocoder frames are opaque byte arrays; this code contains **no AMBE/IMBE codec**.

```
common/  dv_common.[ch]  bit pack/unpack, CRC-16 (X.25 and YSF), rate-1/2 conv encoder + Viterbi (K<=5)
         dv_modem.h      modem interface (cfw/hal has none yet): start_rx/tx, tx_write(bits|dibits), stop
dstar/   dstar.h         public API: header, routing helpers, D-PRS, RX + TX state machines
         dstar_header.c  41-byte header, P_FCS, FEC, interleave, scrambler, callsign/UR helpers, D-PRS CRC
         dstar_rx.c      sync search, header, voice frames, slow data, fast data, end/lost
         dstar_tx.c      preamble + header, frames with slow-data scheduler, fast data, end pattern
         dstar_gmsk.[ch] reference GMSK mod/demod on float samples (optional)
ysf/     ysf.h           public API: FICH, FEC blocks, CSD, RX + TX
         ysf_fec.c       FICH, Golay(24,12,8), PN(9,5) whitening, DCH units, V/D 2 voice protection
         ysf_frame.c     frame assembly, dibit <-> bits, C4FM level mapping, CSD packing
         ysf_rx.c        sync search + flywheel, payload split per FI/DT, callsign/data assembly
         ysf_tx.c        HC, CC (V/D 1, V/D 2, Voice FR incl. CC0, Data FR with blocks), TC
         ysf_c4fm.[ch]   C4FM dibit slicer (sync-trained gain/offset), dibit -> deviation
test/                    host tests
```

## Build / test

```bash
make test
```

```bash
make BUILD=build-arm CC=arm-none-eabi-gcc AR=arm-none-eabi-ar CFLAGS_EXTRA="-Os -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard" lib
```

Host: 222 checks, all pass (2026-11-19). Cortex-M33 `-Os`: 12.8 KB text, 29 B bss.
State: `dstar_rx_t` 968 B, `dstar_tx_t` 464 B, `ysf_rx_t` 864 B, `ysf_tx_t` 188 B.
Peak stack: about 3 KB in `ysf_rx_push_dibit` → `ysf_rx_frame` (960-bit frame + 360-bit unit + Viterbi
decision buffer 672 B); about 2 KB in `dstar_header_decode`. Run the RX from a task with ≥ 4 KB stack.

## Use

RX: the modem (hardware or the reference software modem) delivers bits / dibits, the glue code pushes them in,
callbacks give opaque vocoder frames + events.

```c
static void on_voice(void *u, const uint8_t ambe[9], uint8_t seq) { vocoder_decode_dstar(ambe); }
static void on_event(void *u, const dstar_event_t *ev) { if (ev->type == DSTAR_EV_HEADER) ui_show(&ev->u.header.hdr); }
static const dstar_rx_cb_t cb = { on_voice, on_event };
dstar_rx_t rx; dstar_rx_init(&rx, &cb, NULL);
dstar_rx_push_bits(&rx, bits, n);          /* one bit per byte, air order */

ysf_rx_t yrx; ysf_rx_init(&yrx, &ycb, NULL);
ysf_rx_push_dibit(&yrx, ysf_c4fm_slice(&slicer, soft_symbol, NULL));
```

TX: build bit/dibit buffers and hand them to `dv_modem_ops_t.tx_write`.

```c
dstar_header_t h; dstar_header_repeater(&h, "W1ABC", "D890", "CQCQCQ", "W1XYZ  B", true);
dstar_tx_t tx; dstar_tx_init(&tx, &h); dstar_tx_set_text(&tx, "hello");
n = dstar_tx_header(&tx, 64, buf);  /* then per 20 ms: */  dstar_tx_frame(&tx, ambe, buf96);  /* last: */ dstar_tx_end(&tx, buf48);

ysf_csd_t csd; ysf_csd_init(&csd, "W1ABC", NULL);
ysf_tx_t ytx; ysf_tx_init(&ytx, YSF_DT_VD2, &csd, /*DG-ID*/ 0);
ysf_tx_header(&ytx, dib);  /* per 100 ms: */ ysf_tx_voice(&ytx, five_frames, dib);  /* last: */ ysf_tx_terminator(&ytx, dib);
```

## Coverage

### D-STAR

| Item | Status | Notes |
|---|---|---|
| Bit sync / frame sync | done | 64+ bits `10…`, 15-bit `111011001010000`; RX matches last 16 preamble bits + sync, ≤ 2 errors |
| Radio header layout | done | flags, RPT2, RPT1, UR, MY, MY2, P_FCS (low byte first); flag1 bit constants |
| Header CRC | done | CRC-CCITT reflected (0x8408), init FFFF, inverted = CRC-16/X.25; tested vs check value 0x906E, bitwise reference, FCS residue 0x0F47 |
| Header FEC | done | K=3, G1=1+D+D², G2=1+D², 2 tail bits, 330 → 660; hard-decision Viterbi, corrects spread errors |
| Interleave | done | JARL Ap2.2 matrix (24 rows; rows 0–11 × 28, rows 12–23 × 27); tested against table entries |
| Scrambler | done | x⁷+x⁴+1, all-ones seed; header 660 bits; tested: first 24 bits = slow-data pattern 70 4F 93 |
| Puncturing | n/a | the D-STAR header code is plain rate 1/2 (spec has no puncturing) |
| Voice frames | done | 72-bit AMBE opaque (9 bytes), 24-bit data |
| Data sync / superframe | done | `101010101011010001101000` in the data part of every 21st frame; late entry; 3 missed syncs → LOST; bit-slip realign |
| End pattern | done | 32 × `10` + `000100110101111` + `0` (48 bits) |
| Slow data | done | 2-frame blocks, mini header; text 0x40–0x43 (20 chars), simple data 0x3N (GPS/NMEA, D-PRS, user) with line assembly, header resend 0x5N (9 blocks), code squelch 0xC2, filler 0x66 |
| TX slow-data scheduler | done | per superframe: CSQ, text (first + every 4th), header resend (odd superframes), then fast data / GPS ring / filler |
| D-PRS | done | `$$CRCxxxx,` + APRS TNC2 line + CR, CRC-CCITT X.25 over the APRS part; build + check |
| DV fast data | done (layout unverified) | JARL ch.7: mini header 0x8N/0x9N, 28-byte block with the sync frame, 20-byte blocks after, 4+0x02+4 bytes per voice frame, guard byte. See "Unverified" |
| Routing (UR/RPT1/RPT2/MY) | done | simplex/repeater/gateway headers, CQ, callsign route, `/RPT` zone CQ, `REF001CL` link, unlink `U`, info `I`, echo `E`, classifier, repeater turnaround |
| GMSK reference modem | done | Gaussian FIR (BT 0.5) frequency output; demod: DC track, 1-bit moving average, zero-crossing clock loop; tested with noise, DC, 0.2 % clock error, inverted polarity, full stream into RX |

### YSF

| Item | Status | Notes |
|---|---|---|
| Frame sync | done | `D471C9634D`, search ≤ 3 bit errors, flywheel ≤ 8, out of sync after 4 bad syncs (spec N2) |
| FICH | done | 32 bits + CRC16 → 4 × Golay(24,12,8) → +4 tail → K=5 trellis → 5×20 dibit interleave; Golay generator from Appendix A, min distance 8 verified; corrects ≤ 3 per word |
| DCH units | done | PN(9,5) whitening (seed 1 1100 1001, per unit) → CRC16 → trellis → 9×20 (360) / 5×20 (200) dibit interleave; whitening checked against the widely published 20-byte table |
| V/D mode 1 | done | DCH 72 + VCH 72 per 20 ms, rolling CSD1/CSD2/CSD3/DT1–5 by FN/FT (Table 4-3) |
| V/D mode 2 | done | DCH 40 + VCH 72 + VeCH 32; 49-bit voice: 27 bits × 3 + 22 + 0, whitening, 26×4 interleave, majority vote; rolling Dest/Src/Down/Up/Rem1+2/Rem3+4/DT1–2 (Table 4-4) |
| Voice FR (VW) | done | 5 × 144-bit full-rate frames; CC0 sub header (FT=1 FN=0): CSD3 + 72 reserved + VCH 3, 4 |
| Data FR | done | DCH-1 + DCH-2 per frame, units CSD1, CSD2, CSD3, DT1–13 (Table 4-5), BN/BT up to 4 × 260 bytes |
| HC / TC | done | DCH-1 = CSD1 (dest+src), DCH-2 = CSD2 (down+up) |
| CSD / callsigns | done | dest/src/down/up 10 chars, Rem1–4 5 bytes each (Rem3 = VoIP/Wires-X ID, Rem4 = radio ID) |
| DG-ID | done | 7-bit SC field of the FICH (`ysf_fich_dgid/set_dgid`) |
| Wires-X | fields only | VoIP bit, MR, Rem fields exposed; Wires-X command frames are Data FR payloads (`YSF_EV_DATA`) — no command parser |
| C4FM slicer | done | sync-correlation least-squares gain/offset fit, peak-tracker fallback; dibit 00 +1, 01 +3, 10 −1, 11 −3 |

## Unverified / limits

Cross-checked 2026-11-19 against MMDVMHost (GPL; facts only, values used as test vectors, no code copied):

* **Confirmed same as ours**: YSF sync; FICH byte layout (FI CS CM BN / BT FN FT / rsv Dev MR VoIP DT /
  SQL DG-ID), CRC16 (0x1021, init 0, inverted, high byte first), FICH Golay split and all 4096 Golay(24,12)
  codewords, K=5 trellis (G1 = d+d3+d4, G2 = d+d1+d2+d4), 5x20 / 9x20 dibit interleave, DCH order
  whitening -> CRC (CRC over the whitened bytes), DCH-1/DCH-2 split, V/D 2 voice (x3 + 22 + 0, whitening,
  26x4 interleave, VCH+VeCH contiguous), FN maps for V/D 1, V/D 2 and Data FR (DCH-1 fn1 = CSD3, DCH-2 fn1 =
  DT1, ...), Voice FR CC0 rule (FN=0, FT=1; DCH bits 0..359, VCH at 432 and 576), DG-ID = FICH byte 3 & 0x7F.
  D-STAR: sync bytes 55 2D 16, end pattern ...55 C8 7A, slow-data XOR 70 4F 93, mini header types
  (0x3X, 0x4X, 0x5X, 0x8X/0x9X fast, 0xCX squelch), flag1 bits, silence AMBE.
* **Closed**: D-STAR fast-data voice scrambling = 9 bytes 70 4F 93 40 64 74 6D 30 2B, which is our x^7+x^4+1
  sequence restarted per 72-bit voice frame (now a test).
* **Fixed**: fast-data mini header valid range is 0x81..0x9C (max 28 bytes); RX accepted up to 0x9F.
* **Vocoder adapter facts** (frames stay opaque here): YSF V/D 1 VCH (72 bits) uses the same A/B/C bit tables as
  a DMR AMBE+2 72-bit frame, so it can go to a DMR-format AMBE+2 vocoder as is. YSF Voice FR VCH (144 bits)
  uses the same IMBE interleave as a P25 Phase 1 IMBE frame: decode with the P25 IMBE adapter.
  Our 18-byte VW frame (MSB first, air order, still interleaved) goes straight into
  `p25_imbe_decode(air[18], frame[11])` / out of `p25_imbe_encode` (`cfw/proto/p25`, `include/p25_p1.h`,
  needs `src/p25_fec.c` + `src/p25_p1.c`); confirmed by the P25 session.
* **V/D 2 to DMR vocoder**: `cfw/proto/dmr_nxdn/common/ambe_fec.[ch]` `ambe72_encode(d49[7], out72[9])` /
  `ambe72_decode()` converts the 49-bit payload to a 72-bit AMBE+2 frame, with the 49 bits laid out
  u0(12) u1(12) u2(11) u3(14), MSB first. Whether YSF V/D 2 sends its 49 bits in that order is **not verified**
  (MMDVMHost never converts V/D 2), so check it with a capture or a known V/D 2 test frame first.
* **Still open**: D-STAR code squelch byte format (0xC2 bytes passed raw); fast-data guard byte value and the
  "mitigation" 0x02 byte are from JARL only; nothing tested over the air yet (no capture). Viterbi is hard
  decision (soft input supported).
* D-STAR header FEC/interleave is done in the MMDVM modem firmware, not MMDVMHost, so it was not cross-checked;
  it follows the JARL Ap2.2 table, which our test checks entry by entry.

## Sources

* JARL, "Digitalization Technology Standard for Amateur Radio" (STD6.0, English):
  <https://www.jarl.org/English/7_dstar/STD6_0_E.pdf> — header, CRC, FEC, interleave matrix, scrambler,
  sync/end patterns, slow data mini headers, D-PRS, fast data, GMSK polarity.
* JARL "D-STAR System" (shogen, English): <https://www.qsl.net/kb9mwr/projects/dv/dstar/shogen.pdf>.
* Yaesu, "Amateur Radio Digital Standards" rev 1.02 (2015): <https://www.rcqsl.com/manuals/78.pdf> — frame
  structures, FICH, CSD, DCH, V/D 2 voice protection, Golay matrix, trellis, C4FM mapping.
* Yaesu FT5D updaters (MAIN 1.21, SUB 1.10): firmware images are encrypted (64-bit block cipher, ECB), so no
  comparison was possible — see [docs/FT5D_FIRMWARE.md](docs/FT5D_FIRMWARE.md).
* gr-ysf notes (<https://hb9uf.github.io/gr-ysf>) for context on VW mode only.
* MMDVMHost (<https://github.com/g4klx/MMDVMHost>, GPL) was read on 2026-11-19 to cross-check facts
  (constants, tables, layouts). No code was copied; table values appear only as test vectors in `test/`, and
  our code generates them. ircDDBGateway / DSD were not used.
