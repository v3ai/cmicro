/*
 * screen.c - double-buffered cell grid rendered with ANSI escape codes.
 *
 * Drawing goes to the back grid; scr_flush() diffs it against the front
 * grid (what the terminal shows) and writes only what changed, in a single
 * write():
 *   - unchanged rows are skipped with one memcmp, unchanged cells are
 *     skipped with short cursor moves (or rewritten when that is cheaper),
 *   - SGR attributes are emitted only when they change, as a delta,
 *   - trailing blanks are cleared with EL,
 *   - when rows moved up or down (scrolling), the terminal scrolls them
 *     itself inside a DECSTBM scroll region and only the new rows are drawn,
 *   - when nothing changed, nothing at all is written.
 * Every glyph stored in a cell is sanitized (no C0/C1 controls, DEL,
 * surrogates or invisible format characters reach the terminal).
 */
#include "micro.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cells are compared with memcmp, so the layout must have no padding and
   every byte of g[] past the glyph must be zero. */
typedef struct {
    Style st;
    int width;   /* 1, 2 (left half of a wide char), -1 (right half) */
    char g[16];  /* UTF-8 glyph (base + combining marks), NUL padded */
} Cell;

typedef char cell_has_no_padding[sizeof(Cell) == sizeof(Style) + sizeof(int) + 16 ? 1 : -1];

/* Grid size limits: the terminal may report anything up to 65535x65535
   (or garbage via $COLUMNS/$LINES); the grids are capped to keep memory
   bounded (2 grids * 32 bytes per cell) and W*H far from int overflow. */
#define SCR_MAX_DIM 4096
#define SCR_MAX_CELLS (1L << 20)

static Cell *front, *back;
static int W, H;
static int cursor_x = -1, cursor_y = -1;
static int full_redraw = 1;
static int term_cursor_visible = -1; /* unknown */
static int clip_x0, clip_y0, clip_x1, clip_y1; /* drawing clip rect [x0,x1) x [y0,y1) */
static Str out;
static int out_ready;
static int bce = 1; /* terminal erases with the current background color */
static unsigned *hash_b, *hash_f; /* per-row hashes for scroll detection */
static int hash_b_fresh;           /* hash_b computed this frame */
static int hash_f_valid;           /* hash_f matches front (from last frame's hash_b) */
static unsigned char *row_chg;     /* per-row "differs from the terminal" */
int scr_truecolor = 0;

int scr_detect_truecolor(const char *opt) {
    const char *ct = getenv("COLORTERM");
    if (!strcmp(opt, "on")) return 1;
    if (strcmp(opt, "auto") != 0) return 0;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return 1;
    return getenv("WT_SESSION") != NULL; /* Windows Terminal */
}

static void emit(const char *s) { str_puts(&out, s); }
static void emitn(const char *s, int n) { str_putn(&out, s, n); }

