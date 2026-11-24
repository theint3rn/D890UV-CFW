#!/usr/bin/env python3
"""Write cfw/docs/STOCK_COLUMNS.md: every Optional Settings column and every stock channel column of both radios with
its state (used / not possible / not done) and one reason; --check compares with cfw_cps/schema/cfw_support.json,
--set updates it through cfw_cps/tools/make_cfw_support.py (one change at a time).

A column that another session owns in the channel list (signalling, keys, Radio ID) is taken from cfw_support.json."""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CPS = os.path.abspath(os.path.join(HERE, "..", "..", "cfw_cps"))

R = {"hw": "no such hardware, or not traced", "bb": "done in the SCT3288 baseband / NR board audio path, not by the MCU",
     "vfo": "one receiver in the cfw: no second (B) channel", "feat": "belongs to a feature the cfw does not have",
     "unk": "meaning or value list not confirmed", "res": "reserved / no effect in the stock CPS",
     "cfw": "the cfw has its own setting for this", "auto": "would transmit on its own"}
USED = "used"
# column -> (state, reason); U = used (text: what it does), P = not possible, N = not done
U = lambda t: ("used", t)            # noqa: E731
P = lambda k, t="": ("not possible", R[k] + (": " + t if t else ""))   # noqa: E731
N = lambda k, t="": ("not done", R[k] + (": " + t if t else ""))       # noqa: E731


def hooked(name):
    return name in open(os.path.join(HERE, "..", "app", "protos", "proto_dmr.c")).read()


