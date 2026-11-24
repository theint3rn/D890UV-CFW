#!/bin/sh
# Emulator check of every decoder (make EMU=1 build): select a built-in channel by number, play its test
# stream into the symbol FIFO, screenshot after 2.5 s and at the end. Montage per mode in emu/out/cfw_<mode>/.
# Usage: cfw/tools/emu_modes.sh [FIRST_BUILTIN_CHANNEL_NUMBER]   (Info screen: codeplug ch count + 1)
#        SAMPLES=1 cfw/tools/emu_modes.sh   -> play the 48 kHz discriminator sample files (*.adc8) instead of
#        symbols: the MCU-DSP demodulators run inside the firmware (out dirs cfw_s_<mode>)
set -e
C=$(cd "$(dirname "$0")/.." && pwd); S="$C/build_streams"; cd "$C/.."
B=${1:-$(python3 -c "import sys; sys.path.insert(0, \"emu\"); import run; print(run.first_builtin())")}
keys() { n=$1; out=""; for d in $(echo "$n" | sed 's/./& /g'); do out="$out key:$((d + 1))"; done; echo "$out key:12"; }
run() { # name channel_offset stream rate seconds
  f=$3; r=$4; n="cfw_$1"
  if [ -n "$SAMPLES" ]; then f="${3%.sym}.adc8"; r=48000; n="cfw_s_$1"; fi
  python3 cfw/tools/emutest.py "$n" --elf cfw/build_emu/cfw.elf $(keys $(($B + $2))) \
    "0|syms Load @$S/$f $r" wait:2.5 wait:$5 | tail -1
}
run dmr     5 dmr_call.sym   4800 1.5
run dmr_t3  6 dmr_t3.sym     4800 6
run p25     7 p25_call.sym   4800 1.5
run p25trk  8 p25_trunk.sym  4800 5
run p25p2   9 p25p2_call.sym 6000 2
run dstar  10 dstar_call.sym 4800 2
run nxdn   11 nxdn_call.sym  2400 2.5
run ysf    12 ysf_call.sym   4800 6.5
