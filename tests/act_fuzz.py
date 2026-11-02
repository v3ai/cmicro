#!/usr/bin/env python3
"""Randomized key/mouse/paste fuzzer for the editor, driven through a pty.

Usage: python3 tests/act_fuzz.py path/to/micro [seconds] [seed]

Build the binary with -fsanitize=address,undefined.  Random sequences of
keys (Ctrl/Alt/Shift combos), multi-cursor operations, undo/redo, macros,
splits, tabs, replace/find prompts, command-bar commands, bracketed pastes
and mouse events are fed to the editor; whenever it crashes, exits
abnormally or a sanitizer reports something, the input is saved to
act_fuzz_fail_<seed>_<n>.bin in the current directory and the report is
printed.

Safety: the editor runs in a throwaway directory with HOME and the config
dir pointing there, PATH limited to a few harmless tools, ShellMode
unbound, and random text never contains characters that could form paths
outside that directory or shell redirections.
"""
import os, sys, random, time, tempfile, shutil, signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ESC, UP, DOWN, RIGHT, LEFT, HOME, END, ENTER, TAB, BS, mouse

MICRO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./micro")
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 60
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else int(time.time())

SAFE_CHARS = "abcxyz ABC012(){}[]\"'`_-+=*.,:#\t\\" + "é漢\u0301"
CURATED = ["foo bar foo\n", "    indented\n\tline\n", "a\nb\nc\nd\n", "(", "{\n}", "x\x00y\n",
           "\x1b]0;title\x07\x1b[2J", "日本語テキスト\n", "\r\n\r\n", "if x:\n    y\n  z\n",
           "line\n" * 30, "é" * 50, "\t\t\tdeep\n", "", "\u202eRTL\u202c\n"]
COMMANDS = [
    "replace foo bar", "replace a b", "replace '' X", "replace 'x*' Y", "replace '(a)(b)' '$2$1'",
    "replaceall a bb", "replaceall '' -", "replaceall 'x*' Y", "replaceall '(\\w+)' '[$1]'",
    "replaceall -l . ,", "replaceall '^' '> '", "replaceall '$' ';'", "replaceall b 'q\\nq'",
    "replaceall '\\s+' ' '", "replaceall '(?i)A' '${1}z$99999999999'",
    "goto 1", "goto 99999", "goto -1", "goto 3:5", "goto 2:999", "jump 5", "jump -5",
    "set tabsize 1", "set tabsize 8", "set tabsize 3", "set softwrap on", "set softwrap off",
    "set ruler off", "set ruler on", "set autoclose off", "set autoclose on",
    "set tabstospaces on", "set tabstospaces off", "set smartpaste off", "set smartpaste on",
    "set ignorecase on", "set ignorecase off", "set autoindent off", "set autoindent on",
    "set clipboard internal", "set clipboard terminal", "set clipboard external",
    "set filetype c", "set filetype python", "set filetype unknown", "set hlsearch on",
    "set keymenu on", "set keymenu off", "set statusline off", "set statusline on",
    "set scrollmargin 0", "set scrollmargin 10", "set wordwrap on", "set wordwrap off",
    "set diffgutter on", "set readonly on", "set readonly off", "set multiopen vsplit",
    "set incsearch off", "set incsearch on", "set paste on", "set paste off",
    "set keepautoindent on", "set matchbrace on", "set rmtrailingws on",
    "vsplit", "hsplit", "tab", "retab", "comment", "help", "help options", "log",
    "tabmove 1", "tabmove +1", "tabmove -1", "tabswitch 1", "tabswitch 2", "show tabsize",
    "textfilter sort", "textfilter tr a-z A-Z", "textfilter cat", "run echo hi",
    "bind F5 PlayMacro", "bind F6 'SelectAll,Cut,Paste,Paste'", "bind F7 'command:replaceall a b'",
    "bind F8 'VSplit,NextSplit,Quit'", "bind F9 'command:bind F9 Undo'", "unbind F5",
    "bind F11 'SpawnMultiCursorDown,SpawnMultiCursorDown,InsertTab,Undo,Redo'",
    "bind F12 'Autocomplete|InsertTab'", "reopen", "pwd", "memusage", "raw",
    "save", "open a.txt", "open b.txt", "open a.txt:3:2", "vsplit b.txt", "tab a.txt",
]
SPECIAL = [UP, DOWN, LEFT, RIGHT, HOME, END, b"\x1b[5~", b"\x1b[6~", b"\x1b[3~", b"\x1b[2~",
           ENTER, TAB, b"\x1b[Z", BS, b"\x1bOP", b"\x1bOQ", b"\x1b[15~", b"\x1b[17~", b"\x1b[18~",
           b"\x1b[19~", b"\x1b[20~", b"\x1b[21~", b"\x1b[23~", b"\x1b[24~"]
ARROWS = [b"A", b"B", b"C", b"D", b"H", b"F"]
MODS = [b"2", b"3", b"4", b"5", b"6", b"7", b"8"]
CTRL_KEYS = "acdefgklnopqrsuvwxyzjrh"  # no Ctrl-b (shell) / Ctrl-t is fine too
ALT_KEYS = "nmpcxfbae,.{}[]/gNFPC"


def gen_text(rnd, n):
    return "".join(rnd.choice(SAFE_CHARS) for _ in range(n))


