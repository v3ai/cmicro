"""Randomized pty stress test for the terminal front end.

usage: python3 tests/term_stress.py ./micro [seconds] [seed]

Build the editor with sanitizers first, e.g.
    make clean && make CC=clang CFLAGS="-O1 -g -fsanitize=address,undefined"

Drives the editor (see term.py) with resize storms (TIOCSWINSZ + SIGWINCH
down to 1x1 and up to very large sizes), random keys, random and hostile
mouse reports, splits and tabs opened and closed, and option changes that
stress the layout (softwrap, huge tabsize, scrollmargin, status formats).
The files opened contain control characters, escape sequences and invalid
UTF-8, and one of them has an escape sequence in its name.

Fails if the editor dies, a sanitizer reports anything (ASAN_OPTIONS
log_path), or the output contains terminal escape sequences we never
generate ourselves (i.e. text from a file or file name leaked through
unsanitized).
"""
import fcntl, os, random, re, shutil, signal, struct, sys, tempfile, termios, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ESC, UP, DOWN, LEFT, RIGHT, ENTER, mouse

KEYS = [UP, DOWN, LEFT, RIGHT, b"\x1b[5~", b"\x1b[6~", b"\x1b[H", b"\x1b[F",
        b"\x1b[1;5F", b"\x1b[1;5H", b"\x1b[1;2B", b"\x1b[1;2C", ENTER, b"\t",
        b"\x7f", b"x", b"\xe4\xb8\xad", b"e\xcc\x81", b"\x1b[3~", CTRL("y"),
        CTRL("a"), CTRL("d"), CTRL("k"), CTRL("w"), b"\x1b[1;3A", b"\x1b[1;3B",
        b"\x1b[200~pasted\r\ntext\x1b[201~", CTRL("c"), CTRL("v"), CTRL("f") + b"a" + ENTER,
        ESC]

COMMANDS = [
    "vsplit", "hsplit", "tab", "quit", "set softwrap on", "set softwrap off",
    "set wordwrap on", "set tabsize 1", "set tabsize 64", "set tabsize 4",
    "set scrollmargin 1000", "set scrollmargin 3", "set ruler off", "set ruler on",
    "set relativeruler on", "set colorcolumn 3", "set colorcolumn 0", "set scrollbar on",
    "set scrollbar off", "set statusline off", "set statusline on", "set cursorline on",
    "set keymenu on", "set keymenu off", "set infobar off", "set infobar on",
    "set tabalways on", "set hltrailingws on", "set hlsearch on", "set diffgutter on",
    "set statusformatl " + "$(filename)$(modified)$(line)$(col)$(percentage)" * 40,
    "set statusformatr $(opt:filetype)$(bind:Save)$(status.bytes)$(status.size)$(status.vcol)",
    "set statusformatl $(", "set statusformatr )$($)$(nonexistent)",
    "set showchars tab=>,space=.,trailing=~", "set indentchar \x01",
    "set divchars \x1b\x1b", "set scrollbarchar \x1b", "goto 999999", "goto 1",
    "set matchbrace on", "set fastdirty on", "set basename on", "set mouse on",
]

SIZES = [(1, 1), (2, 1), (1, 2), (3, 3), (5, 2), (10, 4), (20, 5), (80, 24),
         (200, 60), (7, 30), (300, 3), (1000, 300), (2000, 2000), (65535, 2),
         (2, 30000)]