static void emit_int(int v) {
    char buf[12];
    int n = 0;
    if (v < 0) v = 0;
    do {
        buf[sizeof buf - 1 - n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    emitn(buf + sizeof buf - n, n);
}

static void flush_out(void) {
    if (out.len) plat_write(out.s, (size_t)out.len);
    str_clear(&out);
}

/* a blank cell as the terminal shows it after an erase with SGR 0 */
static void cell_blank(Cell *c) {
    memset(c, 0, sizeof *c);
    c->st.fg = c->st.bg = COLOR_DEFAULT;
    c->width = 1;
    c->g[0] = ' ';
}

static void clip_reset(void) {
    clip_x0 = clip_y0 = 0;
    clip_x1 = W;
    clip_y1 = H;
}

static void alloc_grids(void) {
    size_t n = (size_t)W * (size_t)H;
    free(front);
    free(back);
    free(hash_b);
    free(hash_f);
    free(row_chg);
    front = xcalloc(n + 1, sizeof *front);
    back = xcalloc(n + 1, sizeof *back);
    hash_b = xcalloc((size_t)H + 1, sizeof *hash_b);
    hash_f = xcalloc((size_t)H + 1, sizeof *hash_f);
    row_chg = xcalloc((size_t)H + 1, 1);
    full_redraw = 1;
    clip_reset();
}

static void set_size(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > SCR_MAX_DIM) w = SCR_MAX_DIM;
    if (h > SCR_MAX_DIM) h = SCR_MAX_DIM;
    if ((long)w * h > SCR_MAX_CELLS) h = (int)(SCR_MAX_CELLS / w);
    if (front && w == W && h == H) {
        full_redraw = 1;
        clip_reset();
        return;
    }
    W = w;
    H = h;
    alloc_grids();
}

void scr_init(void) {
    int w, h;
    const char *term = getenv("TERM");
    if (!out_ready) {
        str_init(&out);
        out_ready = 1;
    }
    /* GNU screen only does BCE when configured to (tmux sets $TMUX) */
    bce = !(term && !strncmp(term, "screen", 6) && !getenv("TMUX"));
    w = h = 0;
    plat_term_size(&w, &h);
    if (w < 1) w = 80;
    if (h < 1) h = 24;
    set_size(w, h);
    term_cursor_visible = -1;
    /* alternate screen, clear, bracketed paste, no autowrap (a character
       the terminal thinks is wider than we do can then never wrap onto
       the next line, or scroll the whole screen from the last line) */
    emit("\x1b[?1049h\x1b[H\x1b[2J\x1b[?2004h\x1b[?7l");
    flush_out();
}

void scr_set_mouse(int on) {
    if (on) emit("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    else emit("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    flush_out();
}

void scr_fini(void) {
    emit("\x1b[0m\x1b[r\x1b[?7h\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?2004l\x1b[?25h\x1b[2J\x1b[?1049l");
    flush_out();
    term_cursor_visible = 1;
}

void scr_resize(void) {
    int w = 0, h = 0;
    plat_term_size(&w, &h);
    set_size(w, h);
}

int scr_w(void) { return W; }
int scr_h(void) { return H; }

void scr_invalidate(void) { full_redraw = 1; }

void scr_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = w < 0 ? clip_x0 : (x + w > W ? W : x + w);
    clip_y1 = h < 0 ? clip_y0 : (y + h > H ? H : y + h);
}

void scr_noclip(void) { clip_reset(); }

void scr_clear(void) {
    int i, n = W * H;
    Cell c;
    memset(&c, 0, sizeof c);
    c.g[0] = ' ';
    c.width = 1;
    c.st = def_style;
    for (i = 0; i < n; i++) back[i] = c;
    clip_reset();
}

/* Invisible format characters (zero width space/joiners, bidi embeddings
   and overrides, BOM): terminals disagree about them and bidi controls can
   visually reorder the text after them, so they are not drawn at all.
   These all have char_width() 0, so layout is unaffected. */
static int is_format_char(int cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF;
}

/* format characters that char_width() counts as one column (the Arabic
   letter mark, the Mongolian vowel separator, bidi isolates, deprecated
   format controls, interlinear annotations, tag characters) are shown as
   a replacement character */
static int is_visible_format_char(int cp) {
    return cp == 0x061C || cp == 0x180E ||
           (cp >= 0x2065 && cp <= 0x206F) || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
           (cp >= 0xE0000 && cp <= 0xE007F);
}

/* the cell at x of row r is about to be overwritten: if it is half of a
   wide character, turn the other half into a blank so the grid never
   holds an orphaned half (the terminal would erase it anyway) */
static void break_wide(Cell *r, int x) {
    Cell *c = &r[x];
    if (c->width == -1 && x > 0 && r[x - 1].width == 2) {
        Style st = r[x - 1].st;
        memset(&r[x - 1], 0, sizeof *c);
        r[x - 1].st = st;
        r[x - 1].width = 1;
        r[x - 1].g[0] = ' ';
    } else if (c->width == 2 && x + 1 < W && r[x + 1].width == -1) {
        Style st = r[x + 1].st;
        memset(&r[x + 1], 0, sizeof *c);
        r[x + 1].st = st;
        r[x + 1].width = 1;
        r[x + 1].g[0] = ' ';
    }
}

void scr_set(int x, int y, int cp, Style st) {
    Cell *row, *c;
    int w;
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    row = &back[y * W];
    c = &row[x];
    if (cp >= 32 && cp < 127 && c->width == 1 && (x + 1 >= W || row[x + 1].width != -1)) {
        /* fast path: printable ASCII over a narrow cell */
        memset(c->g, 0, sizeof c->g);
        c->g[0] = (char)cp;
        c->st = st;
        return;
    }
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || is_visible_format_char(cp)) cp = 0xFFFD;
    if (is_format_char(cp)) return;
    w = char_width(cp);
    if (w == 0 && cp != 0) {
        /* combining mark: append to the previous cell (within the clip) */
        if (x > clip_x0) {
            Cell *p = &row[x - 1];
            if (p->width == -1 && x - 1 > clip_x0) p = &row[x - 2];
            if (p->width > 0) {
                size_t len = strlen(p->g);
                char buf[4];
                int n = utf8_encode(cp, buf);
                if (len + (size_t)n < sizeof p->g) memcpy(p->g + len, buf, (size_t)n);
            }
        }
        return;
    }
    if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0)) cp = '?';
    break_wide(row, x);
    memset(c->g, 0, sizeof c->g);
    utf8_encode(cp, c->g);
    c->st = st;
    c->width = 1;
    if (w == 2) {
        if (x + 1 < clip_x1) {
            Cell *n = &row[x + 1];
            break_wide(row, x + 1);
            memset(n->g, 0, sizeof n->g);
            n->width = -1;
            n->st = st;
            c->width = 2;
        } else {
            /* no room for the right half */
            memset(c->g, 0, sizeof c->g);
            c->g[0] = ' ';
        }
    }
}

