# cmicro - the micro text editor in Universal C (see README.md).
#
#   make            build ./micro (Linux, macOS, the BSDs, WSL, Cygwin)
#   make test       compile every regex in every bundled syntax file
#   make check      Universal C conformance: every compiler x every standard
#
# The code is C89 that also compiles unchanged as C99, C11, C17 and C23.
# Without make:  cc -O2 -o micro src/*.c
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c89 -Wall -Wextra -pedantic
LDFLAGS ?=
PREFIX  ?= /usr/local

SRCS = src/main.c src/editor.c src/actions.c src/bindings.c src/commands.c \
       src/prompt.c src/render.c src/tabs.c src/pane.c src/buffer.c \
       src/options.c src/screen.c src/input.c src/syntax.c src/colors.c \
       src/backup.c src/runtime.c src/runtime_data.c src/conf.c src/regex.c src/util.c \
       src/platform.c
OBJS = $(SRCS:.c=.o)
TEST_SRCS = tests/syntax_regex_test.c src/conf.c src/regex.c src/util.c src/platform.c
FMT_SRCS  = tests/format_test.c src/util.c src/platform.c

micro: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Regenerate the embedded runtime files (needs python3; output is checked in)
runtime:
	python3 tools/embed.py

test: micro tests/syntax_regex_test tests/format_test
	./tests/format_test
	./tests/syntax_regex_test runtime/syntax/*.yaml

tests/syntax_regex_test: $(TEST_SRCS) src/*.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

tests/format_test: $(FMT_SRCS) src/*.h
	$(CC) $(CFLAGS) -o $@ $(FMT_SRCS)

# Compile with gcc and clang under C89, C99, C11, C17 and C23 with
# -pedantic-errors -Werror and the strict warning set, then build and run the
# tests with sanitizers, unsigned char, 32-bit and big-endian where available.
check:
	sh tools/check.sh

install: micro
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 micro $(DESTDIR)$(PREFIX)/bin/micro

clean:
	rm -f micro $(OBJS) tests/syntax_regex_test tests/format_test

.PHONY: runtime test check install clean
