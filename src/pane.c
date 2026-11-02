/*
 * pane.c - cursors, visual geometry (tabs, wide chars, softwrap) and
 * scrolling for a buffer view.
 */
#include "micro.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- cursor basics ---------------- */

int cur_has_sel(const Cursor *c) { return loc_cmp(c->sel[0], c->sel[1]) != 0; }

void cur_deselect(Cursor *c, int start) {
    if (cur_has_sel(c)) c->loc = start ? c->sel[0] : c->sel[1];
    c->sel[0] = c->sel[1] = c->loc;
    c->orig[0] = c->orig[1] = c->loc;
}

void cur_set_sel_to(Cursor *c, Loc l) {
    /* selection from anchor orig[0] (or orig range for word/line drags) */
    if (loc_cmp(c->orig[0], c->orig[1]) == 0) {
        if (loc_cmp(l, c->orig[0]) < 0) { c->sel[0] = l; c->sel[1] = c->orig[0]; }
        else { c->sel[0] = c->orig[0]; c->sel[1] = l; }
    } else {
        if (loc_cmp(l, c->orig[0]) < 0) { c->sel[0] = l; c->sel[1] = c->orig[1]; }
        else if (loc_cmp(l, c->orig[1]) > 0) { c->sel[0] = c->orig[0]; c->sel[1] = l; }
        else { c->sel[0] = c->orig[0]; c->sel[1] = c->orig[1]; }
    }
}

/* ---------------- pane lifecycle ---------------- */

Pane *pane_new(Buffer *b) {
    Pane *p = xcalloc(1, sizeof *p);
    p->buf = b;
    b->refs++;
    p->ccap = 4;
    p->cur = xcalloc((size_t)p->ccap, sizeof *p->cur);
    p->ncur = 1;
    return p;
}

void pane_free(Pane *p) {
    if (!p) return;
    if (p->buf && --p->buf->refs <= 0) buf_free(p->buf);
    free(p->cur);
    free(p);
}

void pane_set_buffer(Pane *p, Buffer *b) {
    b->refs++;
    if (p->buf && --p->buf->refs <= 0) buf_free(p->buf);
    p->buf = b;
    pane_reset_cursors(p);
    memset(&p->cur[0], 0, sizeof p->cur[0]);
    p->top = p->toprow = p->left = 0;
}

int pane_add_cursor(Pane *p, Loc l) {
    Cursor *c;
    if (p->ncur == p->ccap) {
        p->ccap *= 2;
        p->cur = xrealloc(p->cur, sizeof *p->cur * (size_t)p->ccap);
    }
    c = &p->cur[p->ncur];
    memset(c, 0, sizeof *c);
    c->loc = l;
    c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = l;
    cur_store_vx(p, c);
    return p->ncur++;
}

void pane_remove_cursor(Pane *p, int i) {
    if (p->ncur <= 1 || i < 0 || i >= p->ncur) return;
    memmove(p->cur + i, p->cur + i + 1, sizeof *p->cur * (size_t)(p->ncur - i - 1));
    p->ncur--;
    if (p->active >= p->ncur) p->active = p->ncur - 1;
    else if (p->active > i) p->active--;
}

void pane_reset_cursors(Pane *p) {
    if (p->ncur > 1) {
        p->cur[0] = p->cur[p->active];
        p->ncur = 1;
    }
    p->active = 0;
}

/* remove cursors that ended up at the same location (micro: MergeCursors) */
void pane_merge_cursors(Pane *p) {
    int i, j;
    for (i = 0; i < p->ncur; i++) {
        for (j = i + 1; j < p->ncur; j++) {
            if (loc_cmp(p->cur[i].loc, p->cur[j].loc) == 0) {
                if (p->active == j) p->active = i;
                pane_remove_cursor(p, j);
                j--;
            }
        }
    }
}

/* ---------------- geometry ---------------- */

int pane_gutter_width(const Pane *p) {
    if (!BOPT(p->buf, O_RULER)) return 0;
    return digits(p->buf->nlines) + 1;
}

int pane_text_width(const Pane *p) {
    int w = p->w - pane_gutter_width(p) - (BOPT(p->buf, O_SCROLLBAR) ? 1 : 0);
    return w < 1 ? 1 : w;
}

int pane_text_height(const Pane *p) {
    int h = p->h - (BOPT(p->buf, O_STATUSLINE) ? 1 : 0);
    return h < 1 ? 1 : h;
}

