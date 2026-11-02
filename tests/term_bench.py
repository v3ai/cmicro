"""Measure terminal output volume and CPU time of common operations.

usage: python3 tests/term_bench.py ./micro FILE [softwrap]

Drives the editor in a 200x60 pty (see term.py) and reports, per scenario,
the number of bytes written to the terminal and the child's CPU time,
in total and per key sent (keys are sent slowly enough that each one
normally gets its own frame). The final screen is also checked against a fresh
full redraw of the same state, so output optimizations can't silently
corrupt the display.
"""
import os, shutil, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, UP, DOWN, RIGHT, LEFT

PGDN, PGUP = b"\x1b[6~", b"\x1b[5~"


def cpu(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(")", 1)[1].split()
    tck = os.sysconf("SC_CLK_TCK")
    return (int(parts[11]) + int(parts[12])) / tck


def run(binary, path, extra):
    home = tempfile.mkdtemp(prefix="microbench")  # fresh: no backups or saved cursors
    t = Term([binary] + extra + [path], cols=200, rows=60, env={"HOME": home})
    t.pump(1.0)
    results = []

    def scenario(name, keys, delay=0.004):
        t.pump(0.3)
        b0, c0 = len(t.raw), cpu(t.pid)
        w0 = time.time()
        for k in keys:
            os.write(t.fd, k)
            t.pump(delay)
        t.pump(0.6)
        b1, c1 = len(t.raw), cpu(t.pid)
        results.append((name, b1 - b0, len(keys), c1 - c0, time.time() - w0))

    scenario("pagedown x100", [PGDN] * 100, 0.02)
    scenario("pageup x50", [PGUP] * 50, 0.02)
    scenario("down x300 (hold)", [DOWN] * 300, 0.003)
    scenario("up x300 (hold)", [UP] * 300, 0.003)
    scenario("right x200", [RIGHT] * 200, 0.003)
    scenario("type 200 chars", [b"a"] * 200, 0.01)
    scenario("ctrl-end/home x20", [b"\x1b[1;5F", b"\x1b[1;5H"] * 10, 0.05)
    screen_before = t.text()
    # force a full redraw by resizing away and back
    t.resize(199, 60)
    t.resize(200, 60)
    ok = t.text() == screen_before
    t.close()
    shutil.rmtree(home, ignore_errors=True)
    return results, ok


def main():
    binary, path = sys.argv[1], sys.argv[2]
    extra = []
    if len(sys.argv) > 3 and sys.argv[3] == "softwrap":
        extra = ["-softwrap", "on"]
    res, ok = run(binary, path, extra)
    print("%-22s %12s %7s %9s %9s %10s" % ("scenario", "bytes", "keys", "B/key", "cpu s", "cpu ms/key"))
    for name, nb, fr, c, w in res:
        print("%-22s %12d %7d %9d %9.2f %10.2f" % (name, nb, fr, nb // max(fr, 1), c, 1000 * c / max(fr, 1)))
    print("screen consistent after full redraw:", ok)


if __name__ == "__main__":
    main()
