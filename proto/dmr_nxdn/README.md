# DMR + NXDN protocol layers (cfw/proto/dmr_nxdn)

Portable C11 libraries for the D890UV custom firmware. They contain no hardware access, no
`malloc` and no static RAM (every context is a struct the caller owns). No speech codec is in this
folder; the only vocoder-related code is the channel FEC of the AMBE+2 3600x2450 frame
(49 <-> 72 bits, `common/ambe_fec.c`), needed for NXDN encryption.

```
common/   bits.h, fec.[ch] (Hamming, Golay, QR, RS(12,9), CRCs), bptc.c, trellis.c (DMR 3/4), conv.c (NXDN K=5)
          cipher.[ch] (AES-128/192/256, DES, RC4; encrypt direction only)
          ambe_fec.[ch] (AMBE+2 3600x2450 frame FEC: Golay(24,12)/(23,12), PN whitening, interleave)
dmr/      dmr.h  dmr_burst.c dmr_lc.c dmr_rx.c dmr_tx.c        layers 1-3, Tier II
          dmr_t3.h dmr_t3.c                                    Tier III MS procedures
          dmr_priv.h dmr_priv.c                                Tier III authentication + cfw privacy (all tiers)
          dmr_link.c                                           link modes (direct/TDMA direct/repeater/BS), talkspurt TX, RC
nxdn/     nxdn.h nxdn_ch.c nxdn_msg.c nxdn_rx.c nxdn_tx.c      CAI layers 1-3 (4800/9600, EHR/EFR, Type-D RTCH2)
          nxdn_trunk.c                                         Type-C subscriber trunking
          nxdn_crypt.h nxdn_crypt.c                            TS 1-D scramble / DES / AES encryption
tests/    host tests (make test)
```

```bash
make test            # host build with -Wall -Wextra -Wpedantic -Werror, runs every test
make arm-size        # Cortex-M33 -Os build, prints code size
./build/run_tests 1234   # same tests with another random seed
```

Status (2026-11-19): `make test` passes (16448 checks, also checked with 150 other seeds and
with `-fsanitize=undefined`). Cortex-M33 `-Os`: about 28 KB code (7.7 KB of it encryption + AMBE FEC),
0 bytes data/bss. Contexts: `dmr_rx_t` 3.0 KB (2 x 768 B data reassembly, `DMR_MAX_DATA_BLOCKS`,
plus one AES context per slot), `dmr_t3_t` 280 B, `nxdn_rx_t` 1.4 KB (voice + data cipher state),
`nxt_t` 224 B. Deepest stack: about 2.3 KB (NXDN frame ->
FACCH1 -> Viterbi; DMR rate-3/4 block -> trellis Viterbi about 1.8 KB). Run the receivers in a task with
4 KB of stack.

## Interface

There was no modem interface in `cfw/hal/hal.h` when this was written, so the libraries take and
produce **hard dibits** (0..3, first bit in bit 1; `01`=+3, `00`=+1, `10`=-1, `11`=-3, the same
mapping for DMR and NXDN). The suggested HAL shape is in the hand-over note
(`cfw-dmr-nxdn.md` in the shared notes folder):

```c
/* RX: one call per symbol (or per block), from the demodulator task */
dmr_rx_dibit(&dmr, d);            nxdn_rx_dibit(&nxdn, d);
/* callbacks */
voice(user, slot, frame72[9], idx, crypt)  /* DMR: 72-bit AMBE+2 frame incl. its FEC, 3 per burst */
voice(user, frame, 72|144, pos, crypt) /* NXDN: VCH, EHR 72 bits (49 + FEC) / EFR 144; EHR comes decrypted */
event(user, &ev)                     /* call start/end with IDs, CSBK, data, short LC, ...   */
/* TX: builders return 132 dibits (+12 CACH) for DMR, 192-dibit frames for NXDN */
```

If a chip (for example the SCT3288) hands over aligned bursts instead of a symbol stream,
`dmr_rx_burst()` and `nxdn_rx_frame()` take one aligned unit.

The Tier III and NXDN Type-C state machines are driven by the receiver events plus a millisecond
tick, and talk to the platform through three callbacks: `tune(channel, slot, control?)`,
`tx_csbk` / `tx` (inbound signalling to send in the next inbound slot / frame) and `event`.
Channel number -> frequency is the platform's channel plan.

