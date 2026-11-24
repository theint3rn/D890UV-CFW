# Stock columns and the cfw

Written by `cfw/tools/stock_columns.py`; do not edit. State: **used** = the cfw acts on it, **not possible** = the hardware or the cfw cannot, **not done** = could be done. Column numbers are per radio (they differ from 173 on).

| Radio | Table | used | not possible | not done |
|---|---|---|---|---|
| D890UV | OptionalSetting | 66 | 73 | 60 |
| D890UV | Channel | 26 | 15 | 19 |
| D890UV | AlarmSetting | 20 | 0 | 3 |
| D890UV | TalkAlias | 2 | 0 | 0 |
| D890UV | FriendsList | 2 | 0 | 0 |
| D168UV | OptionalSetting | 65 | 72 | 58 |
| D168UV | Channel | 25 | 13 | 7 |
| D168UV | AlarmSetting | 21 | 0 | 4 |
| D168UV | TalkAlias | 1 | 0 | 1 |
| D168UV | FriendsList | 2 | 0 | 0 |

## D890UV, Optional Settings

| # | Column | State | What / why |
|---|---|---|---|
| 0 | Beep | used | key beep |
| 1 | DSP | not done | meaning or value list not confirmed |
| 2 | KeyLock | used | auto key lock |
| 3 | AutoPowerOff | used | auto power off |
| 4 | TOT | used | TX timeout |
| 5 | Language | not possible | belongs to a feature the cfw does not have: English only |
| 6 | StartDspSet | used | Character = boot text |
| 7 | Password | used | power-on password |
| 8 | Step | used | VFO step (V1.05 table, 2.5K first); absent: the cfw Radio Settings step, which is always stored |
| 9 | SQL1 | used | squelch level |
| 10 | SQL2 | not possible | one receiver in the cfw: no second (B) channel |
| 11 | PowerSave | used | RX duty cycle |
| 12 | VOX | used | VOX |
| 13 | VOX_Delay | used | 0.5-3.0 s VOX hang |
| 14 | ScanType | used | VFO scan TO / CO / SE |
| 15 | MicLevel | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 16 | RMV1 | not possible | reserved / no effect in the stock CPS |
| 17 | RMV2 | not done | meaning or value list not confirmed |
| 18 | STE_Type | not done | no such hardware, or not traced: CTCSS tail is made in the RF chip; its phase control is not decoded |
| 19 | STE_Freq | not done | no such hardware, or not traced: as STE_Type |
| 20 | GroupTalkHold | not done | meaning or value list not confirmed |
| 21 | PersonTalkHold | not done | meaning or value list not confirmed |
| 22 | PreWave | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 23 | Preamble | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 24 | Work_FMCH | used | FM memory at power on |
| 25 | FM_VFO | not done | meaning or value list not confirmed |
| 26 | Work_Zone1 | not done | the cfw has its own setting for this: zone remembered by the radio |
| 27 | Work_Zone2 | not possible | one receiver in the cfw: no second (B) channel |
| 28 | FM_En | used | broadcast receiver at power on |
| 29 | Record_En | not possible | belongs to a feature the cfw does not have: recorder |
| 30 | FailAlarm | used | man down |
| 31 | MonType | not done | meaning or value list not confirmed |
| 32 | Lightness | used | backlight |
| 33 | AutoBKLightTime | used | backlight time |
| 34 | Gps | used | GPS on, clock from GPS |
| 35 | MsgRing | used | SMS beep |
| 36 | FreqDis | not done | the cfw has its own setting for this: the main screen always shows both |
| 37 | FmMon | used | busy channel pauses the broadcast |
| 38 | MainState | not possible | one receiver in the cfw: no second (B) channel |
| 39 | SubMode | not possible | one receiver in the cfw: no second (B) channel |
| 40 | TBST | used | FM: Up key while PTT |
| 41 | CallRing | used | private call beep |
| 42 | TmZone | used | GPS clock |
| 43 | TalkTips | not possible | no such hardware, or not traced: the MCU beep uses the same audio output as the transmitted signal |
| 44 | OverVoice | not done | meaning or value list not confirmed |
| 45 | VoxHeadset | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU: detector source |
| 46 | Start_Char | used | boot text |
| 47 | Reserved_Start_Char | not possible | reserved / no effect in the stock CPS |
| 48 | Password_Char | used | power-on password (digits) |
| 49 | Reserved_EMG_Kind | not possible | reserved / no effect in the stock CPS |
| 50 | Voice_Note | not possible | belongs to a feature the cfw does not have: voice prompts |
| 51 | SqOnVoice | used | beep after a digital call (3 pitches, cfw's own) |
| 52 | IdleWait | not done | meaning or value list not confirmed |
| 53 | MissCallFilter | used | own ID not logged |
| 54 | Boot_Sound | used | startup beep |
| 55 | TalkOverPoint | not possible | one receiver in the cfw: no second (B) channel |
| 56 | MaxVol | used | volume limit |
| 57 | DigiStunKillEn | not possible | belongs to a feature the cfw does not have: remote kill (kept out on purpose) |
| 58 | Reserved_DigiKillEn | not possible | reserved / no effect in the stock CPS |
| 59 | RemoteMoniEn | not possible | would transmit on its own: remote monitor |
| 60 | GpsReplyEn | not possible | would transmit on its own |
| 61 | ContactOutSetEn | not done | meaning or value list not confirmed |
| 62 | Start_Char2 | used | boot text |
| 63 | PfLongTime | not done | meaning or value list not confirmed |
| 64 | VolNoteEn | not done | meaning or value list not confirmed |
| 65 | AutoRepeater | used | VFO auto repeater |
| 66 | DigiMoni | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 67 | AprsUpDate | not possible | reserved / no effect in the stock CPS |
| 68 | AprsTxFreq | not possible | reserved / no effect in the stock CPS |
| 69 | ManDownWait | used | man down wait |
| 70 | AnaHoldTime | not possible | one receiver in the cfw: no second (B) channel: hold of the dual watch |
| 71 | DateDisKind | used | clock in the status bar |
| 72 | VfoScanFreq0 | used | UHF VFO scan start |
| 73 | VfoScanFreq1 | used | UHF VFO scan end |
| 74 | VfoScanFreq2 | used | VHF VFO scan start |
| 75 | VfoScanFreq3 | used | VHF VFO scan end |
| 76 | UhfAutoRep | used | offset entry |
| 77 | VhfAutoRep | used | offset entry |
| 78 | CurTalkPathHold | not done | meaning or value list not confirmed |
| 79 | PriZoneA | not done | meaning or value list not confirmed |
| 80 | PriZoneB | not possible | one receiver in the cfw: no second (B) channel |
| 81 | EnSoundEffect | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 82 | MsgOacsuSet | not done | meaning or value list not confirmed |
| 83 | DigiMoniCc | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 84 | DigiMoniId | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 85 | LastCallDis | not done | meaning or value list not confirmed |
| 86 | EarMaxVol | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU: headphone volume |
| 87 | GpsTextUsed | not done | meaning or value list not confirmed |
| 88 | RecordDelay | not possible | belongs to a feature the cfw does not have: recorder |
| 89 | CallModeDisKind | not done | meaning or value list not confirmed |
| 90 | BlueToothOn | used | module probed and set up at power on (no BT audio) |
| 91 | MicInBlueTooth | not possible | no such hardware, or not traced: audio route to the BT module |
| 92 | SpkInBlueTooth | not possible | no such hardware, or not traced: audio route to the BT module |
| 93 | WtRecordNote | not possible | belongs to a feature the cfw does not have: recorder |
| 94 | MeasurePeriod | not possible | would transmit on its own: ranging |
| 95 | BhtMicGain | used | module gain command |
| 96 | BhtSpkGain | used | module gain command |
| 97 | ChanNumDisKind | not done | meaning or value list not confirmed |
| 98 | MenuDisWorkContact | not done | meaning or value list not confirmed |
| 99 | WanderPerod | used | auto roaming period |
| 100 | Ext_Opt1 | not done | meaning or value list not confirmed |
| 101 | WanderEffectWait | not done | meaning or value list not confirmed |
| 102 | Ext_Opt2 | not done | meaning or value list not confirmed |
| 103 | Scan_Pri_Current | not possible | reserved / no effect in the stock CPS |
| 104 | GroupCallS_Ext | not possible | reserved / no effect in the stock CPS |
| 105 | Contact_Compression | not possible | reserved / no effect in the stock CPS |
| 106 | RX_Contact_Compression | not possible | reserved / no effect in the stock CPS |
| 107 | File_Optimization | not possible | reserved / no effect in the stock CPS |
| 108 | AutoRepFreq0 | used | range |
| 109 | AutoRepFreq1 | used | range |
| 110 | AutoRepFreq2 | used | range |
| 111 | AutoRepFreq3 | used | range |
| 112 | AutoRepeaterB | not possible | one receiver in the cfw: no second (B) channel |
| 113 | BookOwnId | not done | meaning or value list not confirmed |
| 114 | AprsDistanceDis | not done | meaning or value list not confirmed |
| 115 | StartChUse | used | startup channel |
| 116 | StartZone1 | used | startup channel |
| 117 | StartZone2 | not possible | one receiver in the cfw: no second (B) channel |
| 118 | StartCurChan1 | used | startup channel |
| 119 | StartCurChan2 | not possible | one receiver in the cfw: no second (B) channel |
| 120 | MenuLowPower | used | menu exit time |
| 121 | BsModeCheck | not possible | would transmit on its own: repeater check |
| 122 | TimeBsCheck | not possible | would transmit on its own: repeater check |
| 123 | BsCheckTimes | not possible | would transmit on its own: repeater check |
| 124 | FixRomanStartOp | used | Fixed Time only; Out of Range not done |
| 125 | TxDimWait | not done | meaning or value list not confirmed |
| 126 | DiviDisEn | not possible | one receiver in the cfw: no second (B) channel |
| 127 | LastHeardChanSet | not done | meaning or value list not confirmed |
| 128 | ChanNameColour | used | channel name colour |
| 129 | OutRepNote | not possible | would transmit on its own: repeater check |
| 130 | RxDimWait | not done | meaning or value list not confirmed |
| 131 | TimeRoamOn | used | auto roaming |
| 132 | CurRoamZone | used | roaming zone |
| 133 | RoamEffectChanDis | not done | meaning or value list not confirmed |
| 134 | FixTimeMute | not done | meaning or value list not confirmed |
| 135 | comVersion | not possible | reserved / no effect in the stock CPS |
| 136 | OutNoteTimes | not possible | would transmit on its own: repeater check |
| 137 | StartTestGps | not done | meaning or value list not confirmed |
| 138 | StartResetEn | not possible | belongs to a feature the cfw does not have: reset key combination |
| 139 | BhtHoldTime | not possible | belongs to a feature the cfw does not have: BT audio |
| 140 | BhtHoldDelay | not possible | belongs to a feature the cfw does not have: BT audio |
| 141 | WxAlarmSign | used | NOAA 1050 Hz alert |
| 142 | BhtPttHold | not possible | belongs to a feature the cfw does not have: BT PTT |
| 143 | UhfAutoRep2 | used | second range |
| 144 | VhfAutoRep2 | used | second range |
| 145 | CurUhfAutoRep | not possible | one receiver in the cfw: no second (B) channel: stock run-time copy |
| 146 | CurVhfAutoRep | not possible | one receiver in the cfw: no second (B) channel: stock run-time copy |
| 147 | PttSleepTime | not possible | belongs to a feature the cfw does not have: BT PTT |
| 148 | GpsMode | not done | meaning or value list not confirmed |
| 149 | SteTime | not done | no such hardware, or not traced: as STE_Type |
| 150 | CallSignColour | not done | meaning or value list not confirmed |
| 151 | AutoRepMinFreqV2 | used | second range |
| 152 | AutoRepMaxFreqV2 | used | second range |
| 153 | AutoRepMinFreqU2 | used | second range |
| 154 | autoRepMaxFreqU2 | used | second range |
| 155 | DialGroupHold | not done | meaning or value list not confirmed |
| 156 | DialPrivateHold | not done | meaning or value list not confirmed |
| 157 | ChanNameColourB | not possible | one receiver in the cfw: no second (B) channel |
| 158 | DigiEmgKind | not possible | belongs to a feature the cfw does not have: emergency call |
| 159 | WorkCharDisColour | used | frequency colour |
| 160 | bkpic | not possible | belongs to a feature the cfw does not have: background picture |
| 161 | TotPreEn | used | warning before TX timeout |
| 162 | TxAgcCon | not done | meaning or value list not confirmed |
| 163 | ZoneNameColourA | not done | meaning or value list not confirmed |
| 164 | ZoneNameColourB | not possible | one receiver in the cfw: no second (B) channel |
| 165 | ApoKind | not done | meaning or value list not confirmed |
| 166 | SmsFormat | not done | the cfw has its own setting for this: Settings > SMS format |
| 167 | ChanDisCtr | not done | meaning or value list not confirmed |
| 168 | AnaSqOnVoice | used | beep when the FM squelch closes |
| 169 | DateDisFormat | not done | meaning or value list not confirmed |
| 170 | AnaMic | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 171 | ZoneBarsEn | not done | meaning or value list not confirmed |
| 172 | VcallRpheader | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 173 | RepMode | not possible | belongs to a feature the cfw does not have: radio as repeater |
| 174 | RepCcLimit | not possible | belongs to a feature the cfw does not have: radio as repeater |
| 175 | RepSlotPathA | not possible | belongs to a feature the cfw does not have: radio as repeater |
| 176 | RepSlotPathB | not possible | belongs to a feature the cfw does not have: radio as repeater |
| 177 | DcsSte | not done | no such hardware, or not traced: as STE_Type |
| 178 | Drc | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 179 | DmrRasEn | not possible | belongs to a feature the cfw does not have: RAS |
| 180 | RxNr | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 181 | TxNr | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 182 | NightMode | not done | meaning or value list not confirmed |
| 183 | CurWxChan | not done | meaning or value list not confirmed: channel order of the list |
| 184 | AmChanVfo | not done | meaning or value list not confirmed |
| 185 | CurAmChan | not done | meaning or value list not confirmed |
| 186 | AmOffset | used | AM down-converter side |
| 187 | AmSqLevel | not done | no such hardware, or not traced: AM squelch: no level scale for the down-converter |
| 188 | BtNrBefore | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 189 | BtNrAfter | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 190 | SateLocalKind | not possible | belongs to a feature the cfw does not have: satellite |
| 191 | SateTxPower | not possible | belongs to a feature the cfw does not have: satellite |
| 192 | SateAnaSql | not possible | belongs to a feature the cfw does not have: satellite |
| 193 | SateAosLimit | not possible | belongs to a feature the cfw does not have: satellite |
| 194 | DigiProtocal | not done | meaning or value list not confirmed |
| 195 | NxdnMic | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 196 | ResetDigiProtocal | not done | meaning or value list not confirmed |
| 197 | VolType | not possible | no such hardware, or not traced: analog volume knob |
| 198 | MinVolData | not possible | no such hardware, or not traced: analog volume knob |

## D890UV, channel columns the cfw does not model itself

| # | Column | State | What / why |
|---|---|---|---|
| 12 | Radio ID | used | other session's section (signalling, keys, Radio ID) |
| 13 | Busy Lock/TX Permit | used | any value but Always |
| 14 | Squelch Mode | used | CTCSS / DCS decoder (FM) |
| 15 | Optional Signal | used | other session's section (signalling, keys, Radio ID) |
| 16 | DTMF ID | used | other session's section (signalling, keys, Radio ID) |
| 17 | 2Tone ID | used | other session's section (signalling, keys, Radio ID) |
| 18 | 5Tone ID | used | other session's section (signalling, keys, Radio ID) |
| 19 | PTT ID | used | other session's section (signalling, keys, Radio ID) |
| 24 | PTT Prohibit | used |  |
| 25 | Reverse | used |  |
| 26 | Digital Duplex | not possible | one receiver in the cfw: no second (B) channel |
| 27 | Slot Suit | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 28 | AES Digital Encryption | used | other session's section (signalling, keys, Radio ID) |
| 29 | Digital Encryption | used | other session's section (signalling, keys, Radio ID) |
| 30 | Call Confirmation | not possible | would transmit on its own |
| 32 | Work Alone | used | warning, then the emergency alarm |
| 33 | Custom CTCSS | not done | the cfw has its own setting for this: the cfw takes any tone in the channel's CTCSS fields |
| 34 | 2TONE Decode | used | other session's section (signalling, keys, Radio ID) |
| 35 | Ranging | not possible | would transmit on its own |
| 36 | Idle TX | not possible | would transmit on its own |
| 37 | APRS RX | not possible | belongs to a feature the cfw does not have: DMR APRS receive |
| 38 | Analog APRS PTT Mode | used | End: beacon after PTT (not in HW=1 builds) |
| 39 | Digital APRS PTT Mode | not possible | belongs to a feature the cfw does not have: DMR APRS |
| 40 | APRS Report Type | used | Analog: for the line above |
| 41 | Digital APRS Report Channel | not possible | belongs to a feature the cfw does not have: DMR APRS |
| 42 | Correct Frequency[Hz] | used |  |
| 43 | SMS Confirmation | not possible | would transmit on its own |
| 44 | Exclude channel from roaming | used |  |
| 45 | DMR MODE | used | 2 / 3 = TDMA direct |
| 46 | DataACK Disable | not possible | belongs to a feature the cfw does not have: the cfw sends no data ACK |
| 47 | R5toneBot | used | other session's section (signalling, keys, Radio ID) |
| 48 | R5ToneEot | used | other session's section (signalling, keys, Radio ID) |
| 49 | Auto Scan | used | starts the channel's scan list |
| 50 | Ana APRS Mute | not done | meaning or value list not confirmed |
| 51 | Send Talker Alias DMR/NX | used | DMR alias per channel |
| 52 | AnaAprsTxPath | not done | meaning or value list not confirmed |
| 53 | ARC4 | used | other session's section (signalling, keys, Radio ID) |
| 54 | ex_emg_kind | not possible | belongs to a feature the cfw does not have: emergency |
| 55 | Rpga_Mdc | used | other session's section (signalling, keys, Radio ID) |
| 56 | DisturEn | not possible | no such hardware, or not traced: clock shift |
| 57 | DisturFreq | not possible | no such hardware, or not traced: clock shift |
| 58 | dmr_crc_ignore | not done | meaning or value list not confirmed |
| 59 | compand | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 60 | tx_talkalaes | not done | meaning or value list not confirmed |
| 61 | dup_call | not done | meaning or value list not confirmed |
| 62 | tx_int | not done | meaning or value list not confirmed |
| 63 | BtRxState | not possible | no such hardware, or not traced: BT audio |
| 64 | idle_tx | not possible | would transmit on its own |
| 65 | nxdn_wn | not done | meaning or value list not confirmed |
| 66 | NxdnRpga | not done | meaning or value list not confirmed |
| 67 | nxdnSqCon | not done | meaning or value list not confirmed |
| 68 | NxdnTxBusy | not done | meaning or value list not confirmed |
| 69 | NxDnPttId | not done | meaning or value list not confirmed |
| 70 | EnRan | not done | meaning or value list not confirmed |
| 71 | DeRan | not done | meaning or value list not confirmed |
| 72 | NxdnEncry | used | other session's section (signalling, keys, Radio ID) |
| 73 | NxdnGroupId | not done | meaning or value list not confirmed |
| 74 | NxdnIdNum | not done | meaning or value list not confirmed |
| 75 | NxdnStateNum | not done | meaning or value list not confirmed |
| 76 | txcc | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |

## D890UV, Alarm Setting (.rdt-only dialog, table 64)

| # | Column | State | What / why |
|---|---|---|---|
| 0 | Analog Emergency Alarm | used | type of the alarm on non-DMR channels (transmit part not in HW=1 builds) |
| 1 | Analog ENI Type | not done | meaning or value list not confirmed: list texts and the Emergency ID are not decoded |
| 2 | Analog Alarm Time [s] | used | local alarm time |
| 3 | Analog Duration of TX [s] | used | TX time (transmit part not in HW=1 builds) |
| 4 | Analog Duration of RX [s] | used | pause (transmit part not in HW=1 builds) |
| 5 | Analog Emergency Channel | used | alarm channel (transmit part not in HW=1 builds) |
| 6 | Analog ENI Send Select | used | assigned / selected channel (transmit part not in HW=1 builds) |
| 7 | Analog Emergency Cycle (0 = Continuous) | used | number of transmissions (transmit part not in HW=1 builds) |
| 8 | Digital Emergency Alarm | used | type of the alarm on DMR channels (transmit part not in HW=1 builds) |
| 9 | Digital Emergency Channel | used | alarm channel (transmit part not in HW=1 builds) |
| 10 | Digital Alarm Time [s] | used | local alarm time |
| 11 | Digital Duration of TX [s] | used | TX time (transmit part not in HW=1 builds) |
| 12 | Digital Duration of RX [s] | used | pause (transmit part not in HW=1 builds) |
| 13 | Digital ENI Send Select | used | assigned / selected channel (transmit part not in HW=1 builds) |
| 14 | Digital Emergency Cycle (0 = Continuous) | used | number of transmissions (transmit part not in HW=1 builds) |
| 15 | Digital TG/DMR ID | used | target of the alarm calls (transmit part not in HW=1 builds) |
| 16 | Digital Call Type | used | private / group / all (transmit part not in HW=1 builds) |
| 17 | Receive Alarm | used | popup + alert on a received DMR emergency call |
| 18 | QDC1200 Kind | not done | meaning or value list not confirmed: how the stock radio combines it with the emergency packet |
| 20 | QDC1200 Private ID | not done | meaning or value list not confirmed: as AlarmQKind |
| 22 | Work Alone Response Time (0 = 1 min) | used | response time |
| 23 | Work Alone Warning Time (0 = 1 s) | used | warning time |
| 24 | Work Alone Response | used | key / PTT response |

## D890UV, Talk Alias / AIR Alias (.rdt-only dialog, table 70)

| # | Column | State | What / why |
|---|---|---|---|
| 1 | Alias Display Priority | used | off / contact first / alias first |
| 2 | Alias Data Format | used | TX alias form |

## D890UV, Friends List (.rdt-only dialog, table 71)

| # | Column | State | What / why |
|---|---|---|---|
| 0 | No. | used | list order |
| 1 | DMR ID | used | Radio extras > Friends: private call target |

## D168UV, Optional Settings

| # | Column | State | What / why |
|---|---|---|---|
| 0 | Beep | used | key beep |
| 1 | DSP | not done | meaning or value list not confirmed |
| 2 | KeyLock | used | auto key lock |
| 3 | AutoPowerOff | used | auto power off |
| 4 | TOT | used | TX timeout |
| 5 | Language | not possible | belongs to a feature the cfw does not have: English only |
| 6 | StartDspSet | used | Character = boot text |
| 7 | Password | used | power-on password |
| 8 | Step | used | VFO step (V1.05 table, 2.5K first); absent: the cfw Radio Settings step, which is always stored |
| 9 | SQL1 | used | squelch level |
| 10 | SQL2 | not possible | one receiver in the cfw: no second (B) channel |
| 11 | PowerSave | used | RX duty cycle |
| 12 | VOX | used | VOX |
| 13 | VOX_Delay | used | 0.5-3.0 s VOX hang |
| 14 | ScanType | used | VFO scan TO / CO / SE |
| 15 | MicLevel | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 16 | RMV1 | not possible | reserved / no effect in the stock CPS |
| 17 | RMV2 | not done | meaning or value list not confirmed |
| 18 | STE_Type | not done | no such hardware, or not traced: CTCSS tail is made in the RF chip; its phase control is not decoded |
| 19 | STE_Freq | not done | no such hardware, or not traced: as STE_Type |
| 20 | GroupTalkHold | not done | meaning or value list not confirmed |
| 21 | PersonTalkHold | not done | meaning or value list not confirmed |
| 22 | PreWave | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 23 | Preamble | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 24 | Work_FMCH | used | FM memory at power on |
| 25 | FM_VFO | not done | meaning or value list not confirmed |
| 26 | Work_Zone1 | not done | the cfw has its own setting for this: zone remembered by the radio |
| 27 | Work_Zone2 | not possible | one receiver in the cfw: no second (B) channel |
| 28 | FM_En | used | broadcast receiver at power on |
| 29 | Record_En | not possible | belongs to a feature the cfw does not have: recorder |
| 30 | FailAlarm | used | man down |
| 31 | MonType | not done | meaning or value list not confirmed |
| 32 | Lightness | used | backlight |
| 33 | AutoBKLightTime | used | backlight time |
| 34 | Gps | used | GPS on, clock from GPS |
| 35 | MsgRing | used | SMS beep |
| 36 | FreqDis | not done | the cfw has its own setting for this: the main screen always shows both |
| 37 | FmMon | used | busy channel pauses the broadcast |
| 38 | MainState | not possible | one receiver in the cfw: no second (B) channel |
| 39 | SubMode | not possible | one receiver in the cfw: no second (B) channel |
| 40 | TBST | used | FM: Up key while PTT |
| 41 | CallRing | used | private call beep |
| 42 | TmZone | used | GPS clock |
| 43 | TalkTips | not possible | no such hardware, or not traced: the MCU beep uses the same audio output as the transmitted signal |
| 44 | OverVoice | not done | meaning or value list not confirmed |
| 45 | VoxHeadset | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU: detector source |
| 46 | Start_Char | used | boot text |
| 47 | Reserved_Start_Char | not possible | reserved / no effect in the stock CPS |
| 48 | Password_Char | used | power-on password (digits) |
| 49 | Reserved_EMG_Kind | not possible | reserved / no effect in the stock CPS |
| 50 | Voice_Note | not possible | belongs to a feature the cfw does not have: voice prompts |
| 51 | SqOnVoice | used | beep after a digital call (3 pitches, cfw's own) |
| 52 | IdleWait | not done | meaning or value list not confirmed |
| 53 | MissCallFilter | used | own ID not logged |
| 54 | Boot_Sound | used | startup beep |
| 55 | TalkOverPoint | not possible | one receiver in the cfw: no second (B) channel |
| 56 | MaxVol | used | volume limit |
| 57 | DigiStunKillEn | not possible | belongs to a feature the cfw does not have: remote kill (kept out on purpose) |
| 58 | Reserved_DigiKillEn | not possible | reserved / no effect in the stock CPS |
| 59 | RemoteMoniEn | not possible | would transmit on its own: remote monitor |
| 60 | GpsReplyEn | not possible | would transmit on its own |
| 61 | ContactOutSetEn | not done | meaning or value list not confirmed |
| 62 | Start_Char2 | used | boot text |
| 63 | PfLongTime | not done | meaning or value list not confirmed |
| 64 | VolNoteEn | not done | meaning or value list not confirmed |
| 65 | AutoRepeater | used | VFO auto repeater |
| 66 | DigiMoni | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 67 | AprsUpDate | not possible | reserved / no effect in the stock CPS |
| 68 | AprsTxFreq | not possible | reserved / no effect in the stock CPS |
| 69 | ManDownWait | used | man down wait |
| 70 | AnaHoldTime | not possible | one receiver in the cfw: no second (B) channel: hold of the dual watch |
| 71 | DateDisKind | used | clock in the status bar |
| 72 | VfoScanFreq0 | used | UHF VFO scan start |
| 73 | VfoScanFreq1 | used | UHF VFO scan end |
| 74 | VfoScanFreq2 | used | VHF VFO scan start |
| 75 | VfoScanFreq3 | used | VHF VFO scan end |
| 76 | UhfAutoRep | used | offset entry |
| 77 | VhfAutoRep | used | offset entry |
| 78 | CurTalkPathHold | not done | meaning or value list not confirmed |
| 79 | PriZoneA | not done | meaning or value list not confirmed |
| 80 | PriZoneB | not possible | one receiver in the cfw: no second (B) channel |
| 81 | EnSoundEffect | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 82 | MsgOacsuSet | not done | meaning or value list not confirmed |
| 83 | DigiMoniCc | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 84 | DigiMoniId | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 85 | LastCallDis | not done | meaning or value list not confirmed |
| 86 | EarMaxVol | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU: headphone volume |
| 87 | GpsTextUsed | not done | meaning or value list not confirmed |
| 88 | RecordDelay | not possible | belongs to a feature the cfw does not have: recorder |
| 89 | CallModeDisKind | not done | meaning or value list not confirmed |
| 90 | BlueToothOn | used | module probed and set up at power on (no BT audio) |
| 91 | MicInBlueTooth | not possible | no such hardware, or not traced: audio route to the BT module |
| 92 | SpkInBlueTooth | not possible | no such hardware, or not traced: audio route to the BT module |
| 93 | WtRecordNote | not possible | belongs to a feature the cfw does not have: recorder |
| 94 | MeasurePeriod | not possible | would transmit on its own: ranging |
| 95 | BhtMicGain | used | module gain command |
| 96 | BhtSpkGain | used | module gain command |
| 97 | ChanNumDisKind | not done | meaning or value list not confirmed |
| 98 | MenuDisWorkContact | not done | meaning or value list not confirmed |
| 99 | WanderPerod | used | auto roaming period |
| 100 | Ext_Opt1 | not done | meaning or value list not confirmed |
| 101 | WanderEffectWait | not done | meaning or value list not confirmed |
| 102 | Ext_Opt2 | not done | meaning or value list not confirmed |
| 103 | Scan_Pri_Current | not possible | reserved / no effect in the stock CPS |
| 104 | GroupCallS_Ext | not possible | reserved / no effect in the stock CPS |
| 105 | Contact_Compression | not possible | reserved / no effect in the stock CPS |
| 106 | RX_Contact_Compression | not possible | reserved / no effect in the stock CPS |
| 107 | File_Optimization | not possible | reserved / no effect in the stock CPS |
| 108 | AutoRepFreq0 | used | range |
| 109 | AutoRepFreq1 | used | range |
| 110 | AutoRepFreq2 | used | range |
| 111 | AutoRepFreq3 | used | range |
| 112 | AutoRepeaterB | not possible | one receiver in the cfw: no second (B) channel |
| 113 | BookOwnId | not done | meaning or value list not confirmed |
| 114 | AprsDistanceDis | not done | meaning or value list not confirmed |
| 115 | StartChUse | used | startup channel |
| 116 | StartZone1 | used | startup channel |
| 117 | StartZone2 | not possible | one receiver in the cfw: no second (B) channel |
| 118 | StartCurChan1 | used | startup channel |
| 119 | StartCurChan2 | not possible | one receiver in the cfw: no second (B) channel |
| 120 | MenuLowPower | used | menu exit time |
| 121 | BsModeCheck | not possible | would transmit on its own: repeater check |
| 122 | TimeBsCheck | not possible | would transmit on its own: repeater check |
| 123 | BsCheckTimes | not possible | would transmit on its own: repeater check |
| 124 | FixRomanStartOp | used | Fixed Time only; Out of Range not done |
| 125 | TxDimWait | not done | meaning or value list not confirmed |
| 126 | DiviDisEn | not possible | one receiver in the cfw: no second (B) channel |
| 127 | LastHeardChanSet | not done | meaning or value list not confirmed |
| 128 | ChanNameColour | used | channel name colour |
| 129 | OutRepNote | not possible | would transmit on its own: repeater check |
| 130 | RxDimWait | not done | meaning or value list not confirmed |
| 131 | TimeRoamOn | used | auto roaming |
| 132 | CurRoamZone | used | roaming zone |
| 133 | RoamEffectChanDis | not done | meaning or value list not confirmed |
| 134 | FixTimeMute | not done | meaning or value list not confirmed |
| 135 | comVersion | not possible | reserved / no effect in the stock CPS |
| 136 | OutNoteTimes | not possible | would transmit on its own: repeater check |
| 137 | StartTestGps | not done | meaning or value list not confirmed |
| 138 | StartResetEn | not possible | belongs to a feature the cfw does not have: reset key combination |
| 139 | BhtHoldTime | not possible | belongs to a feature the cfw does not have: BT audio |
| 140 | BhtHoldDelay | not possible | belongs to a feature the cfw does not have: BT audio |
| 141 | WxAlarmSign | used | NOAA 1050 Hz alert |
| 142 | BhtPttHold | not possible | belongs to a feature the cfw does not have: BT PTT |
| 143 | UhfAutoRep2 | used | second range |
| 144 | VhfAutoRep2 | used | second range |
| 145 | CurUhfAutoRep | not possible | one receiver in the cfw: no second (B) channel: stock run-time copy |
| 146 | CurVhfAutoRep | not possible | one receiver in the cfw: no second (B) channel: stock run-time copy |
| 147 | PttSleepTime | not possible | belongs to a feature the cfw does not have: BT PTT |
| 148 | GpsMode | not done | meaning or value list not confirmed |
| 149 | SteTime | not done | no such hardware, or not traced: as STE_Type |
| 150 | CallSignColour | not done | meaning or value list not confirmed |
| 151 | AutoRepMinFreqV2 | used | second range |
| 152 | AutoRepMaxFreqV2 | used | second range |
| 153 | AutoRepMinFreqU2 | used | second range |
| 154 | autoRepMaxFreqU2 | used | second range |
| 155 | DialGroupHold | not done | meaning or value list not confirmed |
| 156 | DialPrivateHold | not done | meaning or value list not confirmed |
| 157 | ChanNameColourB | not possible | one receiver in the cfw: no second (B) channel |
| 158 | DigiEmgKind | not possible | belongs to a feature the cfw does not have: emergency call |
| 159 | WorkCharDisColour | used | frequency colour |
| 160 | bkpic | not possible | belongs to a feature the cfw does not have: background picture |
| 161 | TotPreEn | used | warning before TX timeout |
| 162 | TxAgcCon | not done | meaning or value list not confirmed |
| 163 | ZoneNameColourA | not done | meaning or value list not confirmed |
| 164 | ZoneNameColourB | not possible | one receiver in the cfw: no second (B) channel |
| 165 | ApoKind | not done | meaning or value list not confirmed |
| 166 | SmsFormat | not done | the cfw has its own setting for this: Settings > SMS format |
| 167 | ChanDisCtr | not done | meaning or value list not confirmed |
| 168 | AnaSqOnVoice | used | beep when the FM squelch closes |
| 169 | DateDisFormat | not done | meaning or value list not confirmed |
| 170 | AnaMic | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 171 | ZoneBarsEn | not done | meaning or value list not confirmed |
| 172 | VcallRpheader | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 173 | DcsSte | not done | no such hardware, or not traced: as STE_Type |
| 174 | TxNr | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 175 | RxNr | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 176 | Drc | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 177 | DmrRasEn | not possible | belongs to a feature the cfw does not have: RAS |
| 178 | NightMode | not done | meaning or value list not confirmed |
| 179 | SateLocalKind | not possible | belongs to a feature the cfw does not have: satellite |
| 180 | SateTxPower | not possible | belongs to a feature the cfw does not have: satellite |
| 181 | SateAnaSql | not possible | belongs to a feature the cfw does not have: satellite |
| 182 | SateAosLimit | not possible | belongs to a feature the cfw does not have: satellite |
| 183 | NlightStartHour | not done | meaning or value list not confirmed: night mode times |
| 184 | NlightStartMin | not done | meaning or value list not confirmed: night mode times |
| 185 | NlightEndHour | not done | meaning or value list not confirmed: night mode times |
| 186 | NlightEndMin | not done | meaning or value list not confirmed: night mode times |
| 187 | SateFixPointName1 | not possible | belongs to a feature the cfw does not have: satellite |
| 188 | SateFixPointName2 | not possible | belongs to a feature the cfw does not have: satellite |
| 189 | SateFixPointName3 | not possible | belongs to a feature the cfw does not have: satellite |
| 190 | SateFixPointName4 | not possible | belongs to a feature the cfw does not have: satellite |
| 191 | SateFixPointName5 | not possible | belongs to a feature the cfw does not have: satellite |
| 192 | SateFixPointName6 | not possible | belongs to a feature the cfw does not have: satellite |
| 193 | SateFixPointName7 | not possible | belongs to a feature the cfw does not have: satellite |
| 194 | SateFixPointName8 | not possible | belongs to a feature the cfw does not have: satellite |

## D168UV, channel columns the cfw does not model itself

| # | Column | State | What / why |
|---|---|---|---|
| 11 | Radio ID | used | other session's section (signalling, keys, Radio ID) |
| 12 | Busy Lock/TX Permit | used | any value but Always |
| 13 | Squelch Mode | used | CTCSS / DCS decoder (FM) |
| 14 | Optional Signal | used | other session's section (signalling, keys, Radio ID) |
| 15 | DTMF ID | used | other session's section (signalling, keys, Radio ID) |
| 16 | 2Tone ID | used | other session's section (signalling, keys, Radio ID) |
| 17 | 5Tone ID | used | other session's section (signalling, keys, Radio ID) |
| 18 | PTT ID | used | other session's section (signalling, keys, Radio ID) |
| 23 | PTT Prohibit | used |  |
| 24 | Reverse | used |  |
| 25 | Idle TX | not possible | would transmit on its own |
| 26 | Slot Suit | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |
| 27 | AES Digital Encryption | used | other session's section (signalling, keys, Radio ID) |
| 28 | Digital Encryption | used | other session's section (signalling, keys, Radio ID) |
| 29 | Call Confirmation | not possible | would transmit on its own |
| 31 | Work Alone | used | warning, then the emergency alarm |
| 32 | Custom CTCSS | not done | the cfw has its own setting for this: the cfw takes any tone in the channel's CTCSS fields |
| 33 | 2TONE Decode | used | other session's section (signalling, keys, Radio ID) |
| 34 | Ranging | not possible | would transmit on its own |
| 35 | Through Mode | not done | meaning or value list not confirmed |
| 36 | APRS RX | not possible | belongs to a feature the cfw does not have: DMR APRS receive |
| 37 | Analog APRS PTT Mode | used | End: beacon after PTT (not in HW=1 builds) |
| 38 | Digital APRS PTT Mode | not possible | belongs to a feature the cfw does not have: DMR APRS |
| 39 | APRS Report Type | used | Analog: for the line above |
| 40 | Digital APRS Report Channel | not possible | belongs to a feature the cfw does not have: DMR APRS |
| 41 | Correct Frequency[Hz] | used |  |
| 42 | SMS Confirmation | not possible | would transmit on its own |
| 43 | Exclude channel from roaming | used |  |
| 44 | DMR MODE | used | 2 / 3 = TDMA direct |
| 45 | DataACK Disable | not possible | belongs to a feature the cfw does not have: the cfw sends no data ACK |
| 46 | R5ToneBot | used | other session's section (signalling, keys, Radio ID) |
| 47 | R5ToneEot | used | other session's section (signalling, keys, Radio ID) |
| 48 | Auto Scan | used | starts the channel's scan list |
| 49 | Ana Aprs Mute | not done | meaning or value list not confirmed |
| 50 | Send Talker Aias | used | DMR alias per channel |
| 51 | AnaAprsTxPath | not done | meaning or value list not confirmed |
| 52 | ARC4 | used | other session's section (signalling, keys, Radio ID) |
| 53 | ex_emg_kind | not possible | belongs to a feature the cfw does not have: emergency |
| 54 | idle_tx | not possible | would transmit on its own |
| 55 | Compand | not possible | done in the SCT3288 baseband / NR board audio path, not by the MCU |
| 56 | DisturEn | not possible | no such hardware, or not traced: clock shift |
| 57 | DisturFreq | not possible | no such hardware, or not traced: clock shift |
| 58 | Rpga_Mdc | used | other session's section (signalling, keys, Radio ID) |
| 59 | dmr_crc_ignore | not done | meaning or value list not confirmed |
| 60 | txcc | not done | belongs to a feature the cfw does not have: DMR library option (dmr session) |

## D168UV, Alarm Setting (.rdt-only dialog, table 64)

| # | Column | State | What / why |
|---|---|---|---|
| 0 | Analog Emergency Alarm | used | type of the alarm on non-DMR channels (transmit part not in HW=1 builds) |
| 1 | Analog ENI Type | not done | meaning or value list not confirmed: list texts and the Emergency ID are not decoded |
| 2 | Analog Alarm Time [s] | used | local alarm time |
| 3 | Analog Duration of TX [s] | used | TX time (transmit part not in HW=1 builds) |
| 4 | Analog Duration of RX [s] | used | pause (transmit part not in HW=1 builds) |
| 5 | Analog Emergency Channel | used | alarm channel (transmit part not in HW=1 builds) |
| 6 | Analog ENI Send Select | used | assigned / selected channel (transmit part not in HW=1 builds) |
| 7 | Analog Emergency Cycle (0 = Continuous) | used | number of transmissions (transmit part not in HW=1 builds) |
| 8 | Digital Emergency Alarm | used | type of the alarm on DMR channels (transmit part not in HW=1 builds) |
| 9 | Digital Emergency Channel | used | alarm channel (transmit part not in HW=1 builds) |
| 10 | Digital Alarm Time [s] | used | local alarm time |
| 11 | Digital Duration of TX [s] | used | TX time (transmit part not in HW=1 builds) |
| 12 | Digital Duration of RX [s] | used | pause (transmit part not in HW=1 builds) |
| 13 | Digital ENI Send Select | used | assigned / selected channel (transmit part not in HW=1 builds) |
| 14 | Digital Emergency Cycle (0 = Continuous) | used | number of transmissions (transmit part not in HW=1 builds) |
| 15 | Digital TG/DMR ID | used | target of the alarm calls (transmit part not in HW=1 builds) |
| 16 | Digital Call Type | used | private / group / all (transmit part not in HW=1 builds) |
| 17 | Receive Alarm | used | popup + alert on a received DMR emergency call |
| 18 | QDC1200 Kind | not done | meaning or value list not confirmed: how the stock radio combines it with the emergency packet |
| 19 | QDC1200 Group ID | not done | meaning or value list not confirmed: as AlarmQKind |
| 20 | QDC1200 Private ID | not done | meaning or value list not confirmed: as AlarmQKind |
| 21 | Analog alarm signaling type | used | MDC1200: emergency packet opens the FM alarm (transmit part not in HW=1 builds); DTMF / 5TONE need the undecoded Emergency ID |
| 22 | Work Alone Response Time (0 = 1 min) | used | response time |
| 23 | Work Alone Warning Time (0 = 1 s) | used | warning time |
| 24 | Work Alone Response | used | key / PTT response |

## D168UV, Talk Alias / AIR Alias (.rdt-only dialog, table 70)

| # | Column | State | What / why |
|---|---|---|---|
| 0 | Talker Alias Type | not done | meaning or value list not confirmed: the stock CPS shows no text field for Custom Text; treated as Radio Alias |
| 1 | Alias Display Priority | used | off / contact first / alias first |

## D168UV, Friends List (.rdt-only dialog, table 71)

| # | Column | State | What / why |
|---|---|---|---|
| 0 | No. | used | list order |
| 1 | DMR ID | used | Radio extras > Friends: private call target |

