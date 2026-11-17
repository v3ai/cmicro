#!/bin/sh
# Build cmicro with nothing but a C compiler and a POSIX shell.
#
#   ./build.sh            build ./micro
#   ./build.sh test       build and run the tests
#   ./build.sh check      Universal C conformance checks (tools/check.sh)
#   ./build.sh install    install micro to $DESTDIR$PREFIX/bin
#   ./build.sh clean      remove build outputs
#   ./build.sh runtime    re-embed runtime/ files (needs python3)
#
# Several commands can be given at once (./build.sh clean test).
# Environment: CC (default cc), CFLAGS (default -O2), LDFLAGS,
# PREFIX (default /usr/local), DESTDIR.
#
# The code is C89 that also compiles unchanged as C99, C11, C17 and C23;
# without this script, `cc -O2 -o micro src/*.c` builds the editor too.
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2}
LDFLAGS=${LDFLAGS:-}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
STD="-std=c89 -Wall -Wextra -pedantic"

run() {
    echo "$@"
    "$@"
}

# CFLAGS and LDFLAGS are deliberately unquoted: they hold several words.
build() {
    run $CC $STD $CFLAGS -o micro src/*.c $LDFLAGS
}

build_tests() {
    run $CC $STD $CFLAGS -o tests/format_test \
        tests/format_test.c src/util.c src/platform.c $LDFLAGS
    run $CC $STD $CFLAGS -o tests/syntax_regex_test \
        tests/syntax_regex_test.c src/conf.c src/regex.c src/util.c src/platform.c $LDFLAGS
}

run_tests() {
    ./tests/format_test
    ./tests/syntax_regex_test runtime/syntax/*.yaml
}

install_micro() {
    [ -x micro ] || build
    run mkdir -p "$DESTDIR$PREFIX/bin"
    run cp micro "$DESTDIR$PREFIX/bin/micro"
    run chmod 755 "$DESTDIR$PREFIX/bin/micro"
}

usage() {
    sed -n '2,13s/^# \{0,1\}//p' "$0"
}

[ $# -eq 0 ] && set -- build
for cmd in "$@"; do
    case $cmd in
    build)   build ;;
    test)    build; build_tests; run_tests ;;
    check)   sh tools/check.sh ;;
    install) install_micro ;;
    clean)   run rm -f micro tests/format_test tests/syntax_regex_test src/*.o ;;
    runtime) run python3 tools/embed.py ;;
    help|-h|--help) usage ;;
    *) echo "build.sh: unknown command '$cmd'" >&2; usage >&2; exit 2 ;;
    esac
done