## Simplex / direct mode

| Link (`dmr_link_t`) | Syncs | Lead-in (12 dibits) | Use |
|---|---|---|---|
| `DMR_LINK_DIRECT` | MS voice/data | guard, not keyed | simplex / talkaround (TS 102 361-1 4.4.3, 5.1.4.3) |
| `DMR_LINK_TDMA_DIRECT` | TS1 / TS2 direct | guard | dual capacity direct mode (4.4.4, 5.1.4.6) |
| `DMR_LINK_REPEATER` | MS voice/data | guard | MS -> repeater input |
| `DMR_LINK_BS` | BS voice/data | CACH (AT, TC, short LC) | hotspot / repeater output |

`dmr_call_tx_start()` + `dmr_call_tx_next()` per 60 ms (+ `dmr_call_tx_end()`) give the slot to key:
PI header (privacy), `n_headers` voice LC headers, voice bursts A..F with embedded LC, terminator.
For DIRECT / TDMA_DIRECT / REPEATER the radio keys only its own slot (30 ms every 60 ms) and listens in
the other one. Receivers need no mode: every sync type is accepted, the CACH is decoded only when the
burst is BS-sourced (so a channel set to "repeater" still hears simplex/talkaround), MS-sourced syncs
give slot 0 (forward) and RC bursts slot 1 (backward).
Reverse channel: standalone RC burst (table E.12, `dmr_tx_rc_standalone()`, keyed at offset 42 of the
backward slot burst window) and outbound embedded RC (6.4.2, `dmr_rc_emb32()` with PI = 1); commands of
TS 102 361-2 table 7.27 (cease transmission command / request), CRC-7 with mask 0x7A; both arrive as
`DMR_EV_RC`. Not done: acting on RC (the platform decides to stop TX), RC timing control.