OPT = {
 "Beep": U("key beep"), "KeyLock": U("auto key lock"), "AutoPowerOff": U("auto power off"), "TOT": U("TX timeout"),
 "StartDspSet": U("Character = boot text"), "Password": U("power-on password"), "Password_Char": U("power-on password (digits)"),
 "Step": U("VFO step (V1.05 table, 2.5K first); absent: the cfw Radio Settings step, which is always stored"),
 "SQL1": U("squelch level"), "PowerSave": U("RX duty cycle"), "VOX": U("VOX"), "VOX_Delay": U("0.5-3.0 s VOX hang"),
 "ScanType": U("VFO scan TO / CO / SE"), "Lightness": U("backlight"), "AutoBKLightTime": U("backlight time"),
 "Gps": U("GPS on, clock from GPS"), "MsgRing": U("SMS beep"), "FmMon": U("busy channel pauses the broadcast"),
 "TBST": U("FM: Up key while PTT"), "CallRing": U("private call beep"), "TmZone": U("GPS clock"),
 "Start_Char": U("boot text"), "Start_Char2": U("boot text"), "SqOnVoice": U("beep after a digital call (3 pitches, cfw's own)"),
 "MissCallFilter": U("own ID not logged"), "Boot_Sound": U("startup beep"), "MaxVol": U("volume limit"),
 "AutoRepeater": U("VFO auto repeater"), "UhfAutoRep": U("offset entry"), "VhfAutoRep": U("offset entry"),
 "UhfAutoRep2": U("second range"), "VhfAutoRep2": U("second range"), "AutoRepFreq0": U("range"), "AutoRepFreq1": U("range"),
 "AutoRepFreq2": U("range"), "AutoRepFreq3": U("range"), "AutoRepMinFreqV2": U("second range"), "AutoRepMaxFreqV2": U("second range"),
 "AutoRepMinFreqU2": U("second range"), "autoRepMaxFreqU2": U("second range"), "FailAlarm": U("man down"),
 "ManDownWait": U("man down wait"), "DateDisKind": U("clock in the status bar"), "VfoScanFreq0": U("UHF VFO scan start"),
 "VfoScanFreq1": U("UHF VFO scan end"), "VfoScanFreq2": U("VHF VFO scan start"), "VfoScanFreq3": U("VHF VFO scan end"),
 "WanderPerod": U("auto roaming period"), "FixRomanStartOp": U("Fixed Time only; Out of Range not done"),
 "TimeRoamOn": U("auto roaming"), "CurRoamZone": U("roaming zone"), "StartChUse": U("startup channel"),
 "StartZone1": U("startup channel"), "StartCurChan1": U("startup channel"), "MenuLowPower": U("menu exit time"),
 "ChanNameColour": U("channel name colour"), "WorkCharDisColour": U("frequency colour"), "WxAlarmSign": U("NOAA 1050 Hz alert"),
 "TotPreEn": U("warning before TX timeout"), "AnaSqOnVoice": U("beep when the FM squelch closes"), "AmOffset": U("AM down-converter side"),
 "FM_En": U("broadcast receiver at power on"), "Work_FMCH": U("FM memory at power on"),
 "BlueToothOn": U("module probed and set up at power on (no BT audio)"), "BhtMicGain": U("module gain command"), "BhtSpkGain": U("module gain command"),
 # not possible
 "SQL2": P("vfo"), "Work_Zone2": P("vfo"), "MainState": P("vfo"), "SubMode": P("vfo"), "TalkOverPoint": P("vfo"),
 "AutoRepeaterB": P("vfo"), "PriZoneB": P("vfo"), "StartZone2": P("vfo"), "StartCurChan2": P("vfo"), "DiviDisEn": P("vfo"),
 "ChanNameColourB": P("vfo"), "ZoneNameColourB": P("vfo"), "AnaHoldTime": P("vfo", "hold of the dual watch"),
 "MicLevel": P("bb"), "AnaMic": P("bb"), "NxdnMic": P("bb"), "EnSoundEffect": P("bb"), "Drc": P("bb"), "RxNr": P("bb"), "TxNr": P("bb"),
 "EarMaxVol": P("bb", "headphone volume"), "VoxHeadset": P("bb", "detector source"), "BtNrBefore": P("bb"), "BtNrAfter": P("bb"),
 "TalkTips": P("hw", "the MCU beep uses the same audio output as the transmitted signal"),
 "MicInBlueTooth": P("hw", "audio route to the BT module"), "SpkInBlueTooth": P("hw", "audio route to the BT module"),
 "BhtHoldTime": P("feat", "BT audio"), "BhtHoldDelay": P("feat", "BT audio"), "BhtPttHold": P("feat", "BT PTT"), "PttSleepTime": P("feat", "BT PTT"),
 "Record_En": P("feat", "recorder"), "RecordDelay": P("feat", "recorder"), "WtRecordNote": P("feat", "recorder"),
 "Language": P("feat", "English only"), "bkpic": P("feat", "background picture"), "Voice_Note": P("feat", "voice prompts"),
 "SateLocalKind": P("feat", "satellite"), "SateTxPower": P("feat", "satellite"), "SateAnaSql": P("feat", "satellite"), "SateAosLimit": P("feat", "satellite"),
 "MeasurePeriod": P("auto", "ranging"), "GpsReplyEn": P("auto"), "BsModeCheck": P("auto", "repeater check"), "TimeBsCheck": P("auto", "repeater check"),
 "BsCheckTimes": P("auto", "repeater check"), "OutRepNote": P("auto", "repeater check"), "OutNoteTimes": P("auto", "repeater check"),
 "DigiStunKillEn": P("feat", "remote kill (kept out on purpose)"), "RemoteMoniEn": P("auto", "remote monitor"),
 "DigiEmgKind": P("feat", "emergency call"), "RepMode": P("feat", "radio as repeater"), "RepCcLimit": P("feat", "radio as repeater"),
 "RepSlotPathA": P("feat", "radio as repeater"), "RepSlotPathB": P("feat", "radio as repeater"), "DmrRasEn": P("feat", "RAS"),
 "StartResetEn": P("feat", "reset key combination"), "CurUhfAutoRep": P("vfo", "stock run-time copy"), "CurVhfAutoRep": P("vfo", "stock run-time copy"),
 "Reserved_Start_Char": P("res"), "Reserved_EMG_Kind": P("res"), "Reserved_DigiKillEn": P("res"), "AprsUpDate": P("res"), "AprsTxFreq": P("res"),
 "GroupCallS_Ext": P("res"), "Contact_Compression": P("res"), "RX_Contact_Compression": P("res"), "File_Optimization": P("res"), "RMV1": P("res"),
 "Scan_Pri_Current": P("res"), "comVersion": P("res"), "MinVolData": P("hw", "analog volume knob"), "VolType": P("hw", "analog volume knob"),
 # not done
 "SmsFormat": N("cfw", "Settings > SMS format"), "FreqDis": N("cfw", "the main screen always shows both"), "Work_Zone1": N("cfw", "zone remembered by the radio"),
 "STE_Type": N("hw", "CTCSS tail is made in the RF chip; its phase control is not decoded"), "STE_Freq": N("hw", "as STE_Type"),
 "SteTime": N("hw", "as STE_Type"), "DcsSte": N("hw", "as STE_Type"), "CurWxChan": N("unk", "channel order of the list"),
 "PreWave": N("feat", "DMR library option (dmr session)"), "Preamble": N("feat", "DMR library option (dmr session)"), "VcallRpheader": N("feat", "DMR library option (dmr session)"),
 "DigiMoni": N("feat", "DMR library option (dmr session)"), "DigiMoniCc": N("feat", "DMR library option (dmr session)"), "DigiMoniId": N("feat", "DMR library option (dmr session)"),
 "AmSqLevel": N("hw", "AM squelch: no level scale for the down-converter"), "ApoKind": N("unk"), "NightMode": N("unk"),
}
UNK = N("unk")
CH = {"Squelch Mode": U("CTCSS / DCS decoder (FM)"), "Send Talker Alias DMR/NX": U("DMR alias per channel"), "Send Talker Aias": U("DMR alias per channel"),
      "Auto Scan": U("starts the channel's scan list"), "DMR MODE": U("2 / 3 = TDMA direct"), "Analog APRS PTT Mode": U("End: beacon after PTT (not in HW=1 builds)"),
      "APRS Report Type": U("Analog: for the line above"), "Busy Lock/TX Permit": U("any value but Always"), "PTT Prohibit": U(""), "Reverse": U(""),
      "Correct Frequency[Hz]": U(""), "Exclude channel from roaming": U(""),
      "Work Alone": U("warning, then the emergency alarm"), "Ranging": P("auto"), "Idle TX": P("auto"), "idle_tx": P("auto"), "Call Confirmation": P("auto"),
      "SMS Confirmation": P("auto"), "DataACK Disable": P("feat", "the cfw sends no data ACK"), "Digital Duplex": P("vfo"),
      "Digital APRS PTT Mode": P("feat", "DMR APRS"), "Digital APRS Report Channel": P("feat", "DMR APRS"), "APRS RX": P("feat", "DMR APRS receive"),
      "BtRxState": P("hw", "BT audio"), "compand": P("bb"), "Compand": P("bb"), "DisturEn": P("hw", "clock shift"), "DisturFreq": P("hw", "clock shift"),
      "ex_emg_kind": P("feat", "emergency"), "Custom CTCSS": N("cfw", "the cfw takes any tone in the channel's CTCSS fields"),
      "Slot Suit": N("feat", "DMR library option (dmr session)"), "txcc": N("feat", "DMR library option (dmr session)"), "Through Mode": N("unk")}