int pane_tabsize(const Buffer *b) {
    /* the option only guarantees > 0; huge values would make column math
       overflow and tab drawing loop for ages */
    return clampi(BOPT(b, O_TABSIZE), 1, PANE_MAX_TABSIZE);
}

/* visual columns saturate instead of overflowing on absurdly long lines */
int vx_add(int vx, int w) { return w > PANE_MAX_VX - vx ? PANE_MAX_VX : vx + w; }

static int char_cols(int cp, int vx, int tabsize) {
    if (cp == '\t') return tabsize - (vx % tabsize);
    return char_width(cp);
}

int pane_visual_x(const Pane *p, int y, int x) {
    const Line *ln = &p->buf->lines[y];
    int i = 0, vx = 0, ts = pane_tabsize(p->buf);
    if (x > ln->len) x = ln->len;
    while (i < x) {
        int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
        vx = vx_add(vx, char_cols(cp, vx, ts));
        i += n;
    }
    return vx;
}

int pane_x_from_visual(const Pane *p, int y, int target) {
    const Line *ln = &p->buf->lines[y];
    int i = 0, vx = 0, ts = pane_tabsize(p->buf);
    while (i < ln->len) {
        int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
        int nvx = vx_add(vx, char_cols(cp, vx, ts));
        if (nvx > target) return i;
        vx = nvx;
        i += n;
    }
    return ln->len;
}

/* softwrap: compute the starting byte offsets / columns of each row */
typedef struct {
    int n, cap;
    int *bx, *vx;
} Wrap;

static Wrap wrap;

static void wrap_push(int bx, int vx) {
    if (wrap.n == wrap.cap) {
        wrap.cap = wrap.cap ? wrap.cap * 2 : 16;
        wrap.bx = xrealloc(wrap.bx, sizeof(int) * (size_t)wrap.cap);
        wrap.vx = xrealloc(wrap.vx, sizeof(int) * (size_t)wrap.cap);
    }
    wrap.bx[wrap.n] = bx;
    wrap.vx[wrap.n] = vx;
    wrap.n++;
}

/* Wrapping a line is O(length) and happens several times per key press
   (scrolling, cursor movement, drawing). For long lines the last result is
   kept together with a copy of the line, and reused when the line is
   byte-for-byte the same (a memcmp is far cheaper than decoding). */
#define WRAP_CACHE_MIN 4096

static struct {
    char *s;
    int len, cap, width, ts, ww, n, valid;
    int *bx, *vx;
} wcache;

static int wrap_cache_get(const Line *ln, int width, int ts, int ww) {
    if (!wcache.valid || wcache.len != ln->len || wcache.width != width || wcache.ts != ts ||
        wcache.ww != ww || memcmp(wcache.s, ln->s, (size_t)ln->len) != 0)
        return 0;
    if (wcache.n > wrap.cap) {
        wrap.cap = wcache.n;
        wrap.bx = xrealloc(wrap.bx, sizeof(int) * (size_t)wrap.cap);
        wrap.vx = xrealloc(wrap.vx, sizeof(int) * (size_t)wrap.cap);
    }
    memcpy(wrap.bx, wcache.bx, sizeof(int) * (size_t)wcache.n);
    memcpy(wrap.vx, wcache.vx, sizeof(int) * (size_t)wcache.n);
    wrap.n = wcache.n;
    return 1;
}

static void wrap_cache_put(const Line *ln, int width, int ts, int ww) {
    if (ln->len + 1 > wcache.cap) {
        wcache.cap = ln->len + 1;
        free(wcache.s);
        wcache.s = xmalloc((size_t)wcache.cap);
    }
    memcpy(wcache.s, ln->s, (size_t)ln->len);
    wcache.bx = xrealloc(wcache.bx, sizeof(int) * (size_t)wrap.n);
    wcache.vx = xrealloc(wcache.vx, sizeof(int) * (size_t)wrap.n);
    memcpy(wcache.bx, wrap.bx, sizeof(int) * (size_t)wrap.n);
    memcpy(wcache.vx, wrap.vx, sizeof(int) * (size_t)wrap.n);
    wcache.n = wrap.n;
    wcache.len = ln->len;
    wcache.width = width;
    wcache.ts = ts;
    wcache.ww = ww;
    wcache.valid = 1;
}

