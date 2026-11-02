/*
 * screen.c - double-buffered cell grid rendered with ANSI escape codes.
 */
#include "micro.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char g[16];  /* UTF-8 glyph (base + combining marks) */
    signed char width; /* 1, 2, or -1 for the right half of a wide char */
    Style st;
} Cell;

static Cell *front, *back;
static int W, H;
static int cursor_x = -1, cursor_y = -1;
static int full_redraw = 1;
static Str out;
int scr_truecolor = 0;

int scr_detect_truecolor(const char *opt) {
    const char *ct = getenv("COLORTERM");
    if (!strcmp(opt, "on")) return 1;
    if (strcmp(opt, "auto") != 0) return 0;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return 1;
    return getenv("WT_SESSION") != NULL; /* Windows Terminal */
}

static void emit(const char *s) { str_puts(&out, s); }

static void flush_out(void) {
    if (out.len) plat_write(out.s, (size_t)out.len);
    str_clear(&out);
}

static void alloc_grids(void) {
    free(front);
    free(back);
    front = xcalloc((size_t)(W * H + 1), sizeof *front);
    back = xcalloc((size_t)(W * H + 1), sizeof *back);
    full_redraw = 1;
}

void scr_init(void) {
    str_init(&out);
    plat_term_size(&W, &H);
    if (W < 1) W = 80;
    if (H < 1) H = 24;
    alloc_grids();
    /* alternate screen, clear, bracketed paste */
    emit("\x1b[?1049h\x1b[H\x1b[2J\x1b[?2004h");
    flush_out();
}

void scr_set_mouse(int on) {
    if (on) emit("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    else emit("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    flush_out();
}

void scr_fini(void) {
    emit("\x1b[0m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?2004l\x1b[?25h\x1b[2J\x1b[?1049l");
    flush_out();
}

void scr_resize(void) {
    int w, h;
    plat_term_size(&w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    W = w;
    H = h;
    alloc_grids();
}

int scr_w(void) { return W; }
int scr_h(void) { return H; }

void scr_invalidate(void) { full_redraw = 1; }

void scr_clear(void) {
    int i;
    for (i = 0; i < W * H; i++) {
        back[i].g[0] = ' ';
        back[i].g[1] = 0;
        back[i].width = 1;
        back[i].st = def_style;
    }
}

void scr_set(int x, int y, int cp, Style st) {
    Cell *c;
    int w;
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    w = char_width(cp);
    if (w == 0 && cp != 0) {
        /* combining mark: append to previous cell */
        if (x > 0) {
            Cell *p = &back[y * W + x - 1];
            if (p->width == -1 && x > 1) p = &back[y * W + x - 2];
            {
                size_t len = strlen(p->g);
                char buf[4];
                int n = utf8_encode(cp, buf);
                if (len + (size_t)n < sizeof p->g) {
                    memcpy(p->g + len, buf, (size_t)n);
                    p->g[len + (size_t)n] = 0;
                }
            }
        }
        return;
    }
    if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0)) cp = '?';
    c = &back[y * W + x];
    c->g[utf8_encode(cp, c->g)] = 0;
    c->st = st;
    c->width = 1;
    if (w == 2) {
        if (x + 1 < W) {
            Cell *n = &back[y * W + x + 1];
            n->g[0] = 0;
            n->width = -1;
            n->st = st;
            c->width = 2;
        } else {
            c->g[0] = ' ';
            c->g[1] = 0;
        }
    }
}

int scr_puts(int x, int y, const char *s, Style st, int maxw) {
    int i = 0, len = (int)strlen(s), used = 0;
    while (i < len) {
        int cp, n = utf8_decode(s + i, len - i, &cp);
        int w = char_width(cp);
        if (maxw >= 0 && used + w > maxw) break;
        scr_set(x + used, y, cp, st);
        used += w;
        i += n;
    }
    return used;
}

void scr_fill(int x, int y, int w, int h, int cp, Style st) {
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) scr_set(i, j, cp, st);
}

Style scr_get_style(int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return def_style;
    return back[y * W + x].st;
}

void scr_set_style(int x, int y, Style st) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    back[y * W + x].st = st;
}

void scr_show_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}

void scr_hide_cursor(void) { cursor_x = cursor_y = -1; }

