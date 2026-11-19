#!/bin/sh
# Compiles and runs every host test with the pure firmware modules.
set -eu
cd "$(dirname "$0")"
SRCS="../main/proto.c ../main/dpower.c ../main/keydebounce.c ../main/volknob.c ../main/volarc.c ../main/arcvis.c ../main/volstate.c ../main/brtmap.c"
[ -f ../main/motion.c ] && SRCS="$SRCS ../main/motion.c"
SRCS="$SRCS ../main/apack.c ../main/aplay.c ../main/aglue.c ../main/rotblit.c"
OUT="${TMPDIR:-/tmp}/clawd_host_tests.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
for t in *_test.c; do
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -DVOLARC_COUNT_TRIG -I../main "$t" $SRCS -lm -o "$OUT/${t%.c}"
    echo "== $t"
    "$OUT/${t%.c}"
done
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -DVOLARC_COUNT_TRIG -I../main -DVOL_ARC_R_OUT_PX=225 volarc_test.c $SRCS -lm -o "$OUT/volarc_test_225"
echo "== volarc_test.c (VOL_ARC_R_OUT_PX=225)"
"$OUT/volarc_test_225"
if ${CC:-cc} -std=c11 -O2 -I../main -DBRT_MIN_PERCENT=19 -c ../main/dpower.c -o "$OUT/dpower_min19.o" 2>"$OUT/min19.err"; then
    echo "dpower.c compiled with BRT_MIN_PERCENT=19; the minimum must be at least twice the dim level" >&2
    exit 1
fi
grep -q "BRT_MIN_PERCENT must be at least twice DISP_DIM_PERCENT" "$OUT/min19.err" || { cat "$OUT/min19.err" >&2; exit 1; }
echo "== dpower.c (BRT_MIN_PERCENT=19) refused"
