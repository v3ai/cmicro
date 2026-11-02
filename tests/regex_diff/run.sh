#!/bin/sh
# Differential test of src/regex.c against Go's regexp.
#   tests/regex_diff/run.sh [NRANDOM [SEED]]
# Needs go and python3.  Work files go to $TMPDIR (default /tmp).
set -e
cd "$(dirname "$0")/../.."
N=${1:-20000}
SEED=${2:-1}
W=$(mktemp -d "${TMPDIR:-/tmp}/regex_diff.XXXXXX")
GO=${GO:-go}
command -v "$GO" >/dev/null 2>&1 || GO=/usr/local/go/bin/go

${CC:-cc} -O1 -g -std=c99 -fsanitize=address,undefined -o "$W/cdiff" \
    tests/regex_diff.c src/regex.c src/util.c src/platform.c src/platform_win32.c
(cd tests/regex_diff && GOFLAGS=-mod=mod "$GO" build -o "$W/godiff" godiff.go)
make -s tests/syntax_regex_test
SRT_VERBOSE=1 ./tests/syntax_regex_test runtime/syntax/*.yaml 2>"$W/pats" >/dev/null
# sample lines: sources, syntax files, help, plus some long/unicode lines
cat src/*.c runtime/help/*.md runtime/syntax/*.yaml tools/*.py tests/*.py \
    | awk 'length($0) > 0 && length($0) < 400' | awk 'NR % 7 == 0' > "$W/lines"

status=0
for mode in syntax random; do
    if [ $mode = syntax ]; then
        python3 tests/regex_diff/gen.py syntax "$W/pats" "$W/lines" > "$W/in"
    else
        python3 tests/regex_diff/gen.py random "$N" "$SEED" > "$W/in"
    fi
    "$W/cdiff" < "$W/in" > "$W/c.out"
    "$W/godiff" < "$W/in" > "$W/go.out"
    printf '%s: ' $mode
    python3 tests/regex_diff/cmp.py "$W/in" "$W/c.out" "$W/go.out" || status=1
done
[ $status = 0 ] && rm -rf "$W"
exit $status