def gen_event(rnd, cols, rows):
    r = rnd.random()
    if r < 0.30:
        return gen_text(rnd, rnd.randint(1, 4)).encode()
    if r < 0.45:
        return rnd.choice(SPECIAL)
    if r < 0.58:
        return b"\x1b[1;" + rnd.choice(MODS) + rnd.choice(ARROWS)
    if r < 0.70:
        return CTRL(rnd.choice(CTRL_KEYS))
    if r < 0.78:
        return b"\x1b" + rnd.choice(ALT_KEYS).encode()
    if r < 0.83:
        s = rnd.choice(CURATED) if rnd.random() < 0.7 else gen_text(rnd, rnd.randint(0, 200))
        return b"\x1b[200~" + s.encode() + b"\x1b[201~"
    if r < 0.90:
        x, y = rnd.randint(0, cols), rnd.randint(0, rows)
        kind = rnd.random()
        if kind < 0.3:
            return mouse(x, y) + mouse(x, y, release=True)
        if kind < 0.45:
            return mouse(x, y) + mouse(rnd.randint(0, cols), rnd.randint(0, rows + 2), btn=32) + \
                mouse(x, y, release=True)
        if kind < 0.65:
            return mouse(x, y, btn=rnd.choice([64, 65]))
        if kind < 0.8:
            return mouse(x, y, btn=16)  # ctrl-click: MouseMultiCursor
        return mouse(x, y, btn=1)       # middle: PastePrimary
    if r < 0.97:
        c = rnd.choice(COMMANDS)
        return CTRL("e") + c.encode() + ENTER
    # find
    pat = rnd.choice(["foo", "a", "", "x*", "^", "(", "\\b", "é", "[a-z]+", "$"])
    return CTRL("f") + pat.encode() + (ENTER if rnd.random() < 0.8 else ESC)


def main():
    rnd = random.Random(SEED)
    root = tempfile.mkdtemp(prefix="act_fuzz_")
    bindir = os.path.join(root, "bin")
    os.makedirs(bindir)
    for tool in ["sort", "tr", "cat", "echo"]:
        for d in ["/usr/bin", "/bin"]:
            if os.path.exists(os.path.join(d, tool)):
                os.symlink(os.path.join(d, tool), os.path.join(bindir, tool))
                break
    deadline = time.time() + SECONDS
    runs = fails = 0
    while time.time() < deadline:
        runs += 1
        work = os.path.join(root, "w%d" % runs)
        cfg = os.path.join(work, "cfg")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "bindings.json"), "w") as f:
            f.write('{"Ctrl-b": "None", "Alt-x": "SkipMultiCursor"}')
        with open(os.path.join(work, "a.txt"), "w") as f:
            f.write("foo bar\n\tbaz (qux) {x}\nabab foo\n" * rnd.randint(1, 30))
        with open(os.path.join(work, "b.txt"), "wb") as f:
            f.write(bytes(rnd.randint(0, 255) for _ in range(rnd.randint(0, 300))))
        log = os.path.join(work, "san")
        env = {"ASAN_OPTIONS": "log_path=%s:detect_leaks=1" % log,
               "UBSAN_OPTIONS": "log_path=%s:print_stacktrace=1" % log,
               "MICRO_CONFIG_HOME": cfg, "HOME": work, "PATH": bindir, "TMPDIR": work}
        cols, rows = rnd.choice([(80, 24), (40, 10), (120, 40), (20, 6)])
        t = Term([MICRO, "a.txt", "b.txt"] if rnd.random() < 0.5 else [MICRO, "a.txt"],
                 cols=cols, rows=rows, env=env, cwd=work)
        sent = []
        status = None
        n_events = rnd.randint(50, 600)
        for i in range(n_events):
            ev = gen_event(rnd, cols, rows)
            if rnd.random() < 0.01:
                cols, rows = rnd.choice([(80, 24), (30, 8), (100, 30), (10, 4)])
                t.resize(cols, rows)
                sent.append(b"<resize %d %d>" % (cols, rows))
                continue
            sent.append(ev)
            try:
                os.write(t.fd, ev)
            except OSError:
                break
            t.pump(0.004 if rnd.random() < 0.9 else 0.05)
            pid, st = os.waitpid(t.pid, os.WNOHANG)
            if pid:
                status = st
                break
        if status is None:
            t.pump(0.3)
            # leave: force quit everything
            for _ in range(30):
                try:
                    os.write(t.fd, ESC + ESC + CTRL("q") + b"n")
                except OSError:
                    break
                t.pump(0.05)
                pid, st = os.waitpid(t.pid, os.WNOHANG)
                if pid:
                    status = st
                    break
        if status is None:
            # still running (e.g. a prompt we can't escape): kill it, not a failure
            os.kill(t.pid, signal.SIGTERM)
            time.sleep(0.2)
            try:
                pid, st = os.waitpid(t.pid, 0)
                status = st
            except ChildProcessError:
                pass
        t.pump(0.05)
        try:
            os.close(t.fd)
        except OSError:
            pass
        report = ""
        for fn in os.listdir(work):
            if fn.startswith("san"):
                with open(os.path.join(work, fn), errors="replace") as f:
                    report += f.read()
        raw = t.raw.decode("utf-8", "replace")
        k = max(raw.find("runtime error"), raw.find("Sanitizer"))
        if k >= 0:
            report += raw[max(0, k - 300):k + 4000]
        # SIGINT: a Ctrl-C typed while the editor was already restoring the
        # terminal on exit (ISIG back on) - an artifact of the harness
        crashed = status is not None and (os.WIFSIGNALED(status) and
                                          os.WTERMSIG(status) not in (signal.SIGTERM, signal.SIGINT)
                                          or os.WIFEXITED(status) and os.WEXITSTATUS(status) != 0)
        if report or crashed:
            fails += 1
            out = "act_fuzz_fail_%d_%d.bin" % (SEED, runs)
            with open(out, "wb") as f:
                f.write(b"\n----\n".join(sent))
            print("FAIL run %d (status %r), input saved to %s\n%s" % (runs, status, out, report[:6000]))
            sys.stdout.flush()
        shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(root, ignore_errors=True)
    print("seed %d: %d runs, %d failures" % (SEED, runs, fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
