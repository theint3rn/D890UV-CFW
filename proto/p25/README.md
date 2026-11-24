# P25 protocol library (cfw/proto/p25)

Portable C11 implementation of the APCO Project 25 air-interface protocol layer
for the D890UV custom firmware: Phase 1 (FDMA/C4FM), Phase 2 (two-slot TDMA)
and a trunking follower. It does not depend on any hardware, uses no malloc and
no static/global state. Every object lives in a caller-owned struct.

```
make test     # host build (gcc) + 1075 checks: FEC, DSP, Phase 1, Phase 2, trunking, encryption
make arm      # cross-compile for Cortex-M33 (arm-none-eabi-gcc -Os) + size report
make lib      # build/libp25.a for the host
```

Status 2026-11-19: `make test` passes 1075/1075 with no warnings under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

## Footprint (Cortex-M33, -Os)

| | |
|---|---|
| Code | 22.5 KB `.text` (3.4 KB of it encryption), 0 `.data`, 0 `.bss` |
| `p25_p1_rx_t` | 1216 B (one LDU of dibits + PDU reassembly, 256 user octets) |
| `p25_p2_rx_t` + `p25_p2_scrambler_t` | 272 + 541 B |
| `p25_trunk_t` | 648 B (16 iden entries, 32 talkgroups, 8 CCs, 8 adjacent sites) |
| `p25_c4fm_mod_t` / `_demod_t` | 676 / 272 B (optional software modem) |
| `p25_crypt_t` / `p25_keyring_t` | 474 B (one superframe of keystream) / 610 B (16 keys) |
| Worst stack path | about 1.1 KB (`p25_p2_rx_slot` → RS decode → Berlekamp-Massey) |

The largest table is the 128-byte GF(64) log/exp pair. The Golay decoder does a
syndrome search instead of using a 2048-entry table. The BCH and RS decoders use
Berlekamp-Massey with Chien search and Forney.

## Layout

| File | Contents |
|---|---|
| `include/p25_fec.h`, `src/p25_fec.c` | Golay (23,12)/(24,12)/(18,6), Hamming (15,11)/(10,6), cyclic (16,8), BCH (63,16) NID, GF(64) RS encode/decode with erasures, 1/2 and 3/4 rate trellis (Viterbi), CRC-16/9/12/32, IMBE/AMBE PN generator |
| `include/p25_p1.h`, `src/p25_p1.c` | Phase 1: frame sync, NID, status symbols, HDU, LDU1/LDU2 (IMBE extraction and insertion, LC, ES, LSD), TDU, TDULC, TSBK, PDU, streaming receiver, voice TX helper |
| `include/p25_tsbk.h`, `src/p25_tsbk.c` | OSP parsing (grants, updates, IDEN_UP/VU/TDMA, RFSS/NET/ADJ status, affiliation and registration responses, deny), channel-to-frequency math, ISP builders (U_REG_REQ, U_DE_REG_REQ, GRP_AFF_REQ, GRP_V_REQ, UU_V_REQ) |
| `include/p25_p2.h`, `src/p25_p2.c` | Phase 2: I-ISCH/S-ISCH, DUID, scrambling sequence, 4V/2V bursts with AMBE+2 half-rate FEC, ESS (RS 44,16), FACCH/SACCH MAC PDUs (RS 63,35 shortened and punctured, CRC-12), MAC_PTT/END_PTT/IDLE/ACTIVE/HANGTIME, MAC message iteration, slot builders, superframe receiver |
| `include/p25_trunk.h`, `src/p25_trunk.c` | Trunking state machine: CC hunt, site tracking, grant following with talkgroup priority and lockout, preemption, Phase 1 and Phase 2 voice channels, hang and grant timeouts |
| `include/p25_c4fm.h`, `src/p25_c4fm.c` | Symbol helpers: dibit↔level, RC (α 0.2) modulator, discriminator demodulator (kurtosis-based timing, adaptive DC and level slicer) |
| `include/p25_crypt.h`, `src/p25_crypt.c` | Voice encryption with the user's keys: AES-256 (0x84) and DES-OFB (0x81) per TIA-102.AAAD, ADP/ARC4 (0xAA); key ring by ALGID+KID, MI LFSR, keystream layout for Phase 1 IMBE and Phase 2 AMBE frames |
| `include/p25_modem.h` | HAL modem interface (`cfw/hal` has none yet; mirrors `dstar_ysf/common/dv_modem.h`) |
| `tests/` | host tests (`tst.h` harness) |