def make_files(d):
    files = []
    p = os.path.join(d, "ctl.txt")
    with open(p, "wb") as f:
        f.write(b"plain line\n")
        f.write(b"esc \x1b]0;INJECTTITLE\x07 and \x1b[31mRED\x1b[0m\n")
        f.write(b"c1 \xc2\x9b31mC1CSI \xc2\x9d0;OSC\xc2\x9c\n")
        f.write(b"raw c1 \x9b\x9d\x90 del \x7f nul \x00 bel \x07 bs \x08 cr \r mid\n")
        f.write(b"invalid \xff\xfe\xc0\xaf \xed\xa0\x80 surrogate \xf4\x90\x80\x80\n")
        f.write(b"wide \xe4\xb8\xad\xe6\x96\x87" * 40 + b"\n")
        f.write(b"combining e\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81 end\n")
        f.write(b"bidi \xe2\x80\xae reversed \xe2\x80\xac zwj \xe2\x80\x8d\n")
        f.write(b"\t\t\ttabs\t\tx\n")
        f.write(b"long " + b"abc\xe4\xb8\xad\t" * 5000 + b"\n")
        for i in range(300):
            f.write(b"line %d {(\"x\")}  \n" % i)
    files.append(p)
    p = os.path.join(d, "name\x1b]0;FNINJECT\x07\x1b[2J.c")
    with open(p, "wb") as f:
        f.write(b"int main() { return 0; }\n" * 50)
    files.append(p)
    return files


SAFE_ESC = re.compile(rb"\x1b(\[[0-9;?]*[A-Za-z@`]|\]52;c;[A-Za-z0-9+/=]*\x07|[78M=>])")


def check_output(raw):
    """Every escape we emit must be one of ours; no C1 controls or other C0."""
    bad = []
    text = []
    pos = 0
    while True:
        j = raw.find(b"\x1b", pos)
        if j < 0:
            text.append(raw[pos:])
            break
        text.append(raw[pos:j])
        m = SAFE_ESC.match(raw, j)
        if not m:
            bad.append(("ESC", raw[max(0, j - 20):j + 30]))
            pos = j + 1
            continue
        pos = m.end()
    text = b"".join(text)
    m = re.search(rb"[\x00-\x08\x0b-\x0c\x0e-\x1a\x1c-\x1f\x7f]", text)
    if m:
        bad.append(("C0", text[max(0, m.start() - 20):m.start() + 20]))
    m = re.search(rb"\xc2[\x80-\x9f]", text)
    if m:
        bad.append(("C1", text[max(0, m.start() - 20):m.start() + 20]))
    for marker in (b"\x1b]0;INJECTTITLE", b"FNINJECT\x07", b"\x1b[31mRED"):
        if marker in raw:
            bad.append(("marker", marker))
    return bad[:5]


def quiet(t, idle=0.3, limit=5.0):
    """pump until the editor has written nothing for `idle` seconds"""
    end = time.time() + limit
    while time.time() < end:
        n = len(t.raw)
        t.pump(idle)
        if len(t.raw) == n:
            return


def asan_reports(logdir):
    out = []
    for n in os.listdir(logdir):
        with open(os.path.join(logdir, n), errors="replace") as f:
            out.append(f.read())
    return out


