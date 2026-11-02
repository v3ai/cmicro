#!/bin/sh
# Build and run the file/buffer I/O tests, plainly and under
# AddressSanitizer + UBSan.  Usage: tests/io_run.sh [CC]
set -e
cd "$(dirname "$0")/.."
CC=${1:-${CC:-cc}}
SRCS="src/editor.c src/actions.c src/bindings.c src/commands.c src/prompt.c src/render.c
      src/tabs.c src/pane.c src/buffer.c src/options.c src/screen.c src/input.c src/syntax.c
      src/colors.c src/runtime.c src/runtime_data.c src/conf.c src/regex.c src/util.c
      src/platform.c src/platform_win32.c"
FLAGS="-std=c99 -Wall -Wextra -pedantic"
$CC -O2 $FLAGS -o tests/io_test tests/io_test.c $SRCS
./tests/io_test
$CC -O1 -g $FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -o tests/io_test_asan tests/io_test.c $SRCS
ASAN_OPTIONS=detect_leaks=0 ./tests/io_test_asan
