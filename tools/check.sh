#!/bin/sh
# Universal C conformance checks for cmicro (see UNIVERSAL_C.md, section 13,
# at https://github.com/v3ai/universal-c).  Run with `./build.sh check`.
#
#  1. Every file compiles with gcc and clang under -std=c89, c99, c11, c17
#     and c2x with -pedantic-errors -Werror and the strict warning set.
#  2. No construct that -std=c89 lets through: C95 headers, trigraphs, or
#     headers outside the C89 library (except in src/platform.c).
#  3. The tests build and give identical output in every configuration that
#     is available here: sanitizers, unsigned plain char, 32-bit, big-endian.
#
# Configurations whose tools are missing are skipped and listed at the end.
set -u
cd "$(dirname "$0")/.." || exit 1

FLAGS="-Wall -Wextra -pedantic-errors -Werror -Wconversion -Wsign-conversion -Wshadow \
-Wformat=2 -Wcast-align -Wstrict-prototypes -Wmissing-prototypes"
SRC="src/*.c tests/*.c"
APP="src/*.c"
FMT="tests/format_test.c src/util.c src/platform.c"
RGX="tests/syntax_regex_test.c src/conf.c src/regex.c src/util.c src/platform.c"

OUT=$(mktemp -d "${TMPDIR:-/tmp}/cmicro-check.XXXXXX") || exit 1
trap 'rm -rf "$OUT"' EXIT
trap 'exit 130' INT TERM
failed=0
skipped=""

have() { command -v "$1" >/dev/null 2>&1; }
pass() { echo "ok"; }
fail() { echo "FAIL"; sed 's/^/    /' "$OUT/log" | head -40; failed=1; }
row()  { printf '%-58s ' "$1"; }

echo "== 1. compile matrix"
for cc in gcc clang; do
    if ! have "$cc"; then skipped="$skipped $cc"; continue; fi
    for std in c89 c99 c11 c17 c2x; do
        row "$cc -std=$std"
        if $cc -std=$std $FLAGS -fsyntax-only $SRC >"$OUT/log" 2>&1; then pass; else fail; fi
    done
done

echo "== 2. constructs -std=c89 does not reject"
row "no C95 headers or trigraphs"
if grep -nE '#[[:space:]]*include[[:space:]]*<(iso646|wchar|wctype)\.h>|\?\?[=(/)'"'"'<!>-]' \
        src/*.c src/*.h tests/*.c >"$OUT/log" 2>&1; then fail; else pass; fi
row "only C89 headers outside src/platform.c"
C89H='assert|ctype|errno|float|limits|locale|math|setjmp|signal|stdarg|stddef|stdio|stdlib|string|time'
if grep -nE '#[[:space:]]*include[[:space:]]*<' src/*.c src/*.h tests/*.c \
        | grep -v '^src/platform\.c:' \
        | grep -vE "<($C89H)\.h>" \
        | grep -vE '^src/port\.h:[0-9]+:#  include <stdint\.h>' >"$OUT/log" 2>&1; then fail; else pass; fi

echo "== 3. tests in every configuration (output must match)"
REF=""
# config NAME CC "CFLAGS" [RUNNER]
config() {
    name=$1 cc=$2 cflags=$3 runner=${4:-}
    row "$name"
    if ! $cc -std=c89 $cflags -o "$OUT/fmt-$name" $FMT >"$OUT/log" 2>&1 ||
       ! $cc -std=c89 $cflags -o "$OUT/rgx-$name" $RGX >>"$OUT/log" 2>&1 ||
       ! $cc -std=c89 $cflags -o "$OUT/micro-$name" $APP >>"$OUT/log" 2>&1; then
        fail; return
    fi
    if ! { $runner "$OUT/fmt-$name" && $runner "$OUT/rgx-$name" runtime/syntax/*.yaml &&
           $runner "$OUT/micro-$name" -options; } >"$OUT/res-$name" 2>"$OUT/log"; then
        cat "$OUT/res-$name" >>"$OUT/log"; fail; return
    fi
    if [ -z "$REF" ]; then
        REF=$name
    elif ! cmp -s "$OUT/res-$REF" "$OUT/res-$name"; then
        diff "$OUT/res-$REF" "$OUT/res-$name" >"$OUT/log"; fail; return
    fi
    pass
}
# probe: can this compiler build and run with these flags here?
works() {
    printf '#include <errno.h>\n#include <stdio.h>\nint main(void) { return 0; }\n' >"$OUT/probe.c"
    "$1" $2 -o "$OUT/probe" "$OUT/probe.c" >/dev/null 2>&1 && ${3:-} "$OUT/probe" >/dev/null 2>&1
}
try() {
    if works "$2" "$3" "${4:-}"; then config "$@"; else skipped="$skipped $1"; fi
}

CC=${CC:-cc}
config native "$CC" "-O2"
try gcc-c89-asan-ubsan gcc "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
try clang-c89-msan clang "-O1 -g -fsanitize=memory -fno-sanitize-recover=all"
try clang-c89-ubsan clang "-O2 -fsanitize=undefined -fno-sanitize-recover=all"
try gcc-unsigned-char gcc "-O2 -funsigned-char"
try clang-signed-char clang "-O2 -fsigned-char"
try gcc-32-bit gcc "-O2 -m32"
if have qemu-s390x; then
    try s390x-big-endian s390x-linux-gnu-gcc "-O2 -static" qemu-s390x
else
    skipped="$skipped s390x-big-endian"
fi

echo
[ -n "$skipped" ] && echo "skipped (tools not available):$skipped"
if [ "$failed" -ne 0 ]; then echo "UNIVERSAL C CHECK FAILED"; exit 1; fi
echo "all checks passed"
