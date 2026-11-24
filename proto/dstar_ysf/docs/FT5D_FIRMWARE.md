# Yaesu FT5D firmware updaters: static extraction (2026-11-19)

Goal: get the FT5D YSF/C4FM implementation to check `cfw/proto/dstar_ysf/ysf` against it.
**Result: the firmware images inside the updaters are encrypted. Per the task rules nothing was done to
break the encryption, so no firmware comparison was possible, and the YSF code was not changed.**

## Inputs (`other_firmwares/C4FM/`)

| File | SHA-256 |
|---|---|
| `C4FM_MAIN_ver121(USA)/FT5D_MAIN_ver121(USA).exe` (1 312 840 B) | `e5c4bdf84e117b91c20f52198f58587933cdee0b69e9bbec71defb1a4061a8ad` |
| `C4FM_SUB_ver110(ENG)/FT5D_SUB_ver110(ENG).exe` (4 462 608 B) | `436284d2700b1b9bfaecab57416f52aa82b99459aa9e12d405daff6767ad87e3` |
| `RComms.dll`, `RFP_USB_VB.dll`, `HMSEUSBDRIVER.exe`, `vcredist_x86.exe` | identical in both folders; Renesas E8a USB comms / flash-programmer helpers, USB driver installer, MSVC runtime |

Rules followed: the binaries were never executed (not even under Wine). They were copied to a scratch folder and
read only with our own Python scripts run as `python3 -I`.

## Steps

1. Copy `other_firmwares/C4FM/` to an empty scratch folder.
2. `objdump -h` on the updaters: 4 PE sections. `.rsrc` is unusually large (MAIN 0x10F300, SUB 0x40F318).
3. Walk the PE resource directory with [`../tools/pe_rsrc.py`](../tools/pe_rsrc.py) (our own parser, no pefile):
   `python3 -I tools/pe_rsrc.py <updater.exe> <outdir>`. Resources: icons, dialogs, strings, version info, and the
   custom type `23` (named resources):
   * `RES_START_DLG`, `RES_END_DLG`: plain English instruction text.
   * `RES_UPDATE_INFO`: the firmware container. MAIN 1 048 672 B = 1 MiB + 96 B trailer; SUB 4 194 424 B =
     4 MiB + 120 B trailer.
4. The trailer is plain (little-endian):
   * MAIN: `15 bd b0 11` (4 B, checksum/tag?), `0x00100000` (image size 1 MiB), `1`, `"FT5DR(MAIN)"`, `"USA"`,
     `"1.21"`, then small fields (`06 00 40 06 01 …`, `0x020a`, `0x01ac`).
   * SUB: `c3 85 db 0b`, `0x00400000` (4 MiB), `0xFFC00000` (load address), `0`, `0xFFFFFFFF`, `0`, `1`,
     `"FT5DR/E(SUB)"`, `"English"`, `"1.10"`, `7`, …, `0x020b`, `0x01ac`.
5. Statistics of the image part ([`../tools/blob_stats.py`](../tools/blob_stats.py)): entropy ≈ 8.0 bit/byte
   over the code areas; the erased-flash area of MAIN (last ~0x80000 B) is one 8-byte block
   (`15bdb011fb0a8dcb`) repeated 66 932 times, and a few other 8-byte blocks repeat hundreds of times, always
   8-byte aligned. That is the signature of a **64-bit block cipher in ECB mode**. It is not a fixed XOR key:
   the most common blocks differ from each other by unstructured values (no all-`FF` difference for 00 vs FF
   fill), and no period below 8 bytes exists.
6. Searched every extracted file and the updaters for YSF constants in plain form (sync `D471C9634D` both byte
   orders, whitening `93 D7 51 21 9C 2F`, Golay rows, CRC-CCITT tables both orders, S-record / Intel-hex
   markers): **no hits**.

Extracted (still encrypted) images, kept for reference: `other_firmwares/C4FM_extracted/`

| File | SHA-256 |
|---|---|
| `FT5D_MAIN_ver121_USA.enc.bin` | `6d086069b5f990f6991fb1c9ec42cb84120664f29f623c43a9c01b0c6a0d3577` |
| `FT5D_SUB_ver110_ENG.enc.bin` | `ab50a679d7683f0122d580b2596979d214f5b90ad5781d252de50889b4665ddf` |

## CPU

* Updaters ship Renesas `RComms.dll` ("Renesas Technology Communications DLL", class `E8A_USB`) and
  `RFP_USB_VB.dll` (Renesas Flash Programmer), so both CPUs are Renesas MCUs programmed over an E8a-style USB
  link.
* SUB trailer: 4 MiB image at `0xFFC00000` — code flash ending at `0xFFFFFFFF` is the **Renesas RX** memory map
  (RX65N/RX72x-class, 4 MB). MAIN: 1 MiB, most likely also RX (no address in the trailer).
* Which CPU runs the C4FM baseband cannot be told from the encrypted images.

## Stopped here

* No Ghidra project was made: there is no readable code to load (task rule: if the image is encrypted, report
  and stop).
* The decryption is done either by the updater (key inside the .exe) or by the radio's boot code. Finding out
  would mean reverse-engineering the cipher/key, which is out of scope.

## Effect on `cfw/proto/dstar_ysf/ysf`

Nothing from the FT5D could be compared, so the implementation stays based on the Yaesu spec rev 1.02 and
its open items are unchanged (see README "Unverified / limits"): Voice FR bit order, V/D 1 VCH order, CRC
bit order over whitened data, DG-ID in the SC field. Ways to close them without the firmware: an over-the-air
capture from a real YSF radio (decode the HC/TC CSD with our RX; a CRC pass proves CRC, whitening, trellis and
interleave together), or a pcap of YSF network traffic (YSF network frames carry the 720 payload bits + FICH).
