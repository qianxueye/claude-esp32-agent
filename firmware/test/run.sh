#!/bin/sh
# Compiles and runs every host test with the pure firmware modules.
set -eu
cd "$(dirname "$0")"
SRCS="../main/proto.c ../main/dpower.c ../main/keydebounce.c ../main/volknob.c ../main/volarc.c ../main/arcvis.c ../main/volstate.c ../main/netcfg.c ../main/linkstate.c"
[ -f ../main/motion.c ] && SRCS="$SRCS ../main/motion.c"
OUT="${TMPDIR:-/tmp}/clawd_host_tests.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
for t in *_test.c; do
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I../main "$t" $SRCS -lm -o "$OUT/${t%.c}"
    echo "== $t"
    "$OUT/${t%.c}"
done
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I../main -DVOL_ARC_R_OUT_PX=225 volarc_test.c $SRCS -lm -o "$OUT/volarc_test_225"
echo "== volarc_test.c (VOL_ARC_R_OUT_PX=225)"
"$OUT/volarc_test_225"
