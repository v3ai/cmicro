# cmicro: micro, rewritten in C

The [micro](https://github.com/micro-editor/micro) terminal text editor,
rewritten from scratch in C99 with no dependencies. Same keys, commands,
settings, syntax highlighting and colorschemes as micro. Runs on Linux,
macOS, the BSDs and natively on Windows.

```
make
./micro file.c
```

## About

micro is a modern, intuitive terminal editor: Ctrl-S saves, Ctrl-Q quits,
Ctrl-C/Ctrl-V copy and paste, and the mouse just works. The original is
written in Go. cmicro is a full reimplementation in portable C99 that needs
nothing but a C compiler and the C library. It builds in seconds into a
single ~700 KB executable with every syntax file and colorscheme built in.

It aims to feel exactly like micro. If you already use micro, your
`settings.json` and `bindings.json` work as they are, and the key bindings,
commands, options and highlighting behave the same way. If you don't, press
Ctrl-G inside the editor for help.

Highlights:

- **Same editor:** micro's default key bindings, ~130 actions, command bar
  with completion and history, all of its options, and its settings and
  bindings file formats
- **Highlighting:** micro's ~160 syntax definitions and 25 colorschemes,
  embedded, using micro's highlighting algorithm, in 16, 256 or 24-bit color
- **Editing:** splits, tabs, multiple cursors, macros, undo/redo, regex
  find and replace, softwrap, mouse support, system clipboard
- **Safety:** crash-recovery backups, reload prompts when a file changes
  on disk, saving through sudo on permission errors
- **Everything from scratch:** its own regular-expression engine (Go/RE2
  syntax, tested against Go's), YAML and JSON parsers and terminal renderer
- **Portable:** one small platform layer for POSIX systems and one for the
  Windows console; everything else is standard C

Not included: micro's Lua plugin system, its built-in terminal emulator and
the git diff gutter.

## Download

Prebuilt binaries are on the
[Releases](https://github.com/v3ai/cmicro/releases) page:

- `micro.exe`: Windows 10 (1809) or later. Run it from PowerShell, cmd or
  Windows Terminal; no installation needed.
- `micro-linux-x86_64`: statically linked Linux build
  (`chmod +x micro-linux-x86_64`).

Or build it yourself; see below.

## Building

Requirements: any C99 compiler (developed with gcc 13 and clang; uses
nothing newer than C99).

```
make                 # builds ./micro (micro.exe on Windows)
make test            # compiles every regex in every bundled syntax file
make install         # installs to /usr/local/bin (PREFIX=... to change)
make windows         # cross-compiles micro.exe with MinGW-w64
make runtime         # re-embed runtime/ files after editing them (needs python3)
```

Without make, one command builds it on any platform, because each platform
file compiles to nothing on the other system:

```
cc -O2 -o micro src/*.c
```

The build is warning-free with `-std=c99 -Wall -Wextra -pedantic` under
gcc, clang and MinGW-w64.

## Windows

`micro.exe` is a native Windows console program. It needs Windows 10
version 1809 or later (for the console's escape-sequence support) and works
in PowerShell, cmd and Windows Terminal.

Building on Windows with [MSYS2](https://www.msys2.org):

```
pacman -S mingw-w64-ucrt-x86_64-gcc make
make
```

or, from any shell with MinGW-w64 gcc on the PATH:
`gcc -O2 -o micro.exe src/*.c`. To build it from Linux: `make windows`
(produces a statically linked `micro.exe`).

Then run it from PowerShell like any program, e.g. `.\micro.exe notes.txt`,
or put it on your PATH.

How it differs from the Unix build:

- Config lives in `%USERPROFILE%\.config\micro` (as in micro), unless
  `MICRO_CONFIG_HOME` or `XDG_CONFIG_HOME` is set.
- New files default to `dos` (CRLF) line endings, like micro on Windows;
  existing files keep whatever line endings they have.
- Paths may use `\` or `/`; in the command bar a backslash is a path
  separator, not an escape, and names with spaces are completed in quotes.
- Shell commands (`run`, `textfilter`, Ctrl-b) run through `cmd.exe`.
- The clipboard is the native Windows clipboard.
- Non-ASCII file names work everywhere (all file access uses the UTF-16
  Windows APIs).
- Not available: saving through `sudo` and suspending with Ctrl-z.
- Truecolor is used automatically in Windows Terminal; elsewhere colors are
  mapped to the 256-color palette (`set truecolor on` to force it).

## "Only the C standard library"

Everything is written against the ISO C standard library, with one
unavoidable exception: ISO C has no notion of a terminal. Raw keyboard input,
the window size, resize notification, directory listings (for tab
completion) and millisecond clocks don't exist in the standard library, and
it doesn't define how file names are encoded. Those calls are isolated
behind the small interface in `src/platform.h`, implemented twice:

- `src/platform.c`: POSIX (termios, ioctl, poll, opendir, stat, realpath)
  for Linux, macOS, the BSDs, WSL and Cygwin;
- `src/platform_win32.c`: the Windows console API in virtual-terminal mode,
  with the UTF-16 file APIs and the native clipboard.

Every other file includes only standard headers.

Things micro gets from Go's standard library or third-party packages are
implemented here directly:

| micro uses                         | this rewrite                                   |
|------------------------------------|------------------------------------------------|
| Go `regexp` (RE2)                  | `src/regex.c`: RE2-syntax engine, backtracking VM with a visited-state table (linear time) |
| `yaml.v2` for syntax files         | `src/conf.c`: YAML subset parser                |
| `json5` for settings/bindings      | `src/conf.c`: lenient JSON (comments, trailing commas) |
| `tcell` terminal library           | `src/screen.c` + `src/input.c`: diffing cell renderer, escape-sequence decoder, SGR mouse, bracketed paste, 256/truecolor |
| `go-runewidth`                     | `src/util.c`: East-Asian-wide / combining tables |
| Lua plugins `autoclose`, `comment`, `ftoptions`, `status` | built in natively |
| `os/exec` for shell commands       | ISO C `system()`                               |

## Feature coverage

Implemented as in micro:

- All default buffer and command-bar key bindings, `bindings.json`
  (including `buffer`/`command` sections, `command:` and `command-edit:`
  actions, and `,` `|` `&` action chaining), `bind`/`unbind`/`showkey`/`raw`
- Every buffer action micro has (≈130): word/subword motion, paragraphs,
  selection, line moves, duplicate, cut-line accumulation, indent/outdent,
  word autocomplete, overwrite mode, matching brace, macros, and more
- Multiple cursors (Alt-n, Alt-Shift-Up/Down, Alt-m, Alt-p, Alt-c, Alt-x,
  Ctrl-click)
- Undo/redo with micro's 1-second grouping; modified tracking that becomes
  clean again when you undo back to the saved state
- Find (regex and literal, incremental, `hlsearch`), FindNext/Previous with
  wrap-around, `replace` (interactive y/n/esc) and `replaceall` with `$1`
  submatches and selection-limited replacement
- Commands: set, setlocal, toggle, togglelocal, reset, show, showkey, run,
  bind, unbind, quit, goto, jump, save, replace, replaceall, vsplit, hsplit,
  tab, tabmove, tabswitch, help, log, reload, reopen, cd, pwd, open, retab,
  raw, textfilter, memusage, comment, with tab completion and history
- All of micro's options (see `help options`), `settings.json` with
  `ft:` and glob sections, saving on `set`
- Syntax highlighting using micro's region/pattern algorithm and filetype
  detection (filename, header, signature)
- Colorschemes (`color-link`, `include`, attributes, 16/256/hex colors)
- Vertical/horizontal splits, tabs, tab bar, status line format strings,
  info bar, key menu (Alt-g), help pages (Ctrl-g)
- Softwrap and wordwrap with visual-line cursor movement, horizontal scrolling,
  ruler/relative ruler, cursor line, color column, scrollbar, `showchars`
- Mouse: click, double-click word, triple-click line, drag-select, wheel,
  Ctrl-click for cursors, click to switch tabs and splits
- Clipboard: internal, external tools (wl-copy, xclip, xsel, pbcopy) or OSC 52
- Crash-recovery backups with micro's recover/ignore/abort prompt,
  `savecursor`, `autosave`, reload prompt when the file changes on disk,
  save-with-sudo on permission errors, `eofnewline`, `rmtrailingws`,
  `mkparents`, DOS line endings, UTF-8 BOM, reading from a pipe
  (`ls | micro`), `+LINE:COL` and `file:LINE:COL`

Not implemented, because they need a Lua interpreter or a full terminal
emulator: Lua plugins and the plugin manager (`plugin`, `eval`), the `term`
command, the git `diffgutter`, the linter plugin, `saveundo`, and encodings
other than UTF-8. Those options and commands are accepted and give a clear
message.

## Layout

```
src/platform.c   POSIX terminal/filesystem glue
src/platform_win32.c  Windows console/filesystem glue
src/main.c       command line, startup, main loop
src/editor.c     editor state, tabs, event dispatch, frame rendering
src/actions.c    every bindable action, typing, clipboard, search, macros
src/bindings.c   default bindings, bindings.json
src/commands.c   command bar commands, completion, replace, help
src/prompt.c     info-bar prompts, history, completion UI
src/render.c     panes, status line, tab bar, info bar
src/pane.c       cursors, softwrap geometry, scrolling
src/tabs.c       split tree layout
src/buffer.c     text storage, file I/O, undo/redo, highlight state
src/backup.c     crash backups, savecursor, sudo save
src/syntax.c     syntax definitions and highlighter
src/colors.c     colorschemes
src/regex.c      regular expression engine
src/conf.c       YAML and JSON parsers/writers
src/screen.c     cell grid + ANSI output
src/input.c      key/mouse/paste decoding, key names
src/options.c    option table, settings.json
src/runtime*.c   embedded runtime files (generated by tools/embed.py)
runtime/         micro's syntax files and colorschemes (MIT), help pages
```

## Testing

- `make test` compiles all 3,794 regular expressions from micro's syntax
  files with the built-in engine.
- The regex engine is differential-tested against Go's `regexp`: every
  bundled syntax pattern against 365 sample lines (53,328 lines of match and
  capture output) is identical, plus ~1.3M randomly generated RE2 patterns.
  Long lines are handled in linear time (e.g. a match after 3 MB of text).
- `tests/term.py` drives the editor in a pseudo-terminal (via `pyte`) for
  end-to-end tests of editing, prompts, splits, tabs, mouse, paste, resize,
  softwrap, config files and crash recovery.
- The editor was run under AddressSanitizer and UndefinedBehaviorSanitizer
  with scripted scenarios and randomized key/mouse/paste fuzzing, and the
  code was reviewed independently against micro's Go source; the issues
  found were fixed and their reproductions re-run clean.
- The Windows build is cross-compiled with MinGW-w64 and was exercised
  under Wine: editing, selection, clipboard, UTF-8 text and file names,
  saving with CRLF defaults, search, splits, `cmd.exe` commands, settings and
  crash-recovery backups. (It hasn't yet been run on a physical Windows
  machine; Wine itself can't test Ctrl-C as a key or window resizing.)
- Highlighting long minified lines is linear: a 620 KB single-line
  JavaScript file highlights in about half a second.

Known small differences: case-insensitive search folds ASCII letters only,
and Unicode classes such as `\pL` are approximated by code-point ranges.

## License

micro is MIT licensed (see `LICENSE-micro`); the syntax files and
colorschemes in `runtime/` come from micro and keep their licenses
(`runtime/syntax/LICENSE`). The C code in `src/` is a new implementation
written for this project.