int scr_puts(int x, int y, const char *s, Style st, int maxw) {
    int i = 0, len = (int)strlen(s), used = 0;
    if (maxw == 0) return 0;
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
    int i, j, x0 = x, y0 = y, x1, y1;
    if (w <= 0 || h <= 0) return;
    x1 = x + w;
    y1 = y + h;
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1) return;
    if (cp >= 32 && cp < 127) {
        /* fast path: copy one prepared cell; only wide characters crossing
           the edges of the span need fixing up */
        Cell c;
        memset(&c, 0, sizeof c);
        c.st = st;
        c.width = 1;
        c.g[0] = (char)cp;
        for (j = y0; j < y1; j++) {
            Cell *row = &back[j * W];
            break_wide(row, x0);
            break_wide(row, x1 - 1);
            for (i = x0; i < x1; i++) row[i] = c;
        }
        return;
    }
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++) scr_set(i, j, cp, st);
}

Style scr_get_style(int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return def_style;
    return back[y * W + x].st;
}

void scr_set_style(int x, int y, Style st) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
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

/* append ";<color>" for fg or bg */
static void emit_color(int c, int fg) {
    if (c == COLOR_DEFAULT || c < 0) {
        emit(fg ? ";39" : ";49");
        return;
    }
    if (COLOR_IS_RGB(c)) {
        int rgb = c & 0xFFFFFF;
        if (scr_truecolor) {
            emit(fg ? ";38;2;" : ";48;2;");
            emit_int((rgb >> 16) & 255);
            emit(";");
            emit_int((rgb >> 8) & 255);
            emit(";");
            emit_int(rgb & 255);
            return;
        }
        c = to256(rgb);
    }
    c &= 255;
    emit(";");
    if (c < 8) emit_int((fg ? 30 : 40) + c);
    else if (c < 16) emit_int((fg ? 90 : 100) + c - 8);
    else {
        emit(fg ? "38;5;" : "48;5;");
        emit_int(c);
    }
}

static const struct {
    unsigned attr;
    const char *on;
} attr_codes[] = {
    {ATTR_BOLD, ";1"}, {ATTR_DIM, ";2"}, {ATTR_ITALIC, ";3"}, {ATTR_UNDERLINE, ";4"},
    {ATTR_BLINK, ";5"}, {ATTR_REVERSE, ";7"}, {ATTR_STRIKE, ";9"},
};

/* switch the terminal from style `from` (if known) to `to`, emitting only
   the difference; attributes can't be turned off individually in a
   portable way (bold and dim share 22), so losing any means a reset */
static void emit_style(const Style *from, Style to) {
    size_t i, start;
    int reset = !from || (from->attr & ~to.attr);
    emit("\x1b[");
    start = (size_t)out.len;
    if (reset) emit("0");
    for (i = 0; i < sizeof attr_codes / sizeof attr_codes[0]; i++)
        if ((to.attr & attr_codes[i].attr) && (reset || !(from->attr & attr_codes[i].attr)))
            emit(attr_codes[i].on);
    if (reset ? to.fg != COLOR_DEFAULT : from->fg != to.fg) emit_color(to.fg, 1);
    if (reset ? to.bg != COLOR_DEFAULT : from->bg != to.bg) emit_color(to.bg, 0);
    /* drop the leading ';' of a delta */
    if ((size_t)out.len > start && out.s[start] == ';') {
        memmove(out.s + start, out.s + start + 1, (size_t)out.len - start - 1);
        out.len--;
    }
    emit("m");
}

