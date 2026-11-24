#!/bin/sh
# Host check of the proto glue: tools/host_decode.sh MODE STREAM [trunk 0/1] [dgid]   (MODE: dmr nxdn p25 p25p2 dstar ysf)
set -e
C=$(cd "$(dirname "$0")/.." && pwd); P="$C/proto"; B="$C/build_streams"; mkdir -p "$B"
if [ ! -x "$B/host_decode" ] || [ -n "$REBUILD" ]; then
cc -std=gnu11 -O1 -w -DCFW_HOST -I"$C/hal" -I"$C/dsp" -I"$C/voc" -I"$C/app/protos" -I"$P/dmr_nxdn/common" -I"$P/dmr_nxdn/dmr" -I"$P/dmr_nxdn/nxdn" \
   -I"$P/p25/include" -I"$P/dstar_ysf/common" -I"$P/dstar_ysf/dstar" -I"$P/dstar_ysf/ysf" "$C/tools/host_decode.c" \
   "$C"/app/protos/*.c "$P"/dmr_nxdn/common/*.c "$P"/dmr_nxdn/dmr/*.c "$P"/dmr_nxdn/nxdn/*.c "$P"/p25/src/*.c \
   "$P"/dstar_ysf/common/*.c "$P"/dstar_ysf/dstar/*.c "$P"/dstar_ysf/ysf/*.c -lm -o "$B/host_decode"
fi
"$B/host_decode" "$@"
