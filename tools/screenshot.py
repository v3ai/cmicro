#!/usr/bin/env python3
"""Render a screenshot of the editor for the README (docs/screenshot.png).

Runs ./micro in a pseudo-terminal, replays a few keystrokes, and draws the
resulting screen (characters, 24-bit colors, attributes, cursor) with
Pillow.  Needs: pip install pyte pillow.  Run from the repository root
after `make`.
"""
import copy
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from term import Term, CTRL, ENTER  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

COLS, ROWS = 104, 30
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SIZE = 26  # rendered at 2x for sharp text on high-density screens
DEFAULT_FG, DEFAULT_BG = (0xF8, 0xF8, 0xF2), (0x28, 0x28, 0x28)
ANSI = ["000000", "cd0000", "00cd00", "cdcd00", "0000ee", "cd00cd", "00cdcd", "e5e5e5",
        "7f7f7f", "ff0000", "00ff00", "ffff00", "5c5cff", "ff00ff", "00ffff", "ffffff"]
NAMES = {"black": 0, "red": 1, "green": 2, "brown": 3, "blue": 4, "magenta": 5, "cyan": 6, "white": 7}


def color(c, default):
    if c == "default":
        return default
    if c in NAMES:
        c = ANSI[NAMES[c]]
    if isinstance(c, str) and len(c) == 6:
        return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))
    return default


def capture():
    home = tempfile.mkdtemp()
    t = Term([os.path.abspath("micro"), "src/regex.c", "src/syntax.c"], cols=COLS, rows=ROWS,
             env={"HOME": home, "COLORTERM": "truecolor"}, cwd=os.getcwd())
    def cmd(s):
        t.send(CTRL("e"))
        t.send(s.encode())
        t.send(ENTER, 0.3)
    cmd("set hlsearch on")
    t.send(CTRL("f")); t.send(b"visit"); t.send(ENTER, 0.3)  # highlights all matches
    cmd("goto 952")                   # scroll so re_search() starts near the top
    t.send(b"\x1b[A" * 12, 0.4)       # move the cursor up into the function body
    t.send(b"\x1b[C" * 9, 0.3)
    cmd("vsplit tools/embed.py")
    t.send(CTRL("w"))                 # focus back on the C file
    t.pump(0.5)
    screen = copy.deepcopy(t.screen)  # quitting clears the live screen
    t.send(CTRL("q")); t.send(CTRL("q")); t.send(CTRL("q"))
    t.close()
    shutil.rmtree(home, ignore_errors=True)
    return screen


def render(screen, out):
    font = ImageFont.truetype(FONT, SIZE)
    bold = ImageFont.truetype(FONT_BOLD, SIZE)
    cw = round(font.getlength("M"))
    ch = round(SIZE * 1.3)
    pad, bar = 28, 64
    W, H = COLS * cw + 2 * pad, ROWS * ch + 2 * pad + bar
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    # window frame with a title bar
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=22, fill=DEFAULT_BG)
    d.rounded_rectangle([0, 0, W - 1, bar + 22], radius=22, fill=(0x3a, 0x3a, 0x3a))
    d.rectangle([0, bar, W - 1, bar + 22], fill=DEFAULT_BG)
    for i, c in enumerate([(0xff, 0x5f, 0x57), (0xfe, 0xbc, 0x2e), (0x28, 0xc8, 0x40)]):
        cx = pad + 14 + i * 42
        d.ellipse([cx - 13, bar // 2 - 13, cx + 13, bar // 2 + 13], fill=c)
    title = "micro src/regex.c src/syntax.c"
    d.text(((W - font.getlength(title)) / 2, bar // 2), title, font=font, fill=(0xbb, 0xbb, 0xbb), anchor="lm")
    ox, oy = pad, bar + pad
    for y in range(ROWS):
        row = screen.buffer[y]
        for x in range(COLS):
            c = row[x]
            fg, bg = color(c.fg, DEFAULT_FG), color(c.bg, DEFAULT_BG)
            if c.reverse:
                fg, bg = bg, fg
            if (x, y) == (screen.cursor.x, screen.cursor.y):
                fg, bg = DEFAULT_BG, DEFAULT_FG
            px, py = ox + x * cw, oy + y * ch
            d.rectangle([px, py, px + cw, py + ch], fill=bg)
            if c.data and c.data != " ":
                d.text((px, py + ch / 2), c.data, font=bold if c.bold else font, fill=fg, anchor="lm")
            if c.underscore:
                d.line([px, py + ch - 3, px + cw, py + ch - 3], fill=fg, width=2)
    img.save(out, optimize=True)
    print("wrote", out, img.size)


if __name__ == "__main__":
    render(capture(), sys.argv[1] if len(sys.argv) > 1 else "docs/screenshot.png")
