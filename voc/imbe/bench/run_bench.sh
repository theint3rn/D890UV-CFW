#!/bin/sh
# Instruction count per 20 ms frame of the IMBE encoder/decoder on Cortex-M33 (Renode instruction count;
# M33 cycles ~ 1.1-1.5 x instructions with FPU code). Usage: bench/run_bench.sh   (from voc/imbe)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../build/bench}
RENODE=${RENODE:-$HOME/.local/opt/renode_1.17.0-portable/renode}
mkdir -p "$OUT"
CF="-std=c11 -O2 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fsingle-precision-constant -fno-math-errno -ffunction-sections -fdata-sections -I$HERE/.."
for m in 0 1 2 3; do
  arm-none-eabi-gcc $CF -DMODE=$m -nostartfiles -T"$HERE/bench.ld" -Wl,--gc-sections --specs=nano.specs \
    "$HERE/bench.c" "$HERE/../imbe_enc.c" "$HERE/../imbe_dec.c" "$HERE/../imbe_tables.c" -lm -o "$OUT/bench$m.elf"
  GD=$(arm-none-eabi-nm "$OUT/bench$m.elf" | awk '/ g_done$/ {print "0x"$1}')
  cat > "$OUT/b$m.resc" <<EOR
include @$HERE/bench_dump.py
mach create
machine LoadPlatformDescription @$HERE/bench.repl
sysbus LoadELF @$OUT/bench$m.elf
emulation RunFor "00:00:20"
bench_dump @$OUT/count$m.txt $GD
quit
EOR
  timeout 300 "$RENODE" --disable-xwt --console -e "include @$OUT/b$m.resc" >"$OUT/renode$m.log" 2>&1 || true
done
python3 - "$OUT" <<'EOP'
import sys
d = sys.argv[1]
c = [list(map(int, open('%s/count%d.txt' % (d, m)).read().split())) for m in range(4)]
assert all(x[1] == 1 for x in c), c
enc = (c[1][0] - c[0][0]) / 50.0
dec = (c[2][0] - c[1][0]) / 50.0
print("instructions per frame: encode %.0f, decode %.0f (one 512-point FFT: %.0f)" % (enc, dec, (c[3][0] - c[0][0]) / 50.0))
print("at 120 MHz, CPI 1.0..1.5: encode %.1f..%.1f %%, decode %.1f..%.1f %% of real time"
      % (enc / 2.4e4, enc * 1.5 / 2.4e4, dec / 2.4e4, dec * 1.5 / 2.4e4))
EOP