## Interface

Data units:
- **Dibits**: one symbol 0..3 per byte, first bit in bit 1. This is the stream
  to and from the modem, with status symbols included for Phase 1.
- **IMBE frame**: 88 bits in 11 bytes, MSB first, in u-vector order
  u0..u3 (12 bits each), u4..u6 (11 bits each), u7 (7 bits).
- **AMBE+2 half-rate frame**: 49 bits in 7 bytes, MSB first, ordered
  u0 (12), u1 (12), u2 (11), u3 (14).

`p25_imbe_encode()` / `p25_imbe_decode()` work on 144 interleaved air bits
(18 bytes, MSB first). YSF Voice FR (VW) frames use the same IMBE FEC and
interleave, so the YSF library can use these functions too (per the
dstar_ysf session, checked against MMDVMHost).

Vocoder frames are opaque. Only the FEC, PN masking and interleaving around them
are implemented here, with no codec internals. A DVSI-style vocoder
(the SCT3288 may be one, see `cfw/docs/FEASIBILITY.md`) may want another bit
order or may do the FEC itself. The glue code converts.

Glue sketch (RX):

```c
static p25_p1_rx_t rx1; static p25_p2_rx_t rx2; static p25_trunk_t tr;
static void on_imbe(void *c, const uint8_t f[11], int errs) { vocoder_imbe(f, errs); p25_trunk_voice_activity(&tr); }
static void on_p1(void *c, const p25_p1_event_t *ev)        { p25_trunk_p1_event(&tr, ev); ui_p1(ev); }
static void on_ambe(void *c, unsigned ch, const uint8_t f[7], int e) { vocoder_ambe(f, e); p25_trunk_voice_activity(&tr); }
static void on_p2(void *c, const p25_p2_event_t *ev)        { p25_trunk_p2_event(&tr, ev); }
static void tune(void *c, uint32_t rx, uint32_t tx, p25_mode_t m, uint8_t slot) {
    ops->set_freq(ctx, rx, tx);
    ops->start_rx(ctx, m == P25_MODE_P2 ? P25_MOD_HDQPSK_6000 : P25_MOD_C4FM_4800);
    rx2.channel_filter = (int8_t)slot;          /* P2: follow one logical channel */
}
/* modem task: */ p25_p1_rx_push(&rx1, dibits, n, rssi);   /* or p25_p2_rx_push(&rx2, ...) */
/* 10 ms tick:  */ p25_trunk_tick(&tr, 10);
```

For Phase 2, set `p25_p2_scrambler_init(&scr, tr.wacn, tr.sysid, tr.nac)` and
`p25_p2_rx_set_scrambler()` once the site is known.

TX (conventional, licensed amateur use only): `p25_p1_voice_tx_start()` (HDU),
`p25_p1_voice_tx_push()` per 20 ms IMBE frame (returns an LDU1 or LDU2 every 9
frames), `p25_p1_voice_tx_end()` (TDU). Each writes air dibits for
`ops->tx_write()`. Trunked requests are TSBKs built with `p25_isp_*()` +
`p25_p1_build_tsbk()`, sent on `p25_trunk_cc_tx_hz()`.

## Encryption (user keys)

```c
static p25_keyring_t keys; static p25_crypt_t ks_rx;
p25_keyring_init(&keys);
p25_keyring_add(&keys, P25_ALG_AES256, 0x0001, key32, 32);   /* from the user's own key table */
p25_p1_rx_set_crypt(&rx1, &keys, &ks_rx);                     /* Phase 1 */
rx2.channel_filter = slot; p25_p2_rx_set_crypt(&rx2, &keys, &ks_rx);   /* Phase 2 */
```