static void compute_wrap(const Pane *p, int y) {
    const Line *ln = &p->buf->lines[y];
    int width = pane_text_width(p), ts = pane_tabsize(p->buf);
    int ww = BOPT(p->buf, O_WORDWRAP);
    int i = 0, vx = 0, rowvx = 0;
    int last_break_b = -1, last_break_v = 0;
    if (ln->len >= WRAP_CACHE_MIN && wrap_cache_get(ln, width, ts, ww)) return;
    wrap.n = 0;
    wrap_push(0, 0);
    while (i < ln->len) {
        int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
        int w = char_cols(cp, vx, ts);
        if (vx - rowvx + w > width && vx > rowvx) {
            if (ww && last_break_b > wrap.bx[wrap.n - 1]) {
                wrap_push(last_break_b, last_break_v);
                rowvx = last_break_v;
                last_break_b = -1;
                if (vx - rowvx + w > width) {
                    wrap_push(i, vx);
                    rowvx = vx;
                }
            } else {
                wrap_push(i, vx);
                rowvx = vx;
                last_break_b = -1;
            }
        }
        vx = vx_add(vx, w);
        i += n;
        if (ww && (cp == ' ' || cp == '\t')) {
            last_break_b = i;
            last_break_v = vx;
        }
    }
    /* like micro: when the last row is exactly full, the end-of-line
       position (where the cursor can sit) starts a new row */
    if (vx - rowvx >= width && vx > rowvx) wrap_push(ln->len, vx);
    if (ln->len >= WRAP_CACHE_MIN) wrap_cache_put(ln, width, ts, ww);
}

int pane_line_rows(const Pane *p, int y) {
    if (!BOPT(p->buf, O_SOFTWRAP)) return 1;
    compute_wrap(p, y);
    return wrap.n;
}

int pane_wrap_line(const Pane *p, int y, const int **bx, const int **vx) {
    compute_wrap(p, y);
    *bx = wrap.bx;
    *vx = wrap.vx;
    return wrap.n;
}

void pane_loc_to_row(const Pane *p, Loc l, int *row, int *rvx) {
    const Line *ln;
    int lo, hi, i, vx, ts;
    if (!BOPT(p->buf, O_SOFTWRAP)) {
        *row = 0;
        *rvx = pane_visual_x(p, l.y, l.x);
        return;
    }
    compute_wrap(p, l.y);
    /* last row starting at or before l.x (binary search) */
    lo = 0;
    hi = wrap.n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (wrap.bx[mid] <= l.x) lo = mid;
        else hi = mid - 1;
    }
    /* columns from the row start: no need to decode the line from 0 */
    ln = &p->buf->lines[l.y];
    ts = pane_tabsize(p->buf);
    vx = wrap.vx[lo];
    for (i = wrap.bx[lo]; i < l.x && i < ln->len;) {
        int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
        vx = vx_add(vx, char_cols(cp, vx, ts));
        i += n;
    }
    *row = lo;
    *rvx = vx - wrap.vx[lo];
}

Loc pane_row_to_loc(const Pane *p, int y, int row, int rvx) {
    int start_b, start_v, end_b, vx, i, ts = pane_tabsize(p->buf);
    const Line *ln = &p->buf->lines[y];
    if (!BOPT(p->buf, O_SOFTWRAP)) return loc_make(pane_x_from_visual(p, y, rvx), y);
    compute_wrap(p, y);
    if (row >= wrap.n) row = wrap.n - 1;
    if (row < 0) row = 0;
    start_b = wrap.bx[row];
    start_v = wrap.vx[row];
    end_b = row + 1 < wrap.n ? wrap.bx[row + 1] : ln->len;
    vx = start_v;
    i = start_b;
    while (i < end_b) {
        int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
        int nvx = vx_add(vx, char_cols(cp, vx, ts));
        if (nvx - start_v > rvx) return loc_make(i, y);
        vx = nvx;
        i += n;
    }
    if (row + 1 < wrap.n && end_b > start_b) return loc_make(utf8_prev(ln->s, end_b), y);
    return loc_make(end_b, y);
}

/* ---------------- scrolling (SLoc = line + row) ---------------- */

typedef struct {
    int line, row;
} SLoc;

static int sloc_cmp(SLoc a, SLoc b) {
    if (a.line != b.line) return a.line < b.line ? -1 : 1;
    if (a.row != b.row) return a.row < b.row ? -1 : 1;
    return 0;
}