TXN = " (transmit part not in HW=1 builds)"
DLG = {"AlarmSetting": {
      "AlarmAEmergency": U("type of the alarm on non-DMR channels" + TXN), "AlarmATime": U("local alarm time"),
      "AlarmATxTime": U("TX time" + TXN), "AlarmARxTime": U("pause" + TXN), "AlarmAChannel": U("alarm channel" + TXN),
      "AlarmAEniSend": U("assigned / selected channel" + TXN), "AlarmACycle": U("number of transmissions" + TXN),
      "AlarmAEniType": N("unk", "list texts and the Emergency ID are not decoded"),
      "AlarmDEmergency": U("type of the alarm on DMR channels" + TXN), "AlarmDChannel": U("alarm channel" + TXN),
      "AlarmDTime": U("local alarm time"), "AlarmDTxTime": U("TX time" + TXN), "AlarmDRxTime": U("pause" + TXN),
      "AlarmDEniSend": U("assigned / selected channel" + TXN), "AlarmDCycle": U("number of transmissions" + TXN),
      "AlarmDId": U("target of the alarm calls" + TXN), "AlarmDCallType": U("private / group / all" + TXN),
      "AlarmDReceive": U("popup + alert on a received DMR emergency call") if hooked("dmr_rx_emergency") else N("feat", "needs a hook in the DMR glue"),
      "AlarmQKind": N("unk", "how the stock radio combines it with the emergency packet"), "AlarmQGroup": N("unk", "as AlarmQKind"),
      "AlarmQPrivate": N("unk", "as AlarmQKind"),
      "AlarmSigType": U("MDC1200: emergency packet opens the FM alarm" + TXN + "; DTMF / 5TONE need the undecoded Emergency ID"),
      "AlarmWResponse": U("response time"), "AlarmWWarning": U("warning time"), "AlarmWMode": U("key / PTT response")},
  "TalkAlias": {"AliasPrio": U("off / contact first / alias first"), "AliasType": N("unk", "the stock CPS shows no text field for Custom Text; treated as Radio Alias"),
      "AliasFormat": U("TX alias form") if hooked("stockdlg_alias_format") else N("feat", "needs a change in the DMR glue (dmr session)")},
  "FriendsList": {"FriendNo": U("list order"), "FriendId": U("Radio extras > Friends: private call target")}}