- RX: the MI comes from the HDU (Phase 2: MAC_PTT) for the first superframe and
  from each LDU2 ES (Phase 2: ESS in the 2V burst) for the next. Frames are
  delivered decrypted. If the key is missing or the algorithm is unknown,
  frames are **muted**, not passed on as noise. Events carry `crypt`
  (`P25_VC_CLEAR / DECRYPTED / NOKEY / WAIT_MI`).
- Late entry: the first LDU1 joined mid-call plays undecrypted, because the LC
  says "encrypted" only at its end. The call is then muted until the next
  LDU2 ES gives the MI.
- TX (Phase 1): `p25_p1_voice_tx_start_enc()` takes ALGID/KID and a fresh MI.
  It sets the LC encrypted service bit, sends ALGID/KID/MI in the HDU and the
  next MI in each LDU2 ES. Phase 2 TX: apply `p25_crypt_ambe()` to the frames
  before `p25_p2_build_4v/2v()` (the test shows how).
- **Legal:** encrypting to obscure meaning is not allowed on amateur
  frequencies in most countries (US 47 CFR 97.113(a)(4)). Use TX encryption
  only on systems where you are authorised. Nothing here recovers, guesses or
  brute-forces keys.
- Keystream layout (TIA-102.AAAD). DES/AES are OFB: DES IV = MI (64 bits); AES
  IV = MI ‖ LFSR⁶⁴(MI); the first block is discarded. ADP is ARC4 with key =
  key(5) ‖ MI(8). Phase 1 uses 11 bytes per IMBE frame, with 101 bytes per LDU
  (LC/ES 9 + 2 reserved bytes, LSD 2 before frame 9); ADP voice starts at byte
  267. Phase 2 uses 7 bytes per AMBE frame (4V bursts 0..3, then 2V).
  MI LFSR: x⁶⁴+x⁶²+x⁴⁶+x³⁸+x²⁷+x¹⁵+1.
- Verified: AES against FIPS-197 C.3 and openssl AES-256-OFB, DES against the
  classic vector and openssl DES-OFB, ARC4 against an independent Python
  implementation, keystream offsets, and encrypted P1 (AES, DES, ADP) and P2
  (AES) calls end to end, including wrong-key, no-key and late-entry cases.
  **Not verified on air:** the layout was cross-checked with OP25's description
  (OP25 marks its Phase 2 DES/AES as experimental). The bit order of the 88-bit
  IMBE and 49-bit AMBE frames that the keystream is XORed onto is assumed to be
  the u-vector packing used here. Check against a real encrypted capture with a
  known key before relying on interop. Encryption of LSD is not done.

## What is implemented (with TIA-102 references)

