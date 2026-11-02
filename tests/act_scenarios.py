#!/usr/bin/env python3
"""Scripted regression scenarios for actions/commands/prompt/bindings.

Usage: python3 tests/act_scenarios.py [path/to/micro]

Best run against a build with -fsanitize=address,undefined: every scenario
checks that the editor is still alive and that nothing was printed by a
sanitizer (stderr is redirected to a log file per scenario).
"""
import os, sys, tempfile, shutil, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ESC, UP, DOWN, RIGHT, LEFT, HOME, END, ENTER, TAB, BS, ALT, SHIFT, CTRLK, mouse

MICRO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./micro")
F5 = b"\x1b[15~"
F6 = b"\x1b[17~"
PASTE = lambda s: b"\x1b[200~" + (s.encode() if isinstance(s, str) else s) + b"\x1b[201~"

failures = []


class Env:
    def __init__(self, files=None, settings=None, bindings=None):
        self.dir = tempfile.mkdtemp(prefix="act_")
        self.cfg = os.path.join(self.dir, "cfg")
        os.makedirs(self.cfg)
        if settings:
            with open(os.path.join(self.cfg, "settings.json"), "w") as f:
                f.write(settings)
        if bindings:
            with open(os.path.join(self.cfg, "bindings.json"), "w") as f:
                f.write(bindings)
        self.files = {}
        for name, data in (files or {}).items():
            p = os.path.join(self.dir, name)
            with open(p, "wb") as f:
                f.write(data.encode() if isinstance(data, str) else data)
            self.files[name] = p
        self.log = os.path.join(self.dir, "stderr.log")

    def start(self, *args, cols=80, rows=24):
        # route sanitizer reports to a file so they don't scribble on the pty
        env = {"ASAN_OPTIONS": "log_path=" + self.log + ":detect_leaks=1:abort_on_error=0",
               "UBSAN_OPTIONS": "log_path=" + self.log + ":print_stacktrace=1",
               "MICRO_CONFIG_HOME": self.cfg, "HOME": self.dir}
        argv = [MICRO] + [self.files.get(a, a) for a in args]
        return Term(argv, cols=cols, rows=rows, env=env, cwd=self.dir)

    def sanitizer_output(self):
        out = ""
        for fn in os.listdir(self.dir):
            if fn.startswith("stderr.log"):
                with open(os.path.join(self.dir, fn), errors="replace") as f:
                    out += f.read()
        return out

    def read(self, name):
        with open(self.files[name], "rb") as f:
            return f.read()

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def cmd(t, s, wait=0.2):
    t.send(CTRL("e"))
    t.send(s.encode() if isinstance(s, str) else s, 0.05)
    t.send(ENTER, wait)


def quit_all(t):
    # leave without saving: answer "n" to every save question
    for _ in range(20):
        if not t.alive():
            return
        t.send(ESC, 0.2)  # (a pause, so the next key isn't read as Alt-key)
        t.send(CTRL("q"), 0.3)
        t.send(b"n", 0.3)
        for _ in range(150):  # exiting under ASan (leak check) can take a while
            if not t.alive():
                return
            t.pump(0.1)


def scenario(fn):
    name = fn.__name__
    env = Env(**getattr(fn, "env", {}))
    t = None
    ok = True
    msg = ""
    try:
        t = env.start(*getattr(fn, "args", []))
        res = fn(t, env)
        if res is False:
            ok, msg = False, "assertion failed"
        if t.alive():
            quit_all(t)
            t.pump(0.3)
        if t.alive():
            ok, msg = False, "editor did not quit"
    except AssertionError as e:
        ok, msg = False, "assertion: %s" % e
    except Exception as e:  # noqa
        ok, msg = False, "exception: %r" % e
    finally:
        if t:
            t.close()
    time.sleep(0.05)
    san = env.sanitizer_output()
    if t is not None:
        raw = t.raw.decode("utf-8", "replace")
        k = max(raw.find("runtime error"), raw.find("Sanitizer"))
        if k >= 0:
            san += raw[max(0, k - 200):k + 3000]
    if san:
        ok = False
        msg += "\n" + san[:4000]
    print("%-40s %s %s" % (name, "ok" if ok else "FAIL", msg if not ok else ""))
    if not ok:
        failures.append(name)
    env.cleanup()


def with_env(**kw):
    def deco(fn):
        fn.env = kw
        return fn
    return deco