static void emit_goto(int x, int y) {
    emit("\x1b[");
    emit_int(y + 1);
    emit(";");
    emit_int(x + 1);
    emit("H");
}

static int cell_eq(const Cell *a, const Cell *b) { return !memcmp(a, b, sizeof *a); }

/* state of the terminal while emitting a frame */
typedef struct {
    int cx, cy;     /* cursor position, cx < 0: unknown */
    int have_style; /* is `cur` known? */
    Style cur;
    int drew;       /* anything emitted yet (cursor hidden)? */
} Emit;

static void begin_drawing(Emit *e) {
    if (e->drew) return;
    e->drew = 1;
    if (term_cursor_visible != 0) emit("\x1b[?25l");
    term_cursor_visible = 0;
}

static void move_to(Emit *e, int x, int y) {
    if (e->cy == y && e->cx == x) return;
    if (e->cy == y && e->cx >= 0 && x > e->cx) {
        /* CUF: shorter than CUP */
        emit("\x1b[");
        if (x - e->cx > 1) emit_int(x - e->cx);
        emit("C");
    } else if (x == 0 && e->cy >= 0 && y == e->cy + 1 && e->cx >= 0) {
        emit("\r\n"); /* the cursor is never on the last row's scroll margin here */
    } else {
        emit_goto(x, y);
    }
    e->cx = x;
    e->cy = y;
}

#define ATTR_VISIBLE_ON_BLANK (ATTR_REVERSE | ATTR_UNDERLINE | ATTR_STRIKE | ATTR_BLINK)

/* make the terminal's current style st; for a blank cell (space or erase)
   only the background and the attributes visible on blanks matter, so a
   different foreground is left alone */
static void ensure_style(Emit *e, Style st, int blank) {
    if (e->have_style) {
        if (style_eq(e->cur, st)) return;
        if (blank && e->cur.bg == st.bg && !(st.attr & ATTR_VISIBLE_ON_BLANK) &&
            !(e->cur.attr & ATTR_VISIBLE_ON_BLANK))
            return;
    }
    emit_style(e->have_style ? &e->cur : NULL, st);
    e->cur = st;
    e->have_style = 1;
}

static void put_cell(Emit *e, const Cell *c, int x, int y) {
    move_to(e, x, y);
    ensure_style(e, c->st, c->g[0] == ' ' && !c->g[1]);
    if (c->g[0]) emit(c->g);
    else emit(" ");
    e->cx = x + (c->width == 2 ? 2 : 1);
    /* after a combining sequence or a symbol/emoji whose width terminals
       disagree about, don't trust the terminal's cursor position: the next
       cell gets an explicit move */
    if (c->g[1]) {
        int cp, len = (int)strlen(c->g);
        if (utf8_decode(c->g, len, &cp) < len || cp >= 0x1F000 || (cp >= 0x2190 && cp < 0x2C00))
            e->cx = -1;
    }
    if (e->cx >= W) e->cx = -1;
}

/* EL clears with the current background on terminals with BCE (nearly all
   of them); otherwise only use it for blanks in the default colors */
static int el_safe(const Cell *c) {
    return c->width == 1 && c->g[0] == ' ' && !c->g[1] && (bce || c->st.bg == COLOR_DEFAULT) &&
           !(c->st.attr & ATTR_VISIBLE_ON_BLANK);
}

/* number of cells of row a that differ from row b (or from the cell *all) */
static long cells_differing(const Cell *a, const Cell *b, const Cell *all) {
    long n = 0;
    int x;
    for (x = 0; x < W; x++) n += !cell_eq(&a[x], b ? &b[x] : all);
    return n;
}

static unsigned row_hash(const Cell *r) {
    /* multiplicative hash over the row's bytes, 64 bits at a time (Cell
       is a multiple of 8 bytes); matches are verified with memcmp */
    const unsigned char *p = (const unsigned char *)r;
    size_t i, n = sizeof *r * (size_t)W;
    unsigned long long h = 0x9E3779B97F4A7C15ULL;
    for (i = 0; i + 8 <= n; i += 8) {
        unsigned long long v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ULL;
        h ^= h >> 29;
    }
    return (unsigned)(h ^ (h >> 32));
}

