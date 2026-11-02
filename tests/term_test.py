"""Regression tests for the terminal front end (screen, input, rendering).

usage: python3 tests/term_test.py ./micro

Each test drives the editor in a pty (see term.py). Best run against a
sanitizer build:
    clang -O1 -g -fsanitize=address,undefined -o micro_asan src/*.c
"""
import os, signal, shutil, sys, tempfile, time, fcntl, struct, termios
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ENTER, DOWN, UP
from term_stress import check_output, quiet

BIN = None
TMP = None


def editor(files, cols=80, rows=24, extra=()):
    home = tempfile.mkdtemp(dir=TMP)
    logdir = os.path.join(home, "asan")
    os.mkdir(logdir)
    env = {"HOME": home, "ASAN_OPTIONS": "log_path=%s/asan:detect_leaks=0" % logdir,
           "UBSAN_OPTIONS": "log_path=%s/ubsan" % logdir}
    t = Term([BIN] + list(extra) + list(files), cols=cols, rows=rows, env=env, cwd=TMP)
    t.logdir = logdir
    quiet(t)
    return t


def finish(t):
    alive = t.alive()
    t.close()
    reps = [open(os.path.join(t.logdir, n), errors="replace").read() for n in os.listdir(t.logdir)]
    assert not reps, "sanitizer report:\n" + "\n".join(r[:3000] for r in reps)
    return alive


def resize(t, w, h):
    t.screen.resize(min(h, 500), min(w, 500))
    fcntl.ioctl(t.fd, termios.TIOCSWINSZ, struct.pack("HHHH", h, w, 0, 0))
    os.kill(t.pid, signal.SIGWINCH)
    quiet(t)


def full_redraw_matches(t):
    """the incrementally updated screen must equal a full redraw"""
    quiet(t)
    before = t.screen.display[:]
    os.kill(t.pid, signal.SIGWINCH)  # same size: forces a full redraw
    quiet(t)
    after = t.screen.display
    for y in range(len(before)):
        if before[y] != after[y]:
            print("    row %d: %r\n        vs %r" % (y, before[y], after[y]))
    return before == after


def write(name, data):
    p = os.path.join(TMP, name)
    with open(p, "wb") as f:
        f.write(data)
    return p


def test_control_chars_are_sanitized():
    p = write("ctl.txt", b"esc \x1b]0;INJECTTITLE\x07 csi \x1b[31mRED\x1b[0m\n"
              b"c1 \xc2\x9b31m \xc2\x9d0;x\xc2\x9c raw \x9b\x9d del \x7f nul \x00 bs \x08 cr \r\n"
              b"bidi \xe2\x80\xae rlo \xe2\x81\xa6 lri \xef\xbb\xbf bom \xed\xa0\x80 surrogate\n")
    q = write("n\x1b]0;FNINJECT\x07\x1b[2J.txt", b"x\n")
    t = editor([p, q])
    t.send(CTRL("e") + b"set statusformatl \x1b[5mBLINK$(filename)" + ENTER, 0.3)
    t.send(CTRL("e") + b"set tabalways on" + ENTER, 0.3)
    quiet(t)
    assert "INJECTTITLE" in t.text()
    bad = check_output(t.raw)
    assert not bad, bad
    assert finish(t)


def test_tiny_and_huge_sizes():
    p = write("small.c", b"int main() {\n\treturn 0;\n}\n" * 30)
    t = editor([p])
    t.send(CTRL("e") + b"vsplit" + ENTER, 0.2)
    t.send(CTRL("e") + b"hsplit" + ENTER, 0.2)
    for w, h in ((1, 1), (2, 1), (1, 3), (3, 2), (65535, 65535), (10000, 10000), (5, 5), (65535, 1),
                 (1, 65535), (80, 24)):
        resize(t, w, h)
        t.send(DOWN * 3 + b"x", 0.05)
        assert t.alive(), (w, h)
    assert full_redraw_matches(t)
    assert finish(t)