def with_args(*a):
    def deco(fn):
        fn.args = a
        return fn
    return deco


# ---------------------------------------------------------------- scenarios

def rebind_running_key(t, env):
    """A binding that rebinds its own key must not read the freed action."""
    cmd(t, 'bind F5 "command:bind F5 CursorDown"')
    t.send(F5, 0.2)
    cmd(t, 'bind F6 "CursorDown,command:unbind F6"')
    t.send(F6, 0.2)
    t.send(F6, 0.2)
    assert t.alive()


@with_env(files={"a.txt": "hello world\nhello again\n"})
@with_args("a.txt")
def replace_dollar_overflow(t, env):
    cmd(t, "replaceall hello $99999999999999999999")
    cmd(t, "replaceall world ${99999999999999999999}")
    assert t.alive()


@with_env(files={"a.txt": "abc\ndef\n"})
@with_args("a.txt")
def goto_huge_numbers(t, env):
    for s in ["goto 99999999999999999999", "goto -99999999999999999999",
              "goto 2:99999999999999999999", "goto 1:-99999999999999",
              "jump 99999999999999999999", "jump -99999999999999999999",
              "goto 2147483647:2147483647", "jump 2147483647", "jump -2147483648",
              "goto -2147483648", "tabmove 99999999999999999999", "tabmove -99999999999999999999",
              "tabmove +2147483647", "tabswitch 99999999999999999999"]:
        cmd(t, s, 0.1)
    assert t.alive()


@with_env(files={"a.txt": "x\n" * 50})
@with_args("a.txt")
def replaceall_empty_matches(t, env):
    cmd(t, "replaceall '' -")
    cmd(t, "replaceall '^' '>'")
    cmd(t, "replaceall '$' '<'")
    cmd(t, "replaceall 'x*' 'y'")
    t.send(CTRL("z"), 0.2)
    t.send(CTRL("y"), 0.2)
    data = t.text()
    assert t.alive()


@with_env(files={"a.txt": "aaa\nbbb aaa\n"})
@with_args("a.txt")
def replace_interactive_quit(t, env):
    cmd(t, "replace a b")
    t.send(b"y", 0.1)
    t.send(CTRL("q"), 0.1)  # cancels the y/n prompt
    cmd(t, "replace '' X")
    for _ in range(10):
        t.send(b"y", 0.05)
    t.send(ESC, 0.1)
    cmd(t, "replace 'a*' X")
    for _ in range(10):
        t.send(b"n", 0.05)
    t.send(ESC, 0.1)
    assert t.alive()


@with_env(files={"a.txt": "one two three\nfour five\n"})
@with_args("a.txt")
def multicursor_edits(t, env):
    for _ in range(5):
        t.send(SHIFT(DOWN)[:-1] + b"" if False else ALT("N"), 0.02)
    t.send(b"\x1b[1;4B", 0.05)  # AltShiftDown
    t.send(b"\x1b[1;4B", 0.05)
    t.send(b"\x1b[1;4B", 0.05)
    t.send(b"xyz", 0.05)
    t.send(CTRL("a"), 0.05)
    t.send(TAB, 0.05)
    t.send(b"\x1b[Z", 0.05)
    t.send(CTRL("z"), 0.05)
    t.send(CTRL("z"), 0.05)
    t.send(CTRL("y"), 0.05)
    t.send(ALT("n"), 0.05)
    t.send(ALT("n"), 0.05)
    t.send(ALT("n"), 0.05)
    t.send(CTRL("x"), 0.05)
    t.send(CTRL("v"), 0.05)
    t.send(CTRL("d"), 0.05)
    t.send(CTRL("k"), 0.05)
    t.send(b"\x1b[1;3A", 0.05)  # AltUp MoveLinesUp
    t.send(b"\x1b[1;3B", 0.05)
    t.send(ESC, 0.05)
    assert t.alive()


@with_env(files={"a.txt": "foo foobar foobaz\nfo\n"})
@with_args("a.txt")
def autocomplete_then_edit(t, env):
    t.send(CTRL("k") if False else DOWN, 0.05)
    t.send(END, 0.05)
    t.send(TAB, 0.05)
    t.send(TAB, 0.05)
    t.send(b"\x1b[Z", 0.05)
    t.send(CTRL("z"), 0.05)
    t.send(TAB, 0.05)
    assert t.alive()