static SLoc sloc_scroll(const Pane *p, SLoc s, int n) {
    if (!BOPT(p->buf, O_SOFTWRAP)) {
        s.line = clampi(s.line + n, 0, p->buf->nlines - 1);
        s.row = 0;
        return s;
    }
    {
        /* row count of s.line, computed once per line (not once per row:
           a single huge softwrapped line made this quadratic) */
        int rows = n > 0 ? pane_line_rows(p, s.line) : 0;
        while (n > 0) {
            if (s.row + 1 < rows) s.row++;
            else if (s.line + 1 < p->buf->nlines) {
                s.line++;
                s.row = 0;
                rows = pane_line_rows(p, s.line);
            } else break;
            n--;
        }
    }
    while (n < 0) {
        if (s.row > 0) s.row--;
        else if (s.line > 0) { s.line--; s.row = pane_line_rows(p, s.line) - 1; }
        else break;
        n++;
    }
    return s;
}

static SLoc sloc_from_loc(const Pane *p, Loc l) {
    SLoc s;
    int rvx;
    s.line = l.y;
    pane_loc_to_row(p, l, &s.row, &rvx);
    return s;
}

void pane_relocate(Pane *p) {
    Cursor *c = ACUR(p);
    int height = pane_text_height(p);
    int margin = BOPT(p->buf, O_SCROLLMARGIN);
    SLoc cs, start, bstart = {0, 0}, bend;
    if (margin > (height - 1) / 2) margin = (height - 1) / 2;
    if (margin < 0) margin = 0;
    p->top = clampi(p->top, 0, p->buf->nlines - 1);
    if (BOPT(p->buf, O_SOFTWRAP)) {
        int r = pane_line_rows(p, p->top);
        if (p->toprow >= r) p->toprow = r - 1;
    } else {
        p->toprow = 0;
    }
    cs = sloc_from_loc(p, c->loc);
    start.line = p->top;
    start.row = p->toprow;
    bend = sloc_from_loc(p, buf_end(p->buf));
    if (sloc_cmp(cs, sloc_scroll(p, start, margin)) < 0 &&
        sloc_cmp(cs, sloc_scroll(p, bstart, margin - 1)) > 0) {
        start = sloc_scroll(p, cs, -margin);
    } else if (sloc_cmp(cs, start) < 0) {
        start = cs;
    }
    if (sloc_cmp(cs, sloc_scroll(p, start, height - 1 - margin)) > 0 &&
        sloc_cmp(cs, sloc_scroll(p, bend, -margin)) <= 0) {
        start = sloc_scroll(p, cs, -height + 1 + margin);
    } else if (sloc_cmp(cs, sloc_scroll(p, bend, -margin)) > 0 &&
               sloc_cmp(cs, sloc_scroll(p, start, height - 1)) > 0) {
        start = sloc_scroll(p, bend, -height + 1);
    }
    p->top = start.line;
    p->toprow = start.row;
    if (!BOPT(p->buf, O_SOFTWRAP)) {
        int cx = pane_visual_x(p, c->loc.y, c->loc.x);
        int bw = pane_text_width(p);
        int rw = 1, cp = buf_char_at(p->buf, c->loc);
        if (cp > 0 && cp != '\t') rw = char_width(cp) ? char_width(cp) : 1;
        if (cx < p->left) p->left = cx;
        if (cx + rw > p->left + bw) p->left = cx - bw + rw;
    } else {
        p->left = 0;
    }
}

void pane_center(Pane *p) {
    SLoc cs = sloc_from_loc(p, ACUR(p)->loc), s;
    s = sloc_scroll(p, cs, -pane_text_height(p) / 2);
    p->top = s.line;
    p->toprow = s.row;
}

void pane_scroll(Pane *p, int n) {
    SLoc s;
    SLoc bend = sloc_from_loc(p, buf_end(p->buf));
    SLoc maxs = sloc_scroll(p, bend, -pane_text_height(p) + 1);
    s.line = p->top;
    s.row = p->toprow;
    s = sloc_scroll(p, s, n);
    if (n > 0 && sloc_cmp(s, maxs) > 0) {
        /* don't scroll past the end (unless already there) */
        SLoc cur;
        cur.line = p->top;
        cur.row = p->toprow;
        s = sloc_cmp(cur, maxs) > 0 ? cur : maxs;
    }
    p->top = s.line;
    p->toprow = s.row;
}

