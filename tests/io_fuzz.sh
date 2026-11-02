#!/bin/sh
# Build and run the libFuzzer target for buffer loading/editing/undo.
# Usage: tests/io_fuzz.sh [seconds] [corpus dir]
set -e
cd "$(dirname "$0")/.."
SRCS="src/editor.c src/actions.c src/bindings.c src/commands.c src/prompt.c src/render.c
      src/tabs.c src/pane.c src/buffer.c src/options.c src/screen.c src/input.c src/syntax.c
      src/colors.c src/backup.c src/runtime.c src/runtime_data.c src/conf.c src/regex.c src/util.c
      src/platform.c src/platform_win32.c"
clang -g -O1 -std=c99 -Wall -Wextra -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
      -o tests/io_fuzz tests/io_fuzz.c $SRCS
CORPUS=${2:-}
./tests/io_fuzz -max_total_time="${1:-60}" -max_len=1024 $CORPUS