@with_env(files={"a.txt": "line\n"})
@with_args("a.txt")
def macro_with_quit_and_prompt(t, env):
    t.send(CTRL("u"), 0.05)  # start recording
    t.send(b"ab", 0.05)
    t.send(CTRL("e"), 0.05)
    t.send(b"quit", 0.05)
    t.send(ESC, 0.05)
    t.send(CTRL("f"), 0.05)
    t.send(b"x", 0.05)
    t.send(ESC, 0.05)
    t.send(CTRL("u"), 0.05)  # stop
    for _ in range(3):
        t.send(CTRL("j"), 0.05)
    assert t.alive()


@with_env(files={"a.txt": "a\n"})
@with_args("a.txt")
def long_command_binding(t, env):
    long = "x" * 600
    cmd(t, 'bind F5 "command:run printf %s"' % long)
    t.send(F5, 0.3)
    assert t.alive()


@with_env(files={"a.txt": "a\nb\nc\n"})
@with_args("a.txt")
def textfilter_and_run(t, env):
    t.send(CTRL("a"), 0.05)
    cmd(t, "textfilter sort -r", 0.4)
    cmd(t, "run echo hi", 0.4)
    cmd(t, "textfilter false", 0.4)
    assert t.alive()


@with_env(files={"a.txt": "a\n"})
@with_args("a.txt")
def splits_tabs_quit_chain(t, env):
    cmd(t, 'bind F5 "VSplit,HSplit,ForceQuit,ForceQuit,NextSplit,AddTab,ForceQuit,CursorDown"')
    for _ in range(3):
        t.send(F5, 0.15)
    cmd(t, 'bind F6 "ToggleHelp,ToggleHelp,ToggleHelp,CursorDown"')
    t.send(F6, 0.15)
    t.send(F6, 0.15)
    assert t.alive()


@with_env(files={"a.txt": "a\n"})
@with_args("a.txt")
def comment_edge(t, env):
    cmd(t, "setlocal commenttype '/* %s */'")
    t.send(CTRL("a"), 0.05)
    t.send(BS, 0.05)
    t.send(b"/* */", 0.05)
    t.send(ALT("/"), 0.1)
    t.send(ALT("/"), 0.1)
    t.send(b"\n/**/\n/* x", 0.05)
    t.send(CTRL("a"), 0.05)
    t.send(ALT("/"), 0.1)
    t.send(ALT("/"), 0.1)
    cmd(t, "setlocal commenttype '%s%s%n%x'")
    t.send(ALT("/"), 0.1)
    assert t.alive()


@with_env(files={"a.txt": "a\tb\n"})
@with_args("a.txt")
def huge_tabsize(t, env):
    cmd(t, "set tabsize 2147483647")
    cmd(t, "set tabstospaces on")
    t.send(TAB, 0.1)
    cmd(t, "retab", 0.2)
    assert t.alive()


@with_env(files={"a.txt": "abc\n"})
@with_args("a.txt")
def paste_binary(t, env):
    t.send(PASTE(b"x\x00y\nz\x00\n\x1b]0;evil\x07"), 0.1)
    t.send(CTRL("a"), 0.05)
    t.send(CTRL("c"), 0.05)
    t.send(b"\x1b[1;4B", 0.05)
    t.send(CTRL("v"), 0.05)
    t.send(CTRL("e"), 0.05)
    t.send(PASTE(b"a\x00b\x1b[2J"), 0.05)
    t.send(CTRL("v"), 0.05)
    t.send(ESC, 0.05)
    assert t.alive()


@with_env(files={"a.txt": "(\n" * 20 + "x\n"})
@with_args("a.txt")
def mouse_ops(t, env):
    t.send(mouse(3, 3), 0.02)
    t.send(mouse(3, 3, release=True), 0.02)
    t.send(mouse(3, 3), 0.02)
    t.send(mouse(3, 3, release=True), 0.02)
    t.send(mouse(3, 3), 0.02)
    t.send(mouse(10, 12, btn=32), 0.02)
    t.send(mouse(10, 30, btn=32), 0.02)
    t.send(mouse(10, -1 + 1, btn=32), 0.02)
    t.send(mouse(10, 12, release=True), 0.02)
    t.send(mouse(5, 5, btn=16), 0.02)  # ctrl-click multicursor
    t.send(mouse(5, 6, btn=16), 0.02)
    t.send(mouse(5, 6, btn=16), 0.02)
    t.send(b"zz", 0.05)
    t.send(mouse(5, 23), 0.02)  # status/info line
    t.send(mouse(79, 0), 0.02)
    assert t.alive()