/* Detect content that moved vertically and let the terminal scroll it.
   Picks the shift k (back row y == front row y + k) matching the most rows,
   scrolls the span between the first and last matching row, and shifts the
   front grid to match; the normal diff then fixes up whatever differs. */
static void scroll_optimize(Emit *e) {
    int y, k, maxk, best_k = 0, best_n = 0, top, bot, rtop, rbot, i, changed = 0;
    size_t rowsz = sizeof *front * (size_t)W;
    if (H < 4) return;
    for (y = 0; y < H; y++) {
        row_chg[y] = memcmp(&back[y * W], &front[y * W], rowsz) != 0;
        changed += row_chg[y];
    }
    if (changed < 3) return; /* the common case: typing, cursor movement */
    for (y = 0; y < H; y++) hash_b[y] = row_hash(&back[y * W]);
    hash_b_fresh = 1;
    if (!hash_f_valid)
        for (y = 0; y < H; y++) hash_f[y] = row_hash(&front[y * W]);
    maxk = H - 2 < 128 ? H - 2 : 128;
    for (k = -maxk; k <= maxk; k++) {
        int n = 0;
        if (k == 0) continue;
        for (y = k > 0 ? 0 : -k; y < H && y + k < H; y++)
            if (row_chg[y] && hash_b[y] == hash_f[y + k]) n++;
        if (n > best_n) { best_n = n; best_k = k; }
    }
    /* scrolling costs ~25 bytes; a row is usually much more than that */
    if (best_n < 2) return;
    k = best_k;
    /* matching span (in back coordinates), verified with memcmp */
    top = -1;
    bot = -1;
    for (y = k > 0 ? 0 : -k; y < H && y + k < H; y++) {
        if (row_chg[y] && hash_b[y] == hash_f[y + k] && !memcmp(&back[y * W], &front[(y + k) * W], rowsz)) {
            if (top < 0) top = y;
            bot = y;
        }
    }
    if (top < 0 || bot - top + 1 < 2) return;
    /* scroll region in terminal rows: content of [top+k, bot+k] moves to [top, bot] */
    rtop = k > 0 ? top : top + k;
    rbot = k > 0 ? bot + k : bot;
    {
        /* only worth it if it leaves clearly fewer cells to draw (similar
           pages, e.g. after PageDown, often differ in only a few cells) */
        long before = 0, after = 0;
        Cell blank;
        cell_blank(&blank);
        for (y = rtop; y <= rbot; y++) {
            const Cell *b = &back[y * W];
            int src = y + k;
            before += cells_differing(b, &front[y * W], NULL);
            if (src >= rtop && src <= rbot) after += cells_differing(b, &front[src * W], NULL);
            else after += cells_differing(b, NULL, &blank);
        }
        if (after + 2 * (rbot - rtop) + 16 >= before) return;
    }
    begin_drawing(e);
    if (!e->have_style || !style_eq(e->cur, def_style) || def_style.fg != COLOR_DEFAULT ||
        def_style.bg != COLOR_DEFAULT || def_style.attr) {
        /* new lines are cleared with the current colors: use SGR 0 */
        emit("\x1b[0m");
        e->cur.fg = e->cur.bg = COLOR_DEFAULT;
        e->cur.attr = 0;
        e->have_style = 1;
    }
    emit("\x1b[");
    emit_int(rtop + 1);
    emit(";");
    emit_int(rbot + 1);
    emit("r");
    if (k > 0) {
        emit_goto(0, rbot);
        for (i = 0; i < k; i++) emit("\n");
    } else {
        emit_goto(0, rtop);
        for (i = 0; i < -k; i++) emit("\x1bM");
    }
    emit("\x1b[r");
    e->cx = e->cy = -1;
    /* mirror it in the front grid */
    if (k > 0) {
        memmove(&front[rtop * W], &front[(rtop + k) * W], rowsz * (size_t)(rbot - rtop + 1 - k));
        for (y = rbot - k + 1; y <= rbot; y++)
            for (i = 0; i < W; i++) cell_blank(&front[y * W + i]);
    } else {
        memmove(&front[(rtop - k) * W], &front[rtop * W], rowsz * (size_t)(rbot - rtop + 1 + k));
        for (y = rtop; y < rtop - k; y++)
            for (i = 0; i < W; i++) cell_blank(&front[y * W + i]);
    }
}

