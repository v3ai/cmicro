#!/usr/bin/env python3
"""Timing of large editing operations (actions/commands), driven via a pty.

Usage: python3 tests/act_perf.py path/to/micro [name,...]

Each case opens a generated file, performs an operation and waits until
the editor has processed it (a sentinel message typed afterwards shows up).
"""
import os, sys, time, tempfile, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ESC, UP, DOWN, ENTER, TAB, ALT

MICRO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./micro")
ONLY = sys.argv[2].split(",") if len(sys.argv) > 2 else None
LIMIT = 120.0
_n = [0]


def wait_for(t, needle, limit=LIMIT):
    end = time.time() + limit
    while time.time() < end:
        t.pump(0.02)
        if needle in t.text():
            return True
        if not t.alive():
            return False
    return False


def run_case(name, content, ops, settings=None):
    d = tempfile.mkdtemp(prefix="act_perf_")
    cfg = os.path.join(d, "cfg")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "settings.json"), "w") as f:
        f.write(settings or '{"clipboard": "internal", "savehistory": false}')
    path = os.path.join(d, "f.txt")
    with open(path, "wb") as f:
        f.write(content)
    t = Term([MICRO, path], cols=100, rows=30,
             env={"MICRO_CONFIG_HOME": cfg, "HOME": d, "ASAN_OPTIONS": "detect_leaks=0"}, cwd=d)
    wait_for(t, "f.txt", 60)
    res = []
    for label, keys in ops:
        start = time.time()
        t.send(keys, 0.05)  # (a pause, so a trailing Esc isn't read as an Alt prefix)
        # sentinel: a message only the last command produces
        _n[0] += 1
        marker = "MARK%05d" % _n[0]
        t.send(CTRL("e") + ("cd %s" % marker).encode() + ENTER, 0.0)
        ok = wait_for(t, "Error with cd: %s" % marker)
        res.append((label, time.time() - start if ok else None))
    t.close()
    shutil.rmtree(d, ignore_errors=True)
    for label, dt in res:
        print("%-28s %-34s %s" % (name, label, "%.2fs" % dt if dt is not None else "TIMEOUT/CRASH"))
    sys.stdout.flush()


def main():
    big = b"foo bar foo baz\n" * 1000000          # 16 MB, 1M lines, 2M matches of foo
    med = b"    foo(bar); // baz\n" * 200000
    cases = {
        "replaceall_1M": (big, [("replaceall foo X", CTRL("e") + b"replaceall foo X" + ENTER),
                                ("undo", CTRL("z")), ("redo", CTRL("y"))]),
        "replaceall_longline": (b"ab" * 500000 + b"\n", [("replaceall a xy", CTRL("e") + b"replaceall a xy" + ENTER)]),
        "replaceall_group_1M": (big, [("replaceall (f)oo $1", CTRL("e") + b"replaceall '(f)(o+)' '$2$1'" + ENTER)]),
        "indent_all": (med, [("select all + Tab", CTRL("a") + TAB), ("Shift-Tab", b"\x1b[Z"),
                             ("undo", CTRL("z"))]),
        "comment_all": (med, [("select all + comment", CTRL("a") + ALT("/")), ("uncomment", ALT("/"))]),
        "dupline_half": (med, [("goto 100000", CTRL("e") + b"goto 100000" + ENTER),
                               ("bind F5 DuplicateLine", CTRL("e") + b"bind F5 DuplicateLine" + ENTER),
                               ("select to start + DuplicateLine", b"\x1b[1;6H" + b"\x1b[15~"),
                               ("undo", CTRL("z"))]),
        "movelines_half": (med, [("goto 100000", CTRL("e") + b"goto 100000" + ENTER),
                                 ("select to start + MoveLinesDown", b"\x1b[1;6H" + b"\x1b[1;3B")]),
        "copy_paste_all": (big, [("select all + copy", CTRL("a") + CTRL("c")),
                                 ("paste 16MB", b"\x1b[1;5F" + CTRL("v")), ("undo", CTRL("z"))]),
        "cutline_all": (med, [("select all + cut", CTRL("a") + CTRL("x")), ("paste", CTRL("v"))]),
        "find_nomatch": (big, [("find (no match)", CTRL("f") + b"zzz" + ENTER),
                               ("find next x20", CTRL("n") * 20)]),
        "multicursor_5k": (b"abc def\n" * 5000, [("select all + Alt-m (5k cursors)", CTRL("a") + ALT("m")),
                                                  ("type 5 chars", b"hello"), ("Backspace x5", b"\x7f" * 5),
                                                  ("undo", CTRL("z")), ("Esc", ESC)]),
        "movelines_big": (med, [("select 50k + AltDown", CTRL("a") + b"\x1b[1;5F" + (b"\x1b[1;2A" * 1) + b"\x1b[1;3A")]),
        "retab_1M": (b"\t\tfoo\n" * 500000, [("retab", CTRL("e") + b"set tabstospaces on" + ENTER + CTRL("e") + b"retab" + ENTER)]),
    }
    for name, (content, ops) in cases.items():
        if ONLY and name not in ONLY:
            continue
        run_case(name, content, ops)


if __name__ == "__main__":
    main()