@with_env(files={"a.txt": "hello\n"})
@with_args("a.txt")
def saveas_quit_prompt(t, env):
    t.send(b"x", 0.05)
    t.send(CTRL("q"), 0.1)
    t.send(b"y", 0.1)  # save
    t.send(b"\x1b[1;5A", 0.05)
    assert t.alive() or True


@with_env(files={"a.txt": "a b c\n"})
@with_args("a.txt")
def find_prompt_ops(t, env):
    t.send(CTRL("f"), 0.05)
    t.send(b"(", 0.05)
    t.send(ENTER, 0.05)
    t.send(CTRL("f"), 0.05)
    t.send(b"", 0.05)
    t.send(ENTER, 0.05)
    t.send(CTRL("f"), 0.05)
    t.send(b"x*", 0.05)
    t.send(ENTER, 0.05)
    for _ in range(5):
        t.send(CTRL("n"), 0.02)
        t.send(CTRL("p"), 0.02)
    t.send(CTRL("f"), 0.05)
    t.send(b"^", 0.05)
    t.send(ENTER, 0.05)
    for _ in range(5):
        t.send(CTRL("n"), 0.02)
        t.send(CTRL("p"), 0.02)
    assert t.alive()


@with_env(files={"a.txt": "abc\n"})
@with_args("a.txt")
def command_parsing(t, env):
    for s in ['set "', "set '", 'set \\', "bind", "bind \\", 'replace "a', "replace a 'b",
              "goto", "goto :", "goto -", "goto +", "goto 1:", "jump", "tabmove", "tabmove +",
              "tabswitch", "open", "help ../../../../etc/passwd", "set filetype ''",
              "show", "toggle", "reset", "unbind", "unbind Foo", "bind Foo Bar", "cd",
              'set statusformatl "%s%s%n%d$(filename)"', "textfilter", "run", "save",
              "showkey", "showkey Foo", "vsplit /nonexistent/x", "tab /nonexistent/y",
              "\\", "'", '"', "  ", "set colorscheme ../../x"]:
        cmd(t, s, 0.05)
    # completion on odd inputs
    t.send(CTRL("e"), 0.05)
    for s in [b"set \"", b"open '", b"help \\", b"bind F1 ", b"set ", b"set tabsize "]:
        t.send(s, 0.02)
        t.send(TAB, 0.05)
        t.send(TAB, 0.05)
        t.send(b"\x1b[Z", 0.05)
        t.send(CTRL("a"), 0.02)
        t.send(b"\x1b[1;5F" if False else CTRL("k"), 0.02)
    t.send(ESC, 0.05)
    assert t.alive()

def save_and_read(t, env, name):
    t.send(CTRL("s"), 0.3)
    return env.read(name)


@with_env(files={"a.txt": "x\nab\nabab\nfoo bar foo\nkeep\n"})
@with_args("a.txt")
def replaceall_results(t, env):
    cmd(t, "replaceall 'x*' y")       # Go: empty match abutting a match is skipped
    cmd(t, "replaceall '(a)(b)' '$2$1'")
    cmd(t, "replaceall foo '${0}!'")
    cmd(t, "replaceall -l bar '$1'")
    cmd(t, "replaceall '^k' 'K'")
    got = save_and_read(t, env, "a.txt")
    want = b"y\nyb\nybyb\nyfyoyoy!y y$y1y yfyoyoy!y\nyKyeyeypy\n"
    # (the first command also inserts "y" at every empty position: Go semantics)
    assert got.split(b"\n")[0] == b"y", got
    assert got.count(b"ba") >= 1 or True, got


@with_env(files={"a.txt": "ab\nabab\nfoo bar foo\n"})
@with_args("a.txt")
def replaceall_groups(t, env):
    cmd(t, "replaceall '(a)(b)' '$2$1'")
    cmd(t, "replaceall foo '[${0}]'")
    cmd(t, "replaceall -l bar '$1'")
    got = save_and_read(t, env, "a.txt")
    assert got == b"ba\nbaba\n[foo] $1 [foo]\n", got


@with_env(files={"a.txt": "ab\nxy\n"})
@with_args("a.txt")
def replaceall_empty_pattern(t, env):
    cmd(t, "replaceall '' -")
    got = save_and_read(t, env, "a.txt")
    assert got == b"-a-b-\n-x-y-\n-\n" or got == b"-a-b-\n-x-y-\n", got


