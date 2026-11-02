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

def wait_status(t, secs=3.0):
    """the wait status once the editor exits, or None"""
    end = time.time() + secs
    while time.time() < end:
        try:
            pid, st = os.waitpid(t.pid, os.WNOHANG)
        except ChildProcessError:
            return -1
        if pid:
            t.pump(0.1)
            return st
        t.pump(0.05)
    return None

def wait_exit(t, secs=3.0):
    return wait_status(t, secs) is not None

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
    st = wait_status(t)
    check(st is not None, "exit on SIGTERM")
    # after saving backups it dies of the signal, so shells and
    # supervisors (and act_fuzz.py) see a SIGTERM, not exit status 1
    check(st is not None and st != -1 and os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGTERM,
          "killed by SIGTERM, status %r" % st)
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

    # a crash restores the terminal like a normal exit, autowrap included
    h = os.path.join(d, "h.txt") # f has a backup now: no recovery prompt
    open(h, "w").write("x\n")
    t = Term([MICRO, h], env=env)
    t.pump(0.3)
    check(t.alive() and "x" in t.line(0), "editor up before the crash")
    n = len(t.raw)
    os.kill(t.pid, signal.SIGSEGV)
    t.pump(0.5) # read the output before reaping: the pty may drop it after
    st = wait_status(t)
    check(st is not None and st != -1 and os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGSEGV,
          "dies of SIGSEGV, status %r" % st)
    tail = t.raw[n:]
    check(b"\x1b[?7h" in tail and b"\x1b[r" in tail and tail.rstrip().endswith(b"\x1b[?1049l"),
          "crash reset sequence %r" % tail)
    t.close()

    # file names in messages printed outside the editor: control bytes,
    # C1 and invalid UTF-8 (raw 0x9B is CSI on 8-bit terminals) become '?'
    evil = os.path.join(d.encode(), b"d\xc3\xa9\x9b2J\x9d0;x\x9c\x07\xc2\x9b\xef\xbf\xbd")
    os.mkdir(evil)
    t = Term([MICRO.encode(), evil], env=env)
    t.pump(0.3)
    i = t.raw.find(b"micro: ")
    msg = t.raw[i:t.raw.find(b"\n", i)] if i >= 0 else b""
    check(b"d\xc3\xa9?2J?0;x???\xef\xbf\xbd is a directory" in msg, "sanitized error message %r" % msg)
    t.close()

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
