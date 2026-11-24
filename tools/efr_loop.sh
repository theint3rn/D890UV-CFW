#!/bin/sh
# Host loopback test of NXDN96 EFR voice through the proto glue (tools/efr_loop.c). Exit 0 = pass.
set -e
C=$(cd "$(dirname "$0")/.." && pwd); P="$C/proto/dmr_nxdn"; B="$C/build_streams"; mkdir -p "$B"
cc -std=gnu11 -O1 -Wall -DCFW_HOST -I"$C/hal" -I"$C/dsp" -I"$C/voc" -I"$C/voc/imbe" -I"$C/app/protos" \
   -I"$P/common" -I"$P/dmr" -I"$P/nxdn" "$C/tools/efr_loop.c" "$C/app/protos/proto_nxdn.c" "$C/app/protos/crypt_weak.c" \
   "$P"/common/*.c "$P"/dmr/*.c "$P"/nxdn/*.c "$C"/voc/imbe/imbe_enc.c "$C"/voc/imbe/imbe_dec.c \
   "$C"/voc/imbe/imbe_tables.c -lm -o "$B/efr_loop"
"$B/efr_loop"