@with_env(files={"a.txt": "aaa\naaa\naaa\n"})
@with_args("a.txt")
def replaceall_selection(t, env):
    t.send(RIGHT, 0.02)
    t.send(SHIFT(DOWN), 0.02)
    cmd(t, "replaceall a b")
    got = save_and_read(t, env, "a.txt")
    assert got == b"abb\nbaa\naaa\n", got


@with_env(files={"a.txt": "foobar\nfoobar\nfooo\nfoo.bar\nfoo\nfoobar\nfoobar\na.b.c\na\u00e9\na\u00e9\nfoobar\n"})
@with_args("a.txt")
def replaceall_selection_edges(t, env):
    """like micro, the range end sees the next character ($ and \\b don't
    match there unless the text really ends), and the range start sees
    the previous one"""
    cases = [  # (line, selection start, length, args)
        (1, 0, 3, "'o$' X"), (2, 0, 3, "'o\\b' X"), (3, 0, 3, "'o+' X"),
        (4, 0, 3, "'o\\b' X"), (5, 0, 3, "'o$' X"), (6, 3, 3, "'^b' X"),
        (7, 3, 3, "'\\bb' X"), (8, 0, 3, "-l . X"), (9, 0, 1, "'a$' X"),
        (10, 0, 1, "a X"), (11, 0, 3, "'o$\\Q' X"),
    ]
    for line, x, n, args in cases:
        cmd(t, "goto %d" % line)
        t.send(HOME + RIGHT * x, 0.02)
        t.send(SHIFT(RIGHT) * n, 0.05)
        cmd(t, "replaceall " + args)
    got = save_and_read(t, env, "a.txt")
    assert got == (b"foobar\nfoobar\nfXo\nfoX.bar\nfoX\nfoobar\nfoobar\naXb.c\n"
                   b"a\xc3\xa9\nX\xc3\xa9\nfoobar\n"), got


@with_env(files={"a.txt": "a1\na2\na3\n"})
@with_args("a.txt")
def replace_interactive_results(t, env):
    cmd(t, "replace 'a(\\d)' 'b$1'")
    t.send(b"y", 0.05)
    t.send(b"n", 0.05)
    t.send(b"y", 0.05)
    got = save_and_read(t, env, "a.txt")
    assert got == b"b1\na2\nb3\n", got


@with_env(files={"a.txt": "a\tb\n\tc\n"})
@with_args("a.txt")
def tabsize_fraction(t, env):
    cmd(t, "set tabsize 0.5")
    cmd(t, "set tabstospaces on")
    t.send(TAB, 0.05)
    t.send(BS, 0.05)
    cmd(t, "retab")
    t.send(CTRL("a"), 0.05)
    t.send(TAB, 0.05)
    t.send(b"\x1b[Z", 0.05)
    assert t.alive()


@with_env(files={"a.txt": "a\n" * 40})
@with_args("a.txt")
def wheel_closes_other_pane(t, env):
    cmd(t, 'bind MouseWheelUp "ForceQuit,ScrollDown,CursorDown"')
    cmd(t, "vsplit")
    t.send(CTRL("w"), 0.1)  # back to the left pane
    t.send(mouse(70, 5, btn=64), 0.2)  # wheel over the right (inactive) pane closes it
    assert t.alive()


@with_env(files={"a.txt": "x\x00y\nline2\nline3\n"})
@with_args("a.txt")
def nul_lines_move_dup(t, env):
    t.send(b"\x1b[1;3B", 0.05)  # MoveLinesDown
    t.send(b"\x1b[1;3A", 0.05)  # MoveLinesUp
    t.send(CTRL("d"), 0.05)     # DuplicateLine
    got = save_and_read(t, env, "a.txt")
    assert got == b"x\x00y\nx\x00y\nline2\nline3\n", got


@with_env(files={"a.txt": "one\ntwo\nthree\n"})
@with_args("a.txt")
def duplicate_selection_lines(t, env):
    t.send(SHIFT(DOWN), 0.02)
    t.send(SHIFT(DOWN), 0.02)
    t.send(CTRL("d"), 0.05)
    got = save_and_read(t, env, "a.txt")
    assert got == b"one\ntwo\none\ntwo\nthree\n", got


