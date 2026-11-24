#!/bin/sh
# host test of hal/bb_allow.h (see bb_allow_test.c); dictionary: ../tools/sct3288/sct3288_commands.json if present
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
python3 - > "$T/cmds" <<'P'
import json, os
p = "../tools/sct3288/sct3288_commands.json"
if os.path.exists(p):
    for r in json.load(open(p)):
        print((r.get("write") or "").replace(" ", ""), r.get("name"))
P
gcc -Wall -I hal -I proto/dmr_nxdn/common tools/bb_allow_test.c proto/dmr_nxdn/common/ambe_fec.c -o "$T/t" && "$T/t" < "$T/cmds" | tail -4
"$T/t" < "$T/cmds" > /dev/null
R=$?
rm -rf "$T"
[ $R = 0 ] && echo "bb_allow: PASS" || echo "bb_allow: FAIL"
exit $R
