"""Drive micro in a pseudo-terminal and render its output with pyte."""
import os, pty, select, time, signal, struct, fcntl, termios
import pyte

class Term:
    def __init__(self, argv, cols=80, rows=24, env=None, cwd=None, stdin_data=None):
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.cols, self.rows = cols, rows
        pid, fd = pty.fork()
        if pid == 0:
            if cwd: os.chdir(cwd)
            e = dict(os.environ)
            e.update({"TERM": "xterm-256color", "HOME": "/tmp/microhome", "COLORTERM": ""})
            e.pop("DISPLAY", None); e.pop("WAYLAND_DISPLAY", None)
            if env: e.update(env)
            os.execve(argv[0], argv, e)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.raw = b""
        self.pump(0.4)

    def pump(self, t=0.15):
        end = time.time() + t
        while True:
            r, _, _ = select.select([self.fd], [], [], max(0, end - time.time()))
            if not r:
                if time.time() >= end: break
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                break
            if not data: break
            self.raw += data
            self.stream.feed(data)

    def send(self, s, t=0.12):
        if isinstance(s, str): s = s.encode()
        os.write(self.fd, s)
        self.pump(t)

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def line(self, y):
        return self.screen.display[y].rstrip()

    def cursor(self):
        return (self.screen.cursor.x, self.screen.cursor.y)

    def resize(self, cols, rows):
        self.screen.resize(rows, cols)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)
        self.pump(0.3)

    def alive(self):
        try:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            return pid == 0
        except ChildProcessError:
            return False

    def close(self):
        try: os.kill(self.pid, signal.SIGKILL)
        except Exception: pass
        try: os.waitpid(self.pid, 0)
        except Exception: pass

CTRL = lambda c: bytes([ord(c.lower()) - 96])
ESC = b"\x1b"
UP, DOWN, RIGHT, LEFT = b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D"
HOME, END = b"\x1b[H", b"\x1b[F"
ENTER, TAB, BS = b"\r", b"\t", b"\x7f"
def ALT(c): return b"\x1b" + c.encode()
def SHIFT(k): return k[:-1] + b"1;2" + k[-1:]
def CTRLK(k): return k[:-1] + b"1;5" + k[-1:]
def mouse(x, y, btn=0, release=False):
    return ("\x1b[<%d;%d;%d%s" % (btn, x + 1, y + 1, "m" if release else "M")).encode()