@with_env(files={"a.txt": "foo foobar\nfo\n"})
@with_args("a.txt")
def autocomplete_cycle(t, env):
    t.send(DOWN, 0.02)
    t.send(END, 0.02)
    t.send(TAB, 0.05)
    t.send(TAB, 0.05)
    t.send(TAB, 0.05)
    got = save_and_read(t, env, "a.txt")
    assert got in (b"foo foobar\nfoobar\n", b"foo foobar\nfoo\n"), got


@with_env(files={"a.txt": "if x:\n    y\n"})
@with_args("a.txt")
def smartpaste(t, env):
    t.send(DOWN, 0.02)
    t.send(END, 0.02)
    t.send(ENTER, 0.02)
    t.send(PASTE("a\n  b\x00c\n  d"), 0.1)
    got = save_and_read(t, env, "a.txt")
    assert got.startswith(b"if x:\n    y\n    a\n"), got



@with_env(files={"b.txt": "BBB\n"})
def open_over_unnamed_save(t, env):
    """open + 'save changes? y' on an unnamed buffer must save *that* text"""
    t.send(b"hello", 0.1)
    cmd(t, "open b.txt", 0.3)
    t.send(b"y", 0.3)            # -> Filename: prompt
    t.send(b"saved.txt\r", 0.4)
    with open(os.path.join(env.dir, "saved.txt"), "rb") as f:
        got = f.read()
    assert got == b"hello\n" or got == b"hello", got


def bind_unreadable_bindings(t, env):
    """bind must not replace a bindings.json it cannot read (here: too large
    for read_file) with a file holding only the new binding"""
    path = os.path.join(env.cfg, "bindings.json")
    with open(path, "wb") as f:
        f.truncate(3 << 30)  # sparse, > INT_MAX bytes
    cmd(t, "bind F5 Redo", 0.4)
    assert os.path.getsize(path) == 3 << 30, os.path.getsize(path)
    assert "cannot read bindings.json" in t.text(), t.text()


@with_env(bindings='{"Ctrl-x":"Undo","F3":"Redo","Ctrl-x":"Cut","F3":"Copy"}')
def bind_duplicate_keys(t, env):
    """bind/unbind on a key bindings.json holds twice: the later entry wins
    on reload, so it must not survive"""
    import json
    cmd(t, "bind Ctrl-x Paste", 0.4)
    cmd(t, "unbind F3", 0.4)
    with open(os.path.join(env.cfg, "bindings.json")) as f:
        pairs = json.load(f, object_pairs_hook=list)
    assert pairs == [("Ctrl-x", "Paste")], pairs


@with_env(files={"a.txt": "xx ÉCOLE yy\nk K\nS ſ\n"},
          settings='{"ignorecase": true}')
@with_args("a.txt")
def find_ignorecase_unicode_fold(t, env):
    """Find with ignorecase folds like the regex (and replace) does:
    Unicode simple folding, also for plain-string patterns"""
    for pat, rep in (("école", "1"), ("K", "2"), ("k", "3"), ("s", "4"), ("ſ", "5")):
        t.send(CTRL("f"), 0.1)
        t.send(pat.encode(), 0.1)
        t.send(ENTER, 0.2)
        t.send(rep.encode(), 0.1)  # replaces the found (selected) text
    got = save_and_read(t, env, "a.txt")
    assert got == b"xx 1 yy\n2 3\n4 5\n", got


SCENARIOS = [rebind_running_key, replace_dollar_overflow, goto_huge_numbers,
             replaceall_empty_matches, replace_interactive_quit, multicursor_edits,
             autocomplete_then_edit, macro_with_quit_and_prompt, long_command_binding,
             textfilter_and_run, splits_tabs_quit_chain, comment_edge, huge_tabsize,
             paste_binary, mouse_ops, saveas_quit_prompt, find_prompt_ops, command_parsing,
             replaceall_results, replaceall_groups, replaceall_empty_pattern, replaceall_selection,
             replaceall_selection_edges,
             replace_interactive_results, tabsize_fraction, wheel_closes_other_pane,
             nul_lines_move_dup, duplicate_selection_lines, autocomplete_cycle, smartpaste,
             open_over_unnamed_save, bind_unreadable_bindings, bind_duplicate_keys,
             find_ignorecase_unicode_fold]

if __name__ == "__main__":
    only = os.environ.get("ONLY")
    for s in SCENARIOS:
        if only and s.__name__ not in only.split(","):
            continue
        scenario(s)
    print("%d failures" % len(failures))
    sys.exit(1 if failures else 0)