Colour code filter (`cfg.cc_filter` 0..15): headers / data / CSBK of another CC are dropped, and voice is
delivered only once the call's CC is known good: from the voice LC header, or from the EMB of bursts B..F
(checked before each burst is delivered). Burst A has no EMB: on late entry (or after an EMB said "other
CC") it is held and delivered together with burst B once B's EMB matches, else dropped. A burst with an
undecodable EMB keeps the last verdict. A call that never passed the filter gives no `VOICE_END`.

NXDN conventional simplex: RDCH, SU direction 0 (table 5.2-2), preamble first (`nxdn_tx_preamble()`),
`nxdn_lich_rdch_voice()/_data()`. RAN 0 = heard by every receiver; a SACCH with another RAN keeps the
voice muted (`ran_filter`).

## Coverage against the specifications

Legend: done = implemented and covered by a test, part = implemented with limits, - = not done.

### ETSI TS 102 361-1 V2.7.1 (DMR air interface)

| Clause | Item | State | Notes / test |
|---|---|---|---|
| 5.1, 9.1.1 | burst and sync patterns (BS/MS voice/data, RC, TDMA direct TS1/TS2) | done | table 9.2; test checks the symbol-complement rule and annex E positions |
| 5.1.2 | voice superframe A..F | done | `dmr_voice_tx_t`, RX superframe index |
| 6.1 | vocoder socket, 3 x 72 bit | done | E.5: VS(215) first; frames untouched |
| 6.2, 9.1.3 | slot type, Golay(20,8) | done | B.11, d_min = 8 checked, corrects 3 |
| 6.3 | CACH, TACT Hamming(7,4) | done | figure B.9 interleave |
| 7.1.4, 9.1.2 | EMB, QR(16,7,6) | done | B.12, d_min = 6, table E.6 positions |
| 7.1.x | embedded LC (variable BPTC, 5-bit CS), late entry | done | figure B.3; RX emits VOICE_START from embedded LC without header |
| 7.1.x | full LC header/terminator, RS(12,9) + masks | done | B.18 checked against g(x) by polynomial division; B.19 GF spot values |
| 7.1.x | short LC in CACH (BPTC Hamming(17,12), CRC-8) | done | figure B.6 |
| 7.2, 9.1.8 | CSBK, BPTC(196,96), CRC-CCITT + mask | done | annex D idle message reproduces figure D.1 bit for bit |
| 7.2 | MBC header/continuation | part | blocks are collected and reported; last-block CRC check best effort |
| 7.3, D.2 | idle message | done | |
| 8.2 | data header (C/U/response/proprietary/short) | done | fields parsed; unconfirmed builder |
| 8.2 | rate 1/2, 3/4 (trellis), rate 1 blocks; CRC-9; CRC-32 | done | reassembly with CRC-9 per confirmed block and message CRC-32 |
| 8.2 | confirmed delivery responses (C_RHEAD/SACK), retries, fragments over several headers | - | received headers are reported; no response generator yet |
| B.2.2 | single-burst BPTC, reverse channel CRC-7 | part | codec done; embedded RC reported as an event; standalone RC burst not built |
| B.2.4 | rate 3/4 trellis | done | Viterbi with symbol-level metric: 1 adjacent-level error always corrected, 2 errors 95 %, 3 errors 87 % (2000 random blocks) |
| B.3.9 | CRC-32 octet order | done | LS octet first; 3 real captured packets match |

### TS 102 361-2/-3 (services, packet data)

Group and unit-to-unit voice LC (FLCO 0/3) parsed and built; talker alias / GPS FLCOs (4-8) and any
MFID LC are delivered raw (`DMR_EV_VOICE_LC_EMB`). Short LC Nul/Act_Updt/C_SYS_Parms/P_SYS_Parms
decoded to opcode + 24 data bits. PI headers are reported raw; cfw privacy PI headers are used (see Encryption).

### TS 102 361-4 V1.12.1 (Tier III trunking), MS side

| Clause | Item | State |
|---|---|---|
| 6.1 | TSCC hunt over a logical channel list, camp on C_ALOHA / C_BCAST / C_SYS_Parms, SYScode filter | done |
| 6.1.x | TSCC loss -> hunt again; Ann_WD_TSCC adds/withdraws hunt channels; adjacent site event | done |
| 6.2 | random access: C_RAND, Aloha Mask/Service_Function, Backoff, NRand_Wait, retries | part (timing approximated in TDMA frames of 60 ms; the platform sends the CSBK in the next inbound TSCC slot) |
| 6.4 | registration via C_RAND(REGI, Service_Kind 14), C_ACKD/NACKD/WACKD/QACKD | done |
| 6.4.x | C_AHOY presence check -> C_ACKU(MS_Accepted) | done (authentication not implemented) |
| 6.x | TV_GRANT / BTV_GRANT / PV_GRANT(_DX): follow wanted talkgroups / own calls, late entry | done |
| 6.x | return to TSCC: P_CLEAR, end of call + hang timer, loss of payload channel | done |
| 7.1.1.1.2 | absolute-frequency grants (LPCN 0xFFF + CG_AP MBC) | part (grant event reported, no tune: needs CG_AP parse) |
| - | C_MOVE, UDT, short data, authentication, TSCCAS, call diversion, data grants | - |

Proprietary trunking (Motorola Capacity/Connect Plus, Hytera XPT, ...) is out of scope. Hooks:
every CSBK with FID != 0 reaches `T3_EV_MFID_CSBK` (and `DMR_EV_CSBK` before that), and FID != 0 full
LCs are delivered raw, so a separate module can add those systems without touching this code.

### NXDN TS 1-A v1.3 (common air interface), TS 1-E v1.1 (Type-D CAI)

| Clause | Item | State | Notes / test |
|---|---|---|---|
| 3.3 | symbol mapping, 4800 and 9600 bps | done | same 192-symbol frame at both rates |
| 4.4.3-4.4.5 | FSW CDF59, preamble, post field | done | FSW with flywheel (3 frames), re-acquire until a 2nd FSW confirms timing; the frame of a replaced unconfirmed FSW is still decoded (a near-FSW inside the header frame cost the VCALL / VCALL_IV on some RANs, e.g. 13) |
| 4.5.3, 5.2 | LICH 7 bit + parity, all RF/functional channel types | done | table 5.2-2 example |
| 4.6 | scrambler X^9+X^4+1 | done | first outputs derived from table 4.6-1 |
| 4.5.2.1 | SACCH (CRC-6, punct 12->10, 5x12) superframe 4 x 18 bit + single | done | |
| 4.5.2.2 | FACCH1 (CRC-12, punct 3/4, 9x16), steal flags | done | duplicate halves merged |
| 4.5.2.3 | UDCH / FACCH2 (CRC-15, 29x12) | done | data call reassembly - |
| 4.5.1.1 | CAC outbound (CRC-16, 25x12), single/dual message | done | |
| 4.5.1.2/3 | Long CAC / Short CAC inbound | done | builders for SU requests |
| 4.5.4 | puncturing order X1,X2,X3,(X4)... | done | checked against the spec example |
| 6.3 | SR: structure + RAN, RAN filter | done | |
| 6.4 | VCALL, VCALL_IV, TX_REL(_EX), IDLE, DISC; VCALL_REQ/RESP, VCALL_ASSGN(_DUP), SITE_INFO, CCH_INFO, REG_REQ/RESP, GRP_REG | part | fields used by the call/trunking logic; everything also delivered raw |
| 7 | VCH: EHR 72 / EFR 144 bits | done | opaque to the vocoder; FEC 49<->72 and 88<->144 in common/ambe_fec.c, bit exact vs 7.2.1 / 7.2.2 patterns |
| TS 1-E 4.5.1.1, 5.2, 6.3 | Type-D RTCH2: SCCH (CRC-7), INFO1-4 structure, VCH, FACCH1 | part | UDCH2/FACCH3 and the Type-D site/free-repeater logic (TS 1-F) not done |
| 5.6 | inbound collision control (E field) / random access timing | - | requests are sent in the next inbound frame, 3 tries |

### NXDN Type-C trunking (SU), TS 1-A 2.2 / TS 1-C

Hunt over control channels, camp on any valid outbound CAC with the right RAN, SITE_INFO kept,
REG_REQ then GRP_REG_REQ, VCALL_ASSGN(_DUP) for wanted groups / own unit / own request -> traffic
channel, return on DISC, hang timeout or loss; VCALL_REQ for PTT. Composite control channel,
roaming between systems, data calls, status, short data: not done.

## Test vectors and licences

All code here was written from the specifications; no code from DSD, MMDVMHost, OP25, sdrtrunk or
any other project was used or copied. Test vectors are taken from the spec text itself (annex D
idle message, interleave tables, generator matrices, GF table, FSW / preamble / LICH examples,
NXDN EHR test pattern) or are self-consistency checks (CRC catalogue values, minimum distances,
TX -> dibit stream -> RX round trips with noise and injected errors). No over-the-air captures
were used.

Specs used: ETSI TS 102 361-1 V2.7.1, -2 V2.5.1, -3 V1.3.1, -4 V1.12.1 (free from etsi.org);
NXDN TS 1-A v1.3, 1-C v1.3, 1-D v1.3, 1-E v1.1, 1-F v1.0 (NXDN Forum documents, © JVCKENWOOD / Icom,
read from a public mirror; nothing copied into this folder).

Cross-check (facts only, no code or tables copied): MMDVMHost by G4KLX (https://github.com/g4klx/MMDVMHost,
GPL-2.0), read on 2026-11-19. It agrees with this implementation on: the AMBE+2 72-bit frame layout
(all 72 bit positions of C0/C1/C2+C3), DMR sync patterns and data types, NXDN FSW, LICH fields,
message type codes, SACCH/FACCH1/UDCH puncturing positions and the all-ones CRC init. It does not
cover the open questions below (it checks no DMR packet CRC-32 and has no NXDN trunking messages,
Tier III or encryption).

Test data from OK-DMR ok-dmrlib (https://github.com/OK-DMR/ok-dmrlib, AGPL-3.0), read on 2026-11-22:
three captured DMR data packets with their CRC-32 octets, used only as test inputs in `test_fec.c`
(hex data, no code).

## Encryption

Keys are the user's own (keyring structs owned by the caller, set `rx.keys` after `*_rx_init`).
Nothing here recovers, guesses or brute-forces keys. **Legal:** encrypting on amateur bands is not
allowed in most countries (US 47 CFR 97.113(a)(4), ...). The platform has to offer encryption only on
channels where the licence allows it (commercial / PMR licences).

### NXDN (TS 1-D v1.3): standard, interoperable

| Clause | Item | State | Test |
|---|---|---|---|
| 5.3.1 | scramble, PN X^15+X+1, 15-bit key | done | 7.2.2.1 UDCH vector, bit exact |
| 5.3.2 | DES-OFB, 64-bit IV | done | 7.2.2.2 UDCH vector |
| 5.3.3 | AES-256-OFB, 128-bit IV | done | 7.2.2.3 UDCH vector |
| 5.3.2.2/5.3.3.2 | IV LFSR 1+X^15+X^27+X^38+X^46+X^62+X^64 | done | 7.2.3 table, 8 outputs |
| 5.4.1/5.4.3 | Cipher Type / Key ID in VCALL, DCALL; VCALL_IV | done | |
| 5.4.4 | voice sessions: scramble per superframe; DES/AES 2 superframes, IV in FACCH1 then alternate SACCH | done | full call through RX, tracker vs replay |
| 5.4.5 | data: DCALL header IV, user data octets 2..21 decrypted in the receiver | done | spec ciphertext through UDCH frames |
| 5.4.5 | SDCALL (short data on UPCH/FACCH1) | - | same keystream functions apply, not wired |
| 6 | ESN authentication | - | the scramble polynomial of figure 6.3-2 is not readable in the published PDF |

Voice: TS 1-D 5.2 applies the cipher to the 49-bit (EHR) / 88-bit (EFR) vocoder data *before* the
vocoder FEC. EHR is complete: the receiver strips the AMBE+2 frame FEC, decrypts, re-encodes and hands
plain 72-bit frames to the vocoder (`crypt` = DECRYPTED; channel errors in C0/C1 are corrected on the
way). TX: `nxdn_vcrypt_frame()` per frame + `nxdn_vcrypt_apply72()` per VCH. All 80 EHR voice test
vectors of 7.2.1 (scramble 16, DES 32, AES 32 VCH words) are reproduced bit for bit, plus the TS 1-A
7.2.1 tone and silence patterns. EFR (9600 bps, 144-bit VCH) works the same way with the full rate FEC
(`ambe144_*`, `nxdn_vcrypt_apply144()`): all 40 EFR voice test vectors of 7.2.1 (scramble 8, DES 16,
AES 16) and the TS 1-A 7.2.2 tone / silence patterns are bit exact.

Encrypted TX call sequencing (`nxdn_vtx_*`, NXDN48 and NXDN96, scramble / DES-OFB / AES-256-OFB):
`nxdn_vtx_start()` sets Cipher Type / Key ID in the VCALL and takes the 64-bit IV generator state
(its first output is IV(1)); the header frame carries VCALL + VCALL_IV(IV(1)) in its two FACCH1 halves
(`nxdn_vtx_header2()`); `nxdn_vtx_frame()` encrypts each frame's VCH under the FEC and returns that
superframe's SACCH message: VCALL in even superframes, VCALL_IV with the next IV in odd ones (the new
IV applies from the next superframe, one IV per 2-superframe session, 5.4.4.2). Scramble: no IV,
VCALL everywhere. Tested: with generator state ABCDEF1234567890 the TX output equals all 120 voice test
vectors of 7.2.1 (EHR + EFR) and superframe 1 carries the spec's second IV (8B8DDEEB890F4CF1); 6-superframe
calls (3 IV sessions) through the receiver decode with the key and stay encrypted without it, on every RAN.
The cfw glue (`app/protos/proto_nxdn.c`) uses it for all three ciphers on both rates.

EFR full rate FEC (`ambe144_*`): 88 bits = u0..u3 (12 each, Golay(23,12)), u4..u6 (11 each,
Hamming(15,11)), u7 (7, unprotected); c1..c6 xored with 114 bits of the same LCG seeded by 16 x u0;
144-bit interleave: vector bit i (c0..c7 concatenated), r = i / 24, k = i % 24 -> air bit
12 (k / 2) + (k even ? r : 6 + (r ^ 1)). This is the IMBE full rate layout of TIA-102.BABA (P25
phase 1); written from that description and confirmed only by the NXDN vectors (no code copied).
Note: TS 1-A 7.2.2.1 prints the same 88 bits for tone #1 and #2; the FEC words differ in air bit 142
(= the last data bit), and TS 1-D 7.2.1 says the two patterns differ in their LSB, so #2 ends in ...01.

AMBE+2 frame FEC (`ambe_fec.c`): C0 = 12 bits + Golay(23,12) + even parity, C1 = 12 bits + Golay(23,12)
xored with the 23-bit LCG sequence seeded by 16 x u0 (x' = 173 x + 13849 mod 65536, bit = x >= 32768),
C2 11 bits, C3 14 bits; 36-dibit interleave. The layout is not in a public standard; it was taken from
the frame structure as used across DMR/NXDN implementations, confirmed by the 82 NXDN spec test vectors
above and matches MMDVMHost's bit positions. It is the same 72-bit frame as the DMR vocoder socket, so DMR software vocoders can use it too.
Patents: AMBE+2 is DVSI technology; this file only does error correction, no speech coding.

### DMR

ETSI TS 102 361 defines no privacy algorithm ("Privacy is not defined in the present document");
Motorola/Hytera and DMR Association schemes are not public, so they are not implemented.

| Item | State |
|---|---|
| Tier III authentication (TS 102 361-4 6.4.8): RC4(challenge &#124;&#124; K), bytes 256..258, optional PSN; C_AHOY challenge -> C_ACKU(0x48) | done, tables 6.15 / 6.16 vectors |
| cfw privacy for voice + packet data, every tier (direct, Tier II, Tier III payload channels) | done, own scheme (below) |
| Basic / Enhanced Privacy, DMRA ARC4 / AES, Hytera | - (no public spec); `dmr_priv_t.ks` is the hook for a keystream provider |

cfw privacy (only between radios running this firmware): AES-256-CTR, nonce = IV(32 random bits per
transmission) | superframe(16) | counter(16) | 'V'/'D' | key id. The keystream is xored onto the
72-bit vocoder frames as sent, so no AMBE knowledge is needed and a channel bit error stays one bit
error. Signalling: PI header (data type 0) before the voice header / data header; service options
privacy bit (0x40) set in the voice LC; on odd superframes the embedded LC slots carry a privacy LC
(FLCO 0x3E, FID 0, both configurable) so late entry works within about 0.7 s. Packet data: user data
and pad octets are encrypted, the CRC-32 covers the ciphertext. Frames that cannot be decrypted are
flagged `DMR_CRYPT_ENCRYPTED` in the voice callback so the platform mutes them. Limitation: on late
entry the frames before the first privacy LC look clear (nothing marks them yet).

## Open questions (verify with a real capture before relying on them)

1. **DMR CRC-32 octet order (B.3.9): settled.** The linearised word-swapped CRC was right; the 4 CRC
   octets go least significant first (figure B.8C: "less significant octets appear before more
   significant octets"). Confirmed with three real over-the-air packets (test data from OK-DMR
   ok-dmrlib's CRC-32 test; `test_fec.c`). TX and RX were changed from big-endian on 2026-11-22.
2. **NXDN REG_REQ / REG_RESP / GRP_REG / SITE_INFO octet layout: settled.** Checked on the rendered
   figures 6.4-44/45/49/50/53 of TS 1-A: REG unit ID octets 4-5, group 6-7, RESP cause 8, REQ
   subscriber type 8-9 + version 10; GRP_REG unit 2-3, group 4-5; SITE_INFO octet 15 bits 7-4 =
   adjacent site allocation, 1st control channel = octet 15 bits 3-0 + octet 16 bits 7-2, 2nd = octet
   16 bits 1-0 + octet 17. The code matched already.
3. **Tier III random access timing** is approximate; on a busy site the inbound CSBK must be aligned to
   the TSCC inbound slot by the platform.
4. **Vocoder boundary: settled.** EHR and EFR voice come out of the receiver decrypted (FEC done here).
5. **DMR privacy LC FLCO 0x3E** is unassigned by ETSI for FID 0; if another product uses it, change
   `DMR_PRIV_LC_FLCO` / `DMR_PRIV_LC_FID`.
6. **Vocoder frames.** DMR hands 72-bit frames (AMBE+2 3600x2450 incl. its FEC, as in the vocoder
   socket); NXDN hands 72-bit (EHR) or 144-bit (EFR) VCH. Feed them to the AMBE chip/codec as they are.
