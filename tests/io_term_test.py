"""End-to-end I/O checks of the real editor in a pty (needs pyte):
   python3 tests/io_term_test.py [path/to/micro]"""
import os, sys, signal, tempfile, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from term import Term, CTRL, ENTER

MICRO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "micro")
fails = 0

def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)

def wait_exit(t, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            pid, st = os.waitpid(t.pid, os.WNOHANG)
        except ChildProcessError:
            return True
        if pid:
            return True
        t.pump(0.05)
    return False

with tempfile.TemporaryDirectory() as d:
    env = {"HOME": d, "XDG_CONFIG_HOME": "", "MICRO_CONFIG_HOME": ""}
    f = os.path.join(d, "f.txt")
    with open(f, "w") as fh:
        fh.write("one\ntwo\nthree\n")

    # +LINE:COL far out of range: no overflow, cursor on the last line
    t = Term([MICRO, "+99999999999999999999:99999999999999", f], env=env)
    check(t.alive(), "+LINE:COL overflow kills the editor")
    t.send(CTRL("q"))
    check(wait_exit(t), "quit after +LINE")
    t.close()

    # piped stdin keeps NUL bytes; save it under a name
    out = os.path.join(d, "piped.out")
    t = Term(["/bin/sh", "-c", "printf 'a\\000b\\nline2\\n' | exec \"$0\"", MICRO], env=env)
    t.send(CTRL("s"), 0.3)
    t.send(out.encode() + ENTER, 0.5)
    t.send(CTRL("q"))
    wait_exit(t)
    t.close()
    data = open(out, "rb").read() if os.path.exists(out) else b""
    check(data == b"a\x00b\nline2\n", "piped stdin with NUL saved as %r" % data)

    # SIGTERM with unsaved changes: backup written, editor exits
    t = Term([MICRO, f], env=env)
    t.send(b"XYZ")
    os.kill(t.pid, signal.SIGTERM)
    check(wait_exit(t), "exit on SIGTERM")
    bdir = os.path.join(d, ".config", "micro", "backups")
    names = os.listdir(bdir) if os.path.isdir(bdir) else []
    check(len(names) == 1, "one backup after SIGTERM: %r" % names)
    if names:
        st = os.stat(os.path.join(bdir, names[0]))
        check(st.st_mode & 0o777 == 0o600, "backup mode %o" % (st.st_mode & 0o777))
        check(open(os.path.join(bdir, names[0]), "rb").read().startswith(b"XYZone\n"), "backup content")
        check(os.stat(bdir).st_mode & 0o777 == 0o700, "backup dir mode")
    check(t.raw.rstrip().endswith(b"\x1b[?1049l"), "terminal restored on SIGTERM")
    t.close()
    check(open(f).read() == "one\ntwo\nthree\n", "file untouched by SIGTERM")

    # the terminal going away (pty closed, no SIGHUP): exit, don't spin
    g = os.path.join(d, "g.txt")
    open(g, "w").write("x\n")
    t = Term(["/bin/sh", "-c", "trap '' HUP; exec \"$0\" \"$1\"", MICRO, g], env=env)
    os.close(t.fd)
    end = time.time() + 3
    gone = False
    while time.time() < end:
        try:
            pid, st = os.waitpid(t.pid, os.WNOHANG)
        except ChildProcessError:
            gone = True
            break
        if pid:
            gone = True
            break
        time.sleep(0.05)
    check(gone, "editor exits when its terminal disappears")
    if not gone:
        os.kill(t.pid, signal.SIGKILL)
        os.waitpid(t.pid, 0)

print("io_term_test: %d failures" % fails)
sys.exit(1 if fails else 0)
