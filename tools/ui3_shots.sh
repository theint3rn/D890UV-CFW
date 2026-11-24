#!/bin/sh
# UI tours for the README screenshots (screens/ui3_*.png), either radio, on made-up data only:
# cfw_cps/examples/synth.d8cp preloaded by ui_test.py (179 channels, so the built-ins start at 180) and the radio's
# emu/images/synth.pages (EMU_NO_ANYTONE=1). One emulator at a time.
#   cfw/tools/ui3_shots.sh [d890|d168] [TOUR...]      TOUR: zcc scan calllog dial settings pf3 picker
#   persist (default: all; pf3 picker persist D890UV only)
# Out: <emu>/out/ui3_<tour>_<radio>/montage.png, copied to the radio's cfw/screens/ui3_<name>.png.
set -e
C=$(cd "$(dirname "$0")/.." && pwd); cd "$C/.."
R=${1:-d890}; [ $# -gt 0 ] && shift
TOURS=${*:-zcc scan calllog dial settings $( [ "$R" = d890 ] && echo pf3 picker persist || true)}
export EMU_NO_ANYTONE=1
if [ "$R" = d890 ]; then
  EMU="$C/../emu"; SCR="$C/screens"; EMUELF="$C/build_emu/cfw.elf"
else
  EMU="$C/../../../D168UV/emu"; SCR="$C/../../../D168UV/cfw/screens"; EMUELF="$C/../../../D168UV/cfw/build_emu/cfw168.elf"
fi
STREAM="$C/../emu/streams/dmr_call.dibits"
B=180                                    # first built-in channel with synth.d8cp
ch() { n=$1; out=""; for d in $(echo "$n" | sed 's/./& /g'); do out="$out key:$((d + 1))"; done; echo "$out key:12"; }
t() { # name screens-file ui_test args...
  n=$1; f=$2; shift 2
  python3 cfw/tools/ui_test.py "ui3_${n}_$R" --radio "$R" "$@" | tail -1
  case $f in ../*) d="$SCR/${f#../}.png" ;; *) d="$SCR/ui3_$f.png" ;; esac
  cp "$EMU/out/ui3_${n}_$R/montage.png" "$d"
}
for tour in $TOURS; do case $tour in
zcc)   # Menu > Zone list > pick "FM Repeaters" > Channels list > pick 044 > Contacts
  t zcc zones_channels_contacts key:13 key:13 key:16x2 key:13 wait:1.5 key:13 key:16 key:13 key:16x3 key:13 \
    key:13 key:16x2 key:13 ;;
scan)  # zone FM Repeaters, Menu > Scan > Start; carrier on 145.300 (044) stops it, * = nuisance delete, scan goes
       # on; carrier off; Scan menu shows the nuisance count; Menu > Monitor on (squelch open), then off
  t scan scan key:13 key:13 key:16x2 key:13 wait:1 key:13 key:16x6 key:13 key:13 wait:1.5 \
    "0|rf0 Signal 145300000 -70" wait:9 key:11 wait:2 "0|rf0 SignalOff" wait:1 key:13 key:13 wait:1 \
    key:13 key:16x7 key:13 wait:1 ;;
calllog) # EMU=1 build: built-in DMR TG91 channel, DMR call from 9990201 played into the symbol FIFO; Menu > Call log,
         # select the entry = call back (TX target TG 91)
  t calllog calllog --elf "$EMUELF" $(ch $((B + 5))) "0|syms Load @$STREAM 4800" wait:1.5 wait:3 \
    key:13 key:16x4 key:13 key:13 wait:1 ;;
dial)  # built-in DMR simplex: Menu > Manual dial 9990202, # = private, OK; Contacts > pick a group = TX target
  t dial dial_contacts $(ch $((B + 4))) key:13 key:16x3 key:13 key:10 key:10 key:10 key:1 key:3 key:1 key:3 \
    key:12 key:13 wait:1 key:13 key:16x2 key:13 key:16 key:13 wait:1 ;;
settings) # Menu > Settings: TX timeout 30 s, backlight 5, key lock Auto; main screen: lock after 10 s, long * unlocks;
          # PTT held past 30 s: TX TIMEOUT
  t settings settings_effects key:13 key:16x8 key:13 key:16 key:13 key:15x2 key:13 key:16x4 key:13 key:16x4 key:13 \
    key:16x4 key:13 key:16 key:13 key:14 key:14 wait:11 long:11 pttdown wait:1 wait:30 pttup wait:1 ;;
pf3)   # D890UV: PF3 cycles the mode of codeplug channel 001 (FM>DMR>P25>P25P2>DSTAR>NXDN>YSF>FM)
  t pf3 ../mode_switch_pf3 pf3 pf3 pf3 pf3 pf3 pf3 pf3 wait:1 ;;
picker) # built-in DMR simplex: Menu > Channel settings, colour code 4, talkgroup 999; Menu > Mode list, pick DSTAR
  t picker ../menu_settings_picker $(ch $((B + 4))) key:13 key:16x10 key:13 key:16 key:5 key:16x2 key:11x2 \
    key:10x3 key:14 key:14 wait:1 key:13 key:16x9 key:13 key:16x3 key:13 wait:1 ;;
persist) # built-in DMR simplex: Channel settings talkgroup 123, Menu > Save now, power off (board PowerOff), power on:
          # the same channel with TG 123 comes back
  t persist ../settings_persist $(ch $((B + 4))) key:13 key:16x10 key:13 key:16x3 key:11x3 key:2 key:3 key:4 \
    key:14 key:14 key:13 key:15x2 key:13 wait:1 "0|board PowerOff" wait:3 "0|machine Reset" "0|board PowerOn" wait:6 ;;
*) echo "unknown tour $tour"; exit 1 ;;
esac; done
