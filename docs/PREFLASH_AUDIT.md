# Pre-flash audit: cfw on D890UV + D168UV before the first J-Link flash

Date: 2026-11-23. Scope: everything between "cfw passes in the emulator" and "cfw runs safely on a real radio".
Nothing here was run on hardware. Status key: **OK** = checked in code/tests, **FIX** = offline fix (dispatched), **done** = offline fix in place,
**HW** = can only be checked on the real radio (do it in the order of section 4).

## 1. Verified offline (2026-11-23, main session)

| Check | Result |
|---|---|
| 8 cfw builds (D890 + D168 x make / EMU=1 / HW=1 / DEBUG=1) | all rc 0, **0 warnings** |
| `make test-libs` (dmr_nxdn, p25, dstar_ysf, imbe, efr_loop) both trees | pass |
| `cfw_cps make test` | 86 pass |
| `tools/hw` pytest (fake J-Link / OpenOCD chip) | 73 pass (46 + H1-H4, 2026-11-23) |
| Emulator matrices (cfw-ui v0.5 worker) | D890 24/24, D168 22/22; hwpaths_test, features, powercut 14/14, enc, APRS, M-SMS pass |

## 2. Audit by area

| # | Area | D890UV (STM32U575) | D168UV (AT32F403A) | Status |
|---|---|---|---|---|
| 1 | Memory map vs bootloader | ld ORIGIN 0x0800C000, ASSERT; image < 0x08180000 | ORIGIN 0x08004000, ASSERT; image < 0x08080000 | OK |
| 2 | Flash writes by the cfw | codeplug 0x08180000-0x081DFFFF + settings page 0x081E0000 only, range-checked; table cache 0x081E4000+ untouched | settings 0x08080000 + codeplug 0x080A0000-0x080FFFFF (bank 2), range-checked | OK |
| 3 | HW=1 flash writes | compiled out (`CFW_HW_NO_SAVE`), CPS link off | same | done F10 (`make HW=1 HW_SAVE=1`); H4 done (`hw.py codeplug FILE` over J-Link) |
| 4 | NAND | driver is read-only (no program/erase code) | same | OK |
| 5 | VTOR | set to 0x0800C000 in Reset_Handler | set to 0x08004000 | OK |
| 6 | RAM / SP | 768 KB, SP 0x200C0000 (SRAM1-3 contiguous) | 224 KB, SP 0x20038000: needs EOPB0 = 224 KB mode. Stock SP 0x2001F670 (> 96 KB) proves the factory set it | HW (`hw.py info` decodes EOPB0; `flash` refuses an SP above it: H2 done) |
| 7 | Option bytes / RDP / FAP / TZEN | never written; hw.py refuses RDP != 0, TZEN shown | never written; FAP must be off | HW (`hw.py info`; TZEN=1 refused by `flash`: H2 done) |
| 8 | Bootloader hand-over state | cfw assumes reset clocks: clears PLL1ON while it may be SYSCLK, rewrites PLL cfg, no SysTick/NVIC clean-up | same (CRM_CFG rewritten with PLL possibly running as SCLK) | done F4 (emu test f4) |
| 9 | Power latch timing | `power_hold()` after `clock_init()` (HSE wait can take ~0.5 s at reset clock) | same | done F4 (latch first) |
| 10 | Clock failure | HSE/PLL timeouts fall through silently, wrong timing, TX still possible | same | done F9 (emu test f9) |
| 11 | Watchdog | cfw never refreshes IWDG; if the bootloader or a HW-watchdog option starts it -> reset loop. Stock D168 uses WDT only to force reset (FUN_08040248) | same | done F3 + HW (option bits: `hw.py info` decodes them, H2 done) |
| 12 | Fault handlers | HardFault/Default spin forever: if TX is on, PA stays keyed | same | done F1 (emu tests f1halt, f1reset) |
| 13 | TX time limit | TOT only in main loop and only if TOT != 0 | same | done F2 (ISR hard cap; emu tests f2cap, f2stall) |
| 14 | APC | stock formula `t + cal - ref` u16 can wrap -> near full scale; cal missing -> 0xFF | same, masked to 12 bit = up to 4095 | done F5 (clamp, no TX without cal; emu test f5) |
| 15 | First-flash TX inhibit | HW=1 still allows FM TX at channel power | same | done F7 (`HW_NOTX=1`, Low until picked; emu test f7) |
| 16 | Digital TX | HW=1 needs HW_DTX=1, dummy load only | same | OK |
| 17 | TX audio PB9 PWM / RX audio PC0 | from stock, coupling / pre-emphasis / DC level unknown | same | HW (scope + service monitor) |
| 18 | Deviation / APC level scaling | from stock cal tables (identical to stock in emulator) | same | HW (service monitor, dummy load) |
| 19 | RF chip (unknown part) register set | stock register lists replayed | same | HW (lock, RSSI, reg 0x57) |
| 20 | SCT3288 baseband / vocoder | stock boot handshake modelled; vocoder packet API unverified | same | HW (RTT log of frames) |
| 21 | LCD init | stock FUN_0805d830 sequence | stock FUN_0803d2fc, MADCTL 0xE8 | HW (visual) |
| 22 | Keys / knob / PTT pins | from stock; SWD pins PA14 (D890), PA13/PA14 (D168) are keys | same | HW (KEEP_SWD / DEBUG builds keep SWD) |
| 23 | BT / GPS / FM / accel | UART4 JieLi, USART2 GPS, I2C pot 0x2E, LIS2DW12 | AT BT UART4, USART2, I2C 0x10/0x19 | HW |
| 24 | Version / ident | CPS ident D890CFW / D168CFW; boot screen + RTT build string | same | done F8 (boot screen, RTT, CPS info block +40; emu test f8) |
| 25 | RTT debug | DEBUG=1, verified in Renode (D890) | DEBUG=1, SLEEP_DEBUG set | HW |
| 26 | Recovery | `hw.py recover` restores the chip's own first verified backup of the app window (`--stock` = package image); official USB updater as fallback; bootloader backup restore manual | same | OK (H1 done) |
| 27 | Bootloader app check (CRC / key / signature) | unknown: bootloader not in the package; D890 activation key 0x0800A000 is in the bootloader area | unknown | HW (`hw.py bootinfo` on the backup, before the first write: H3 done) |
| 28 | Activation key | cfw does not check it (no standby loop); stays untouched | n/a | OK |