| Feature | Reference | Notes |
|---|---|---|
| Frame sync 0x5575F5FF77FF, inverted-polarity detection, ≤4 bit errors | BAAA-B §6.2 | resync on a clean sync mid-frame |
| NID: BCH(63,16,23) + parity bit, NAC filter | BAAA-B §6.2, §7.5 | up to 11 bit errors corrected (BM + Chien) |
| Status symbols every 35 data dibits | BAAA-B §6.3 | inserted on TX (configurable value), reported on RX |
| HDU: RS(36,20,17) + Golay(18,6,8), MI/MFID/ALGID/KID/TGID | BAAA-B §5.2, §7.3–7.4 | Golay failures become RS erasures |
| LDU1/LDU2: 9 IMBE frames, LC RS(24,12,13), ES RS(24,16,9), Hamming(10,6,3), LSD cyclic(16,8,5) | BAAA-B §5.3–5.4 | voice decoded as each frame completes (low latency) |
| IMBE FEC: Golay(23,12) ×4, Hamming(15,11) ×3, PN masking, bit interleave | BABA §7 | |
| TDU, TDULC (LC RS(24,12,13) + Golay(24,12)) | BAAA-B §5.5–5.6 | |
| Link control: group/unit voice user, call termination, group voice channel update | AABF-D | |
| TSBK: 1/2-rate trellis, data interleave, CRC-CCITT, 1–3 blocks | BAAA-B §5.7, §7.2 | Viterbi decoder |
| PDU: header, unconfirmed (1/2 rate) and confirmed (3/4 rate, CRC-9) data blocks, CRC-32 | BAAA-B §5.4 (data) | "basics": no ACK/retry, no SNDCP |
| C4FM symbol helpers: levels, RC modulator, discriminator demod/slicer | BAAA-B §8 | inverse-sinc TX shaping left to the FM path |
| Phase 2 superframe/timeslot model, I-ISCH/S-ISCH (40-bit), DUID (8,4) | BBAC §4–5 | slot id from I-ISCH, S-ISCH sync |
| 4V/2V bursts, AMBE+2 half-rate FEC (Golay 24/23 + PN) and interleave | BBAC §5, BABA-1 | |
| ESS: ESS-B in 4V, ESS-A in 2V, RS(44,16,29) | BBAC §5.4 | |
| Scrambling sequence from WACN/SysID/NAC (44-bit LFSR, 4320 bits) | BBAC §7.2.5 | |
| FACCH RS(45,26,20), SACCH RS(52,30,23) (RS(63,35) shortened and punctured), CRC-12 | BBAC §7 | |
| MAC PDUs: PTT, END_PTT, IDLE, ACTIVE, HANGTIME, SIGNAL; message length table | BBAD | abbreviated 0x40\|op messages mapped onto the TSBK layouts |
| OSP: GRP_V_CH_GRANT(_UPDT/_UPDT_EXP), UU grants, IDEN_UP / IDEN_UP_VU / IDEN_UP_TDMA, RFSS/NET/ADJ_STS_BCST, GRP_AFF_RSP, U_REG_RSP, DENY_RSP | AABC-E | |
| Trunking follower: CC hunt list, quiet-CC detection, WACN/SysID filter, adjacent sites, priority/scan list with lockout, preemption via grant updates (CC, LDU1 LC 0x02, Phase 2 MAC), Phase 2 grants on a Phase 1 CC | AABD | |
| ISP builders: U_REG_REQ, U_DE_REG_REQ, GRP_AFF_REQ, GRP_V_REQ, UU_V_REQ | AABC-E | messages only, sent by the application |

## How it was verified

The tests have no RF recordings. Everything below is either a published table or
a self-consistency check.

- **Generator matrices / tables cross-checked against published values.** These
  are the values OP25 (boatbod/op25) publishes and they match the TIA
  descriptions: Golay(24,12) rows, Hamming(10,6) parity table, all 16 BCH NID
  generator rows (incl. the 64th bit), the IMBE interleave (raw LDU bit
  positions of voice frames 0 and 4), the LC hex-word and LSD raw bit positions,
  the PN start value (`pr_n[0]`), all 128 I-ISCH code words, the complete
  256-entry DUID decode table, and scrambler sequences for two WACN/SysID/NAC
  sets (computed with OP25's `lfsr.py` algorithm in Python, then checked
  against `p25_p2_scrambler_init()`). CRC-16 and CRC-32 use catalogue check
  values.
- **Second source: MMDVMHost** (g4klx/MMDVMHost, GPLv2+), used to check facts
  only. These agree with this library: 1/2- and 3/4-rate trellis state tables,
  constellation-to-dibit map and data interleave, NID parity bit per DUID
  (1 for LDU1/LDU2, else 0), LSD parity (spot-checked against its CCS_PARITY
  table) and LSD position (raw bits 1546..1577), HDU/TSDU/LC raw bit ranges,
  status symbol positions (70/71 + 72k), and CRC-CCITT (init 0, inverted).