/* map a screen position inside the pane's text area to a buffer location */
int pane_screen_to_loc(Pane *p, int sx, int sy, Loc *out) {
    int gw = pane_gutter_width(p);
    int ry = sy - p->y, rx = sx - p->x - gw;
    SLoc s;
    if (ry < 0) ry = 0;
    if (ry >= pane_text_height(p)) ry = pane_text_height(p) - 1;
    if (rx < 0) rx = 0;
    s.line = p->top;
    s.row = p->toprow;
    s = sloc_scroll(p, s, ry); /* stops at the end of the buffer */
    if (BOPT(p->buf, O_SOFTWRAP)) *out = pane_row_to_loc(p, s.line, s.row, rx);
    else *out = loc_make(pane_x_from_visual(p, s.line, rx + p->left), s.line);
    return 1;
}

/* ---------------- cursor movement ---------------- */

void cur_store_vx(Pane *p, Cursor *c) {
    int row, rvx;
    c->loc = buf_clamp(p->buf, c->loc);
    pane_loc_to_row(p, c->loc, &row, &rvx);
    c->lastvx = rvx;
}

/* micro's Cursor.UpN/DownN: clamp the line; if the line didn't change,
   go to the start (up) or end (down) of the line and remember that column */
void cur_up(Pane *p, Cursor *c, int n) {
    int row, rvx;
    if (!BOPT(p->buf, O_SOFTWRAP)) {
        int y = c->loc.y - n;
        if (y < 0) y = 0;
        if (y == c->loc.y) { c->loc.x = 0; cur_store_vx(p, c); return; }
        c->loc = loc_make(pane_x_from_visual(p, y, c->lastvx), y);
        return;
    }
    pane_loc_to_row(p, c->loc, &row, &rvx);
    {
        int y = c->loc.y, moved = 0;
        while (n > 0) {
            if (row > 0) row--;
            else if (y > 0) { y--; row = pane_line_rows(p, y) - 1; }
            else break;
            n--;
            moved = 1;
        }
        if (!moved) { c->loc.x = 0; cur_store_vx(p, c); return; }
        c->loc = pane_row_to_loc(p, y, row, c->lastvx);
    }
}

void cur_down(Pane *p, Cursor *c, int n) {
    int row, rvx;
    Buffer *b = p->buf;
    if (!BOPT(b, O_SOFTWRAP)) {
        int y = c->loc.y + n;
        if (y > b->nlines - 1) y = b->nlines - 1;
        if (y == c->loc.y) { c->loc.x = b->lines[y].len; cur_store_vx(p, c); return; }
        c->loc = loc_make(pane_x_from_visual(p, y, c->lastvx), y);
        return;
    }
    pane_loc_to_row(p, c->loc, &row, &rvx);
    {
        int y = c->loc.y, moved = 0, rows = pane_line_rows(p, y);
        while (n > 0) {
            if (row + 1 < rows) row++;
            else if (y + 1 < b->nlines) { y++; row = 0; rows = pane_line_rows(p, y); }
            else break;
            n--;
            moved = 1;
        }
        if (!moved) { c->loc.x = b->lines[y].len; cur_store_vx(p, c); return; }
        c->loc = pane_row_to_loc(p, y, row, c->lastvx);
    }
}

static int tab_movement_width(Pane *p, Cursor *c, int left) {
    /* with tabmovement, move over a whole indent of spaces at once */
    const Line *ln = &p->buf->lines[c->loc.y];
    int ts = pane_tabsize(p->buf), i, ws = 0;
    if (!BOPT(p->buf, O_TABMOVEMENT) || !BOPT(p->buf, O_TABSTOSPACES)) return 1;
    while (ws < ln->len && ln->s[ws] == ' ') ws++;
    if (left) {
        if (c->loc.x > ws || c->loc.x % ts != 0 || c->loc.x == 0) return 1;
        return ts;
    }
    if (c->loc.x >= ws || c->loc.x % ts != 0) return 1;
    for (i = c->loc.x; i < c->loc.x + ts; i++)
        if (i >= ln->len || ln->s[i] != ' ') return 1;
    return ts;
}

void cur_left(Pane *p, Cursor *c) {
    int n = tab_movement_width(p, c, 1);
    c->loc = buf_move_chars(p->buf, c->loc, -n);
    cur_store_vx(p, c);
}

void cur_right(Pane *p, Cursor *c) {
    int n = tab_movement_width(p, c, 0);
    c->loc = buf_move_chars(p->buf, c->loc, n);
    cur_store_vx(p, c);
}