## 3. Offline fixes dispatched

cfw (worker "cfw UI", both trees):
- Status 2026-11-23: F1-F10 done in both trees (`cfw/README.md` "v0.6", `hal/safety.c`), emulator test
  `cfw/tools/preflash_test.py --radio d890|d168` (f1halt f1reset f2cap f2stall f4 f5 f7 f9 f8).
- **F1** Fault handlers (HardFault, NMI, MemManage, BusFault, UsageFault, Default): first force TX off with direct register
  writes only (APC DAC = 0, TX enables low: D890 PA15/PE1 + TX GPIOs, D168 PE6/PC4; stop TIM4 PWM, PD1 high), store
  PC/LR/CFSR in a no-init RAM block, then system reset (DEBUG=1: `bkpt` then loop). Next boot: show "FAULT pc=..." once
  and log it on RTT.
- **F2** TX hard cap independent of the main loop: SysTick ISR forces TX off when TX lasts longer than the cap
  (HW builds 180 s even with TOT off) or when the main loop has not run for > 2 s while transmitting.
- **F3** IWDG/WDT refresh (key 0xAAAA) every main-loop pass and inside long blocking loops (flash erase, NAND wait,
  baseband boot wait). Harmless when the watchdog is not running. Do not start it in normal builds.
- **F4** Bootloader hand-over: in Reset_Handler load MSP from the vector table, disable SysTick, clear all NVIC
  enables/pending, clear PendSV/SysTick pending. Set the power latch (PC3) before clock_init. clock_init: switch
  SYSCLK to the internal oscillator first (D890 HSI16/MSI, D168 HICK), wait for the switch, PLL off, configure, on.
  Emulator test: preset RCC/CRM to "PLL running as SYSCLK" + SysTick running, then start the cfw: must boot.
- **F5** APC: compute in signed int, clamp to 0..max(stock table for that band at high power); no TX if the cal block
  is missing or invalid (popup), instead of 0xFF.