def test_hostile_mouse_and_input():
    p = write("m.txt", b"hello\n" * 100)
    t = editor([p], extra=["-mouse", "on"])
    for seq in (b"\x1b[<0;99999999999999999999;99999999999999999999M", b"\x1b[<0;0;0M",
                b"\x1b[<64;-5;-5M", b"\x1b[<35;5;5M", b"\x1b[M\xff\xff\xff", b"\x1b[M   ",
                b"\x1b[<2;1000000;1000000m", b"\x1b[<0;1;1M"):
        t.send(seq, 0.05)
    # a flood of ignored sequences used to recurse once per sequence
    t.send(b"\x1b[?1u" * 400000, 1.0)
    quiet(t)
    t.send(b"\x1b[200~" + b"y" * 300000, 3.0)  # unterminated paste
    quiet(t)
    assert t.alive()
    assert finish(t)


def test_huge_tabsize_and_margins():
    p = write("tabs.txt", b"\t\t\tx\t\ty\n" * 200)
    t = editor([p])
    for cmd in (b"set tabsize 2147483647", b"set scrollmargin 2147483647", b"set colorcolumn 2147483647",
                b"set softwrap on", b"set ruler on", b"set relativeruler on", b"set scrollbar on",
                b"set statusformatr " + b"$(line)$(col)$(status.vcol)" * 200):
        t.send(CTRL("e") + cmd + ENTER, 0.2)
    t0 = time.time()
    t.send(b"\x1b[6~\x1b[6~" + DOWN * 5 + b"\x1b[F", 0.5)
    quiet(t)
    assert time.time() - t0 < 20, "too slow"
    assert t.alive()
    assert full_redraw_matches(t)
    assert finish(t)


def test_scrolling_output_is_consistent():
    lines = b"".join(b"line %d \xe4\xb8\xad\xe6\x96\x87 e\xcc\x81 \t%s\n" % (i, b"=" * (i % 70)) for i in range(400))
    p = write("scroll.c", lines)
    t = editor([p], cols=100, rows=30)
    t.send(DOWN * 40, 0.3)
    assert full_redraw_matches(t)
    t.send(UP * 35, 0.3)
    assert full_redraw_matches(t)
    t.send(b"\x1b[6~" * 3, 0.3)
    assert full_redraw_matches(t)
    t.send(b"\x1b[<65;10;10M" * 5 + b"\x1b[<64;10;10M" * 2, 0.3)  # mouse wheel
    assert full_redraw_matches(t)
    t.send(CTRL("e") + b"hsplit" + ENTER + DOWN * 30, 0.3)
    assert full_redraw_matches(t)
    # nothing changes: nothing is written
    quiet(t)
    n = len(t.raw)
    t.pump(1.2)
    assert len(t.raw) == n, ("idle output", t.raw[n:])
    assert finish(t)


def test_wide_chars_at_the_edge():
    p = write("wide.txt", (b"a" + b"\xe4\xb8\xad" * 50 + b"\n") * 5 + (b"\xe4\xb8\xad" * 50 + b"\n") * 5)
    t = editor([p], cols=41, rows=12, extra=["-ruler", "off"])
    assert full_redraw_matches(t)
    t.send(b"\x1b[F", 0.2)  # End: scroll horizontally
    assert full_redraw_matches(t)
    t.send(CTRL("e") + b"set softwrap on" + ENTER, 0.3)
    assert full_redraw_matches(t)
    assert finish(t)


def test_many_tabs_and_splits():
    p = write("t.txt", b"x\n")
    t = editor([p], cols=60, rows=20)
    for i in range(25):
        t.send(CTRL("t"), 0.02)
    t.send(CTRL("e") + b"vsplit" + ENTER, 0.05)
    for i in range(10):
        t.send(CTRL("e") + b"vsplit" + ENTER, 0.03)
        t.send(CTRL("e") + b"hsplit" + ENTER, 0.03)
    t.send(b"\x1b[<0;1;1M" + b"\x1b[<0;59;1M", 0.1)
    assert t.alive()
    assert "[" in t.line(0)  # the current tab stays visible
    for i in range(80):
        t.send(CTRL("q"), 0.01)
    quiet(t)
    finish(t)


def main():
    global BIN, TMP
    BIN = os.path.abspath(sys.argv[1])
    TMP = tempfile.mkdtemp(prefix="microtermtest")
    failed = 0
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]
    for name, fn in tests:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as e:
            failed += 1
            print("FAIL", name, str(e)[:3000])
    shutil.rmtree(TMP, ignore_errors=True)
    print("%d/%d passed" % (len(tests) - failed, len(tests)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