- **Error-correction tests**: random errors up to the design limit for every
  code (Golay ≤3, BCH ≤11, RS errors + erasures, punctured RS, trellis).
- **End-to-end loops**: Phase 1 stream (noise, HDU, LDU1, LDU2, LDU1, TDU, a
  wrong-NAC LDU, a 2-block TSBK, TDULC, unconfirmed PDU, confirmed PDU) fed in
  random chunk sizes, clean, inverted and with 60 random symbol errors. Phase 2:
  3 scrambled superframes (voice on channel 0 with ESS, PTT/END_PTT on SACCH,
  idle FACCH/SACCH with grant update and NET_STS on channel 1), clean and with
  errors. C4FM: modulator → gain/DC/noise/timing offset → demodulator, 0–3
  symbol errors after settling.
- **Trunking**: simulated site with TSBKs going through the real Phase 1
  framer. Covers hunt dwell, iden tables, site/adjacent tracking, locked-out and
  unlisted grants, P1 grant, LDU1 channel-update preemption to a Phase 2 TDMA
  channel/slot, P2 PTT/END_PTT on the right slot only, grant-wait and hang
  timeouts, TDU return, CC loss and the WACN filter.

### Not verified / open points (check against a real capture before relying on them)
- **CRC-9** (confirmed PDU blocks) is from the standard as I remember it and is
  not cross-checked; neither OP25 nor MMDVMHost has it. The 3/4-rate trellis
  table now matches MMDVMHost.
- **TDULC Golay pairing** (two hexbits per Golay(24,12) word, in order) and
  **PDU header bit layout** are from the spec description and not checked
  against a capture.
- **ISP field layouts** (reserved bits in U_REG_REQ / GRP_AFF_REQ / GRP_V_REQ)
  have not been checked against a real system.
- **IDEN_UP_TDMA channel types**: the table of slots per carrier and bandwidth
  covers types 0–5 only.
- **Phase 2 FACCH**: dibits 89..109 of the slot are not part of the FACCH
  code word. TX writes zeros there and RX ignores them.
- **Phase 2 is outbound only.** The SU inbound burst structure (H-CPM
  ramp/guard), LCCH (Phase 2 control channel) and therefore Phase 2 control
  channel hunting are not implemented. The trunking control channel must be
  Phase 1. Phase 2 *voice* channels granted from it are supported.
- PDU: no confirmed-delivery protocol (ACK/NAK/retry) and no SNDCP/IP.
- Next step: run a recorded dibit stream from a real system (for example an OP25
  capture) through `p25_p1_rx_push()` / `p25_p2_rx_push()`.

## Hardware path

`cfw/docs/FEASIBILITY.md` says the D890UV has no known path yet to get
discriminator samples in or modulator baseband out for 4FSK. This library does
not depend on that. It works as soon as a dibit (or sample) stream exists.

## Licence and sources

The code is original and written from the TIA-102 descriptions. **No OP25,
MMDVMHost, SDRTrunk or DSD code was copied.** OP25 (GPLv3) and MMDVMHost
(GPLv2+) were used only as references to cross-check numeric tables. Some of those tables are factual protocol data
defined by TIA-102 and appear verbatim in this library:
- the Golay, Hamming and cyclic generator rows (`p25_fec.c`)
- the MAC message length table (`mac_len` in `p25_p2.c`, TIA-102.BBAD)
- the AMBE half-rate interleave map (`p25_p2.c`)
- the ISCH generator constants, derived from the 128 code words

The test vectors in `tests/` (ISCH code words, DUID table, BCH rows, raw bit
positions) are the published OP25 tables. If a stricter licence position is
needed, regenerate those tables from the TIA documents.

Encryption works only with keys the user loads. There is no key recovery or
encryption breaking. AES, DES and ARC4 are written from FIPS-197, FIPS 46-3
and the public ARC4 description; the P25 keystream layout follows TIA-102.AAAD
(cross-checked against OP25's description, no code copied). TX encryption must
not be used on amateur frequencies.
