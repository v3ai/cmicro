#!/bin/sh
# Differential tests for nolibc: each program is built twice, against the
# host C library and against nolibc, and the outputs must be identical.
# Usage: tests/nolibc/run.sh   (from the repository root; needs a host libc)
set -e
cd "$(dirname "$0")"
NL=../../nolibc
CC=${CC:-cc}
F="-std=c99 -O2 -w -ffreestanding -nostdinc -I$NL/include -fno-stack-protector -fno-pie -fno-strict-aliasing"
case "$($CC --version 2>/dev/null)" in *clang*) ;; *) F="$F -fno-tree-loop-distribute-patterns" ;; esac
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
for f in bignum string stdlib stdio time; do $CC $F -c $NL/$f.c -o "$OUT/$f.o"; done
$CC -c $NL/start.S -o "$OUT/start.o"
command -v python3 >/dev/null && python3 gen_mids.py "$OUT/mids.txt" || : > "$OUT/mids.txt"
fail=0
run() { # name, then environment assignments
    t=$1; shift
    (cd "$OUT" && env "$@" ./${t}_host > host.txt 2>&1; env "$@" ./${t}_nolibc > nolibc.txt 2>&1) || true
    if cmp -s "$OUT/host.txt" "$OUT/nolibc.txt"; then echo "ok    $t $*"
    else echo "FAIL  $t $*"; diff "$OUT/host.txt" "$OUT/nolibc.txt" | head -5; fail=1; fi
}
for t in libtest fmt fuzz mids alloc tztest; do
    $CC -O2 -w -o "$OUT/${t}_host" $t.c
    $CC $F -c $t.c -o "$OUT/$t.o"
    ld -static -nostdlib -o "$OUT/${t}_nolibc" "$OUT/$t.o" "$OUT"/bignum.o "$OUT"/string.o "$OUT"/stdlib.o \
        "$OUT"/stdio.o "$OUT"/time.o "$OUT"/start.o
done
for t in libtest fmt fuzz mids alloc; do run $t; done
for z in UTC America/Chicago Europe/Berlin Asia/Kolkata Australia/Lord_Howe Pacific/Chatham \
         America/Santiago EST5EDT JST-9 "<+03>-3" "CET-1CEST,M3.5.0,M10.5.0/3" \
         "NZST-12NZDT,M9.5.0,M4.1.0/3" "XXX3YYY,J60/0,J300/25"; do
    run tztest "TZ=$z"
done
exit $fail