static int to256(int rgb) {
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int ri = 0, gi = 0, bi = 0, i, best, bd, gray, gd, gv;
    for (i = 0; i < 6; i++) {
        if (abs(lv[i] - r) < abs(lv[ri] - r)) ri = i;
        if (abs(lv[i] - g) < abs(lv[gi] - g)) gi = i;
        if (abs(lv[i] - b) < abs(lv[bi] - b)) bi = i;
    }
    best = 16 + 36 * ri + 6 * gi + bi;
    bd = (lv[ri] - r) * (lv[ri] - r) + (lv[gi] - g) * (lv[gi] - g) + (lv[bi] - b) * (lv[bi] - b);
    gray = (r + g + b) / 3;
    i = (gray - 8) / 10;
    if (i < 0) i = 0;
    if (i > 23) i = 23;
    gv = 8 + 10 * i;
    gd = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    if (gd < bd) best = 232 + i;
    return best;
}

static void emit_color(int c, int fg) {
    char buf[32];
    if (c == COLOR_DEFAULT) {
        emit(fg ? ";39" : ";49");
        return;
    }
    if (COLOR_IS_RGB(c)) {
        int rgb = c & 0xFFFFFF;
        if (scr_truecolor) {
            sprintf(buf, ";%d;2;%d;%d;%d", fg ? 38 : 48, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
            emit(buf);
            return;
        }
        c = to256(rgb);
    }
    if (c < 8) sprintf(buf, ";%d", (fg ? 30 : 40) + c);
    else if (c < 16) sprintf(buf, ";%d", (fg ? 90 : 100) + c - 8);
    else sprintf(buf, ";%d;5;%d", fg ? 38 : 48, c);
    emit(buf);
}

static void emit_style(Style st) {
    emit("\x1b[0");
    if (st.attr & ATTR_BOLD) emit(";1");
    if (st.attr & ATTR_DIM) emit(";2");
    if (st.attr & ATTR_ITALIC) emit(";3");
    if (st.attr & ATTR_UNDERLINE) emit(";4");
    if (st.attr & ATTR_BLINK) emit(";5");
    if (st.attr & ATTR_REVERSE) emit(";7");
    if (st.attr & ATTR_STRIKE) emit(";9");
    emit_color(st.fg, 1);
    emit_color(st.bg, 0);
    emit("m");
}

static int cell_eq(const Cell *a, const Cell *b) {
    return a->width == b->width && style_eq(a->st, b->st) && !strcmp(a->g, b->g);
}

void scr_flush(void) {
    int x, y, cx = -1, cy = -1, have_style = 0;
    Style cur = def_style;
    char buf[32];
    emit("\x1b[?25l");
    if (full_redraw) {
        emit("\x1b[0m\x1b[2J");
    }
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            Cell *b = &back[y * W + x], *f = &front[y * W + x];
            if (b->width == -1) continue;
            if (!full_redraw && cell_eq(b, f)) {
                if (b->width == 2 && x + 1 < W && !cell_eq(&back[y * W + x + 1], &front[y * W + x + 1])) {
                    /* fallthrough to redraw */
                } else continue;
            }
            if (cx != x || cy != y) {
                sprintf(buf, "\x1b[%d;%dH", y + 1, x + 1);
                emit(buf);
            }
            if (!have_style || !style_eq(cur, b->st)) {
                emit_style(b->st);
                cur = b->st;
                have_style = 1;
            }
            emit(b->g[0] ? b->g : " ");
            cx = x + (b->width == 2 ? 2 : 1);
            cy = y;
            if (cx >= W) cx = -1;
        }
    }
    emit("\x1b[0m");
    memcpy(front, back, sizeof *front * (size_t)(W * H));
    full_redraw = 0;
    if (cursor_x >= 0 && cursor_y >= 0 && cursor_x < W && cursor_y < H) {
        sprintf(buf, "\x1b[%d;%dH\x1b[?25h", cursor_y + 1, cursor_x + 1);
        emit(buf);
    }
    flush_out();
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void scr_osc52(const char *text, int len) {
    int i;
    emit("\x1b]52;c;");
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)text[i] << 16;
        char q[5];
        if (i + 1 < len) v |= (unsigned char)text[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)text[i + 2];
        q[0] = b64[(v >> 18) & 63];
        q[1] = b64[(v >> 12) & 63];
        q[2] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
        q[3] = i + 2 < len ? b64[v & 63] : '=';
        q[4] = 0;
        emit(q);
    }
    emit("\x07");
    flush_out();
}