void scr_flush(void) {
    int x, y;
    Emit e;
    e.cx = e.cy = -1;
    e.have_style = 0;
    e.cur = def_style;
    e.drew = 0;
    hash_b_fresh = 0;
    if (full_redraw) {
        int i, n = W * H;
        begin_drawing(&e);
        emit("\x1b[0m\x1b[r\x1b[H\x1b[2J");
        e.cx = e.cy = 0;
        e.have_style = 1;
        e.cur.fg = e.cur.bg = COLOR_DEFAULT;
        e.cur.attr = 0;
        for (i = 0; i < n; i++) cell_blank(&front[i]);
        hash_f_valid = 0;
    } else {
        scroll_optimize(&e);
    }
    for (y = 0; y < H; y++) {
        Cell *brow = &back[y * W], *frow = &front[y * W];
        int x0, x1, tb;
        if (!memcmp(brow, frow, sizeof *brow * (size_t)W)) continue;
        /* first and last differing cell, widened to whole wide chars */
        for (x0 = 0; x0 < W && cell_eq(&brow[x0], &frow[x0]); x0++)
            ;
        for (x1 = W - 1; x1 > x0 && cell_eq(&brow[x1], &frow[x1]); x1--)
            ;
        if (x0 > 0 && (brow[x0].width == -1 || frow[x0].width == -1)) x0--;
        if (x1 + 1 < W && (brow[x1].width == 2 || frow[x1].width == 2)) x1++;
        /* trailing run of default-colored blanks: clear it with EL */
        tb = W;
        while (tb > x0 && el_safe(&brow[tb - 1]) && brow[tb - 1].st.bg == brow[W - 1].st.bg) tb--;
        if (x1 < tb || W - tb < 4) tb = W; /* not worth it / not needed */
        begin_drawing(&e);
        for (x = x0; x <= x1 && x < tb;) {
            Cell *b = &brow[x], *f = &frow[x];
            if (b->width == -1) { x++; continue; }
            if (cell_eq(b, f) && !(b->width == 2 && x + 1 < W && !cell_eq(&brow[x + 1], &frow[x + 1]))) {
                /* unchanged run: skip it, or rewrite it when that is shorter
                   than moving the cursor (same style, plain ASCII) */
                int r = x, cheap = e.cx == x && e.have_style;
                while (r <= x1 && r < tb && cell_eq(&brow[r], &frow[r])) {
                    if (cheap && (r - x >= 4 || brow[r].width != 1 || (unsigned char)brow[r].g[0] >= 0x80 ||
                                  !style_eq(brow[r].st, e.cur)))
                        cheap = 0;
                    r++;
                }
                if (cheap && r - x < 4) {
                    for (; x < r; x++) put_cell(&e, &brow[x], x, y);
                }
                x = r;
                continue;
            }
            put_cell(&e, b, x, y);
            x += b->width == 2 ? 2 : 1;
        }
        if (tb < W) {
            move_to(&e, tb, y);
            ensure_style(&e, brow[tb].st, 1);
            emit("\x1b[K");
        }
        memcpy(frow, brow, sizeof *brow * (size_t)W);
    }
    if (e.drew) emit("\x1b[0m");
    full_redraw = 0;
    /* front == back now, so this frame's back hashes are next frame's
       front hashes */
    if (hash_b_fresh) {
        unsigned *t = hash_f;
        hash_f = hash_b;
        hash_b = t;
    }
    hash_f_valid = hash_b_fresh;
    if (cursor_x >= 0 && cursor_y >= 0 && cursor_x < W && cursor_y < H) {
        static int last_x = -1, last_y = -1;
        if (e.drew || cursor_x != last_x || cursor_y != last_y || term_cursor_visible != 1) {
            emit_goto(cursor_x, cursor_y);
            if (term_cursor_visible != 1) emit("\x1b[?25h");
            term_cursor_visible = 1;
        }
        last_x = cursor_x;
        last_y = cursor_y;
    } else if (term_cursor_visible != 0) {
        emit("\x1b[?25l");
        term_cursor_visible = 0;
    }
    flush_out();
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void scr_osc52(const char *text, int len) {
    int i;
    emit("\x1b]52;c;");
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned)(unsigned char)text[i] << 16;
        char q[4];
        if (i + 1 < len) v |= (unsigned)(unsigned char)text[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)text[i + 2];
        q[0] = b64[(v >> 18) & 63];
        q[1] = b64[(v >> 12) & 63];
        q[2] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
        q[3] = i + 2 < len ? b64[v & 63] : '=';
        emitn(q, 4);
    }
    emit("\x07");
    flush_out();
}