def main():
    binary = os.path.abspath(sys.argv[1])
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else int(time.time())
    rnd = random.Random(seed)
    d = tempfile.mkdtemp(prefix="microstress")
    logdir = os.path.join(d, "asan")
    os.mkdir(logdir)
    home = os.path.join(d, "home")
    os.mkdir(home)
    env = {"HOME": home, "ASAN_OPTIONS": "log_path=%s/asan:detect_leaks=0:abort_on_error=0" % logdir,
           "UBSAN_OPTIONS": "log_path=%s/ubsan:print_stacktrace=1" % logdir}
    end = time.time() + duration
    runs = 0
    failures = 0
    print("seed", seed)
    while time.time() < end:
        runs += 1
        # fresh files each run: the previous run was SIGKILLed with unsaved
        # changes, and reopening the same paths would stop at the (cooked
        # mode) backup recovery prompt
        rd = os.path.join(d, "run%d" % runs)
        os.mkdir(rd)
        files = make_files(rd)
        t = Term([binary] + files, cols=80, rows=24, env=env, cwd=rd)
        log = []
        mismatches = []
        size = (80, 24)
        for step in range(rnd.randint(50, 250)):
            if not t.alive():
                break
            r = rnd.random()
            if r < 0.05 and size[0] <= 300 and size[1] <= 120:
                # incremental output must leave the screen exactly as a full
                # redraw does (a same-size SIGWINCH forces one)
                quiet(t)
                before = t.screen.display[:]
                n0 = len(t.raw)
                os.kill(t.pid, signal.SIGWINCH)
                quiet(t)
                exited = b"\x1b[?1049l" in t.raw[n0:] or not t.alive()
                if not exited and t.screen.display != before:
                    diff = [(y, before[y], t.screen.display[y]) for y in range(len(before))
                            if before[y] != t.screen.display[y]]
                    mismatches.append((step, diff[:3], log[:]))
                continue
            if r < 0.12:
                w, h = rnd.choice(SIZES) if rnd.random() < 0.6 else (rnd.randint(1, 300), rnd.randint(1, 100))
                size = (w, h)
                log.append(("resize", w, h))
                t.screen.resize(min(h, 500), min(w, 500))
                fcntl.ioctl(t.fd, termios.TIOCSWINSZ, struct.pack("HHHH", h, w, 0, 0))
                os.kill(t.pid, signal.SIGWINCH)
                t.pump(0.02)
            elif r < 0.30:
                x, y = rnd.randint(0, 300), rnd.randint(0, 100)
                kind = rnd.choice([0, 0, 0, 1, 2, 32, 64, 65, 66, 67, 3, 35, 4, 8, 16])
                if rnd.random() < 0.1:
                    x, y = rnd.choice([(-1, -1), (10 ** 9, 10 ** 9), (0, 0), (99999999, 3)])
                seq = ("\x1b[<%d;%d;%d%s" % (kind, x + 1, y + 1, rnd.choice("Mm"))).encode()
                log.append(("mouse", seq))
                t.send(seq, 0.01)
            elif r < 0.40:
                c = rnd.choice(COMMANDS)
                log.append(("cmd", c))
                t.send(CTRL("e"), 0.01)
                t.send(c.encode() + ENTER, 0.03)
            elif r < 0.44:
                log.append(("key", "ctrl-t/ctrl-q"))
                t.send(rnd.choice([CTRL("t"), CTRL("q"), CTRL("w"), b"\x1b,", b"\x1b."]), 0.02)
                if rnd.random() < 0.5:
                    t.send(b"n", 0.01)
            elif r < 0.46:
                # (no Ctrl-Z / Ctrl-B: suspending or running a shell leaves
                # raw mode, and the echo of later input or the shell's output
                # would be blamed on the editor)
                junk = bytes(rnd.choice([c for c in range(256) if c not in (0x1a, 0x02)])
                             for _ in range(rnd.randint(1, 40)))
                log.append(("junk", junk))
                t.send(junk, 0.02)
            else:
                k = rnd.choice(KEYS)
                log.append(("key", k))
                t.send(k * rnd.randint(1, 5), 0.005)
        t.pump(0.2)
        alive = t.alive()
        t.close()
        bad = check_output(t.raw)
        reps = asan_reports(logdir)
        if reps or bad or mismatches:
            failures += 1
            print("=== run %d FAILED (alive=%s)" % (runs, alive))
            for b in bad:
                print("  output:", b)
            for m in mismatches[:2]:
                print("  screen differs from a full redraw at step %d:" % m[0])
                for y, a, b in m[1]:
                    print("    row %d: %r" % (y, a))
                    print("        vs %r" % b)
                print("  steps before it:", m[2][-25:])
            for rep in reps:
                print(rep[:4000])
            print("  last steps:", log[-15:])
            for n in os.listdir(logdir):
                os.remove(os.path.join(logdir, n))
            if failures >= 3:
                break
    shutil.rmtree(d, ignore_errors=True)
    print("runs:", runs, "failures:", failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
