#!/bin/sh
# Compiles and runs every host test with the pure firmware modules.
set -eu
cd "$(dirname "$0")"
SRCS="../main/proto.c ../main/dpower.c"
[ -f ../main/motion.c ] && SRCS="$SRCS ../main/motion.c"
OUT="${TMPDIR:-/tmp}/clawd_host_tests.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
for t in *_test.c; do
    cc -std=c11 -Wall -Wextra -Werror -O2 -I../main "$t" $SRCS -lm -o "$OUT/${t%.c}"
    echo "== $t"
    "$OUT/${t%.c}"
done
