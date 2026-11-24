#!/bin/sh
# Build cfw/tools/gen_streams.c on the host against the cfw/proto libraries and write the emulator test
# streams to cfw/build_streams/ (play them with `syms Load @<file> <rate>` in a `make EMU=1` build).
set -e
C=$(cd "$(dirname "$0")/.." && pwd)
P="$C/proto"
B="$C/build_streams"
mkdir -p "$B"
cc -std=gnu11 -O2 -w \
   -I"$P/dmr_nxdn/common" -I"$P/dmr_nxdn/dmr" -I"$P/dmr_nxdn/nxdn" -I"$P/p25/include" \
   -I"$P/dstar_ysf/common" -I"$P/dstar_ysf/dstar" -I"$P/dstar_ysf/ysf" \
   "$C/tools/gen_streams.c" "$P"/dmr_nxdn/common/*.c "$P"/dmr_nxdn/dmr/*.c "$P"/dmr_nxdn/nxdn/*.c \
   "$P"/p25/src/*.c "$P"/dstar_ysf/common/*.c "$P"/dstar_ysf/dstar/*.c "$P"/dstar_ysf/ysf/*.c \
   -lm -o "$B/gen_streams"
"$B/gen_streams" "$B"
# discriminator sample files (48 kHz) for the MCU-DSP demodulators: *.adc16 (analog Stream) / *.adc8 (syms)
make -s -C "$C/dsp" waves