NODE = {"AlarmSetting": "@alarm", "TalkAlias": "@talkalias", "FriendsList": "@friends"}


def main():
    sup = json.load(open(os.path.join(CPS, "schema", "cfw_support.json")))
    out, bad, counts = ["# Stock columns and the cfw", "", "Written by `cfw/tools/stock_columns.py`; do not edit. State: **used** = the cfw "
                        "acts on it, **not possible** = the hardware or the cfw cannot, **not done** = could be done. Column "
                        "numbers are per radio (they differ from 173 on).", "", ""], [], {}
    models = ("D890UV", "D168UV")
    sch = {m: json.load(open(os.path.join(CPS, "schema", "stock_%s.json" % m.lower()))) for m in models}
    for m in models:
        for t, table in (("OptionalSetting", OPT), ("Channel", CH)):
            T = sch[m]["tables"][t]
            out += ["## %s, %s" % (m, "Optional Settings" if t == "OptionalSetting" else "channel columns the cfw does not model itself"), "",
                    "| # | Column | State | What / why |", "|---|---|---|---|"]
            for i, c in enumerate(T["columns"]):
                if t == "Channel" and c in T.get("cfw_mapped", []):
                    continue
                cur = sup["keys"].get(t, {}).get(c, {}).get(m, sup["sections"][t][m])
                if c.startswith(("NlightS", "NlightE")):
                    st = N("unk", "night mode times")
                elif c.startswith("SateFix"):
                    st = P("feat", "satellite")
                elif c in table:
                    st = table[c]
                elif t == "Channel" and cur == "used":
                    st = U("other session's section (signalling, keys, Radio ID)")
                else:
                    st = UNK
                counts.setdefault((m, t), {}).setdefault(st[0], []).append(c)
                want = "used" if st[0] == "used" else "stored"
                if cur != want:
                    bad.append((t, c, m, want, cur))
                out.append("| %d | %s | %s | %s |" % (i, c, st[0], st[1].replace("|", "/")))
            out.append("")
        dl = json.load(open(os.path.join(CPS, "schema", "rdt_dialogs.json")))["tables"]
        for t, table in DLG.items():
            out += ["## %s, %s (.rdt-only dialog, table %d)" % (m, dl[t]["title"], dl[t]["id"]), "", "| # | Column | State | What / why |", "|---|---|---|---|"]
            for i, f in enumerate(dl[t]["fields"]):
                c = f["name"]
                if m not in f:
                    continue
                cur = sup["keys"].get(t, {}).get(c, {}).get(m, sup["sections"].get(NODE[t], {}).get(m, "stored"))
                if cur == "n/a":
                    continue
                st = table[c]
                counts.setdefault((m, t), {}).setdefault(st[0], []).append(c)
                want = "used" if st[0] == "used" else "stored"
                if cur != want:
                    bad.append((NODE[t] if dl[t]["kind"] == "list" else t, None if dl[t]["kind"] == "list" else c, m, want, cur))
                out.append("| %d | %s | %s | %s |" % (i, f.get("label", c), st[0], st[1].replace("|", "/")))
            out.append("")
    hd = ["| Radio | Table | used | not possible | not done |", "|---|---|---|---|---|"]
    for (m, t), d in counts.items():
        hd.append("| %s | %s | %d | %d | %d |" % (m, t, len(d.get("used", [])), len(d.get("not possible", [])), len(d.get("not done", []))))
    out[4:4] = hd
    open(os.path.join(HERE, "..", "docs", "STOCK_COLUMNS.md"), "w").write("\n".join(out) + "\n")
    print("\n".join(hd))
    for t, c, m, want, cur in bad:
        what = "%s:%s" % (t, c) if c else t
        print("support.json %s %s is %s, want %s" % (what, m, cur, want))
        if "--set" in sys.argv:
            subprocess.check_call([sys.executable, "tools/make_cfw_support.py", "--set", what, m, want], cwd=CPS, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