- **F6** D168 Makefile comment still says digital TX on DAC2 PA5 (now PB9 PWM).
- **F7** `HW_NOTX=1`: all TX refused ("TX inhibited"), for the first smoke test. HW=1 default power = Low.
- **F8** Version string (tree, build kind HW/EMU/DEBUG, date) on the boot screen, in RTT and in the CPS ident reply.
- **F9** Clock failure: if HSE or PLL does not come up, stay on the internal clock, show "CLK ERR", refuse TX.
- **F10** No way to load a codeplug into a HW build today (`CFW_PROG_HW` exists in prog.c but no Makefile option, and
  `CFW_HW_NO_SAVE` refuses writes). Add `make HW=1 HW_SAVE=1`: CPS link + codeplug/settings flash writes on, still
  range-checked; documented as "only after the owner OKs it".

hw.py (worker "hw-jlink") - **all four done 2026-11-23**, offline tests 73 pass (see docs/HARDWARE.md section 4):
- **H1** `recover` restores this chip's own verified backup of the app window by default (stock package image only
  with `--stock`), so cfw settings/codeplug pages and any factory data in free flash come back as they were.
  Done: default = the chip's first (oldest) verified backup, `--backup DIR` picks another; whole window written.
- **H2** `info` decodes watchdog option bits (D890 FLASH_OPTR IWDG_SW / IWDG_STOP / IWDG_STDBY, D168 USD
  nWDT_ATO_EN) and D168 EOPB0 (SRAM 96/224 KB); `flash` refuses a D168 image whose SP is above the configured SRAM,
  and refuses when TZEN = 1 on the D890. Done (EOPB0 at USD 0x1FFFF810: 0xFE = 224 KB, else 96 KB; also WWDG_SW,
  bank swap and boot bits shown).
- **H3** `bootinfo`: after a backup, print the bootloader's reads of the app area (vector table, CRC unit use,
  references to 0x0800A000 / app end) from `flash_full.bin` with a quick Ghidra headless or objdump pass, so a CRC
  check is known before the first write. Done with objdump (literal pools, movw/movt, mov immediates, secure
  aliases, CRC tables); writes bootinfo.txt / .json into the backup dir. Static scan: "no sign" is not proof.
- **H4** `codeplug FILE`: write a cfw_cps binary export into the codeplug window only (D890 0x08180000-0x081DFFFF,
  D168 0x080A0000-0x080FFFFF) with the same guards as `flash` (header "D8CP" + model id checked against `--radio`).
  Done: CRC-32 checked too, rest of the window erased to 0xFF, refused unless the chip's window is blank or a codeplug.

## 4. First hardware session (order)

1. J-Link connect under reset, `hw.py --radio X info`: CPU, UID, RDP 0 / FAP off, TZEN 0, watchdog bits, EOPB0.
   RDP 1 / FAP on: stop, no SWD path on that unit (USB updater only).
2. `backup`, `verify-backup` (twice identical, bootloader present). Keep the backup off the PC too.
   RF calibration, still on stock: `tools/hw/cal_dump.py --radio X --port /dev/ttyACM0` (stock USB PC link, read
   only) and `hw.py calram --compare <dump>` (SWD, RAM copy); "copy in use = NONE" = no TX plans for that radio
   (cfw/docs/CALIBRATION.md, both trees).
3. `bootinfo` (H3, no probe): know if the bootloader checks the app. Verdict "CHECK LIKELY" (CRC / HASH unit, CRC
   table, app-end words) or a key reference near the app jump: stop and ask.
4. Flash `HW=1 HW_NOTX=1 DEBUG=1` build. Check: boots, RTT boot line, LCD, keys, knob, battery, RX audio on a
   known FM signal, RSSI. `recover` once to prove the way back works, then re-flash the cfw.
5. HW=1 (FM TX, low power) into a **dummy load**: power, frequency error, deviation on a service monitor / SDR.
   Check the TX cap (F2) and TOT.
6. HW_DTX=1, dummy load: PB9 baseband for 4FSK/C4FM/GMSK, eye diagram via SDR; then radio-to-radio over the
   dummy loads (two of each radio).
7. Codeplug: `make HW=1 HW_SAVE=1` (F10, CPS link + flash writes, after the user OKs it) or a J-Link write of a
   CPS-exported image inside the codeplug window (H4).
