# micro (C rewrite) - builds with any C99 compiler; tested with gcc and clang.
#
#   make                      Linux, macOS, BSDs (or MSYS2/MinGW on Windows)
#   make windows              cross-compile micro.exe with MinGW-w64
#
# Without make, this also works on any platform:  cc -O2 -o micro src/*.c
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -pedantic
LDFLAGS ?=
PREFIX  ?= /usr/local

SRCS = src/main.c src/editor.c src/actions.c src/bindings.c src/commands.c \
       src/prompt.c src/render.c src/tabs.c src/pane.c src/buffer.c \
       src/options.c src/screen.c src/input.c src/syntax.c src/colors.c \
       src/backup.c src/runtime.c src/runtime_data.c src/conf.c src/regex.c src/util.c \
       src/platform.c src/platform_win32.c
OBJS = $(SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
EXE = .exe
endif

micro$(EXE): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Cross-compile a native Windows build (needs x86_64-w64-mingw32-gcc)
MINGW ?= x86_64-w64-mingw32-gcc
windows: micro.exe
micro.exe: $(SRCS) src/*.h
	$(MINGW) -O2 -std=c99 -Wall -Wextra -pedantic -static -o $@ $(SRCS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Regenerate the embedded runtime files (needs python3; output is checked in)
runtime:
	python3 tools/embed.py

test: micro tests/syntax_regex_test tests/regex_test tests/syntax_filter_test
	./tests/syntax_regex_test runtime/syntax/*.yaml
	./tests/regex_test
	./tests/syntax_filter_test runtime/syntax/*.yaml -- src/main.c runtime/help/options.md tests/term.py Makefile

# Slower, more thorough checks: file I/O tests (plain and under
# ASan/UBSan) and end-to-end tests driving the editor in a pty (need
# python3 with pyte: pip install pyte).
check: test
	sh tests/io_run.sh $(CC)
	python3 tests/term_test.py ./micro
	python3 tests/io_term_test.py ./micro
	python3 tests/act_scenarios.py ./micro

RT_SRCS = src/regex.c src/conf.c src/util.c src/colors.c src/runtime.c \
          src/runtime_data.c src/platform.c src/platform_win32.c

tests/regex_test: tests/regex_test.c src/regex.c src/util.c src/platform.c src/platform_win32.c src/*.h
	$(CC) $(CFLAGS) -o $@ tests/regex_test.c src/regex.c src/util.c src/platform.c src/platform_win32.c

tests/syntax_filter_test: tests/syntax_filter_fuzz.c src/syntax.c $(RT_SRCS) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/syntax_filter_fuzz.c $(RT_SRCS)

tests/syntax_regex_test: tests/syntax_regex_test.c src/conf.c src/regex.c src/util.c src/platform.c src/platform_win32.c
	$(CC) $(CFLAGS) -o $@ $^

install: micro
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 micro $(DESTDIR)$(PREFIX)/bin/micro

clean:
	rm -f micro micro.exe $(OBJS) tests/syntax_regex_test tests/regex_test tests/syntax_filter_test \
	      tests/io_test tests/io_test_asan

.PHONY: runtime test check install clean windows
