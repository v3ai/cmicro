/*
 * buffer.c - text storage, file I/O, undo/redo and highlight state.
 */
#include "micro.h"
#include "platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_THRESHOLD 1000

/* Lines and texts use int lengths, so a buffer holds less than 2 GiB.
   (The slack keeps len + small constants from overflowing.) */
#define MAX_TEXT (INT_MAX - 64)

/* syntax detection only looks at the start of the first lines */
#define DETECT_BYTES 4096

void (*buf_edit_hook)(Buffer *b, int type, Loc start, Loc end) = NULL;
Loc (*buf_get_cursor_hook)(Buffer *b, int *num) = NULL;
void (*buf_set_cursor_hook)(Buffer *b, Loc l, int num) = NULL;

int loc_cmp(Loc a, Loc b) {
    if (a.y != b.y) return a.y < b.y ? -1 : 1;
    if (a.x != b.x) return a.x < b.x ? -1 : 1;
    return 0;
}

Loc loc_make(int x, int y) {
    Loc l;
    l.x = x;
    l.y = y;
    return l;
}

/* ---------------- lines ---------------- */

/* exactly sized: most lines of a loaded file are never edited */
static void line_init(Line *l, const char *s, int len) {
    memset(l, 0, sizeof *l);
    l->cap = len + 1;
    l->s = xmalloc((size_t)l->cap);
    if (len) memcpy(l->s, s, (size_t)len);
    l->len = len;
    l->s[len] = 0;
}

static void line_free(Line *l) {
    free(l->s);
    free(l->hl);
}

/* room for n bytes (+ NUL); n <= MAX_TEXT */
static void line_reserve(Line *l, int n) {
    if (n + 1 > l->cap) {
        size_t c = l->cap < 16 ? 16 : (size_t)l->cap * 2;
        if (c < (size_t)n + 1) c = (size_t)n + 1;
        if (c > (size_t)INT_MAX) c = (size_t)INT_MAX;
        l->s = xrealloc(l->s, c);
        l->cap = (int)c;
    }
}

static void lines_reserve(Buffer *b, int n) {
    if (n > b->cap) {
        size_t c = b->cap ? (size_t)b->cap * 2 : 64;
        if (c < (size_t)n) c = (size_t)n;
        if (c > (size_t)INT_MAX) c = (size_t)INT_MAX;
        b->lines = xrealloc(b->lines, sizeof *b->lines * c);
        b->cap = (int)c;
    }
}

static void lines_insert(Buffer *b, int at, int count) {
    lines_reserve(b, b->nlines + count);
    memmove(b->lines + at + count, b->lines + at, sizeof *b->lines * (size_t)(b->nlines - at));
    b->nlines += count;
}

static void lines_delete(Buffer *b, int at, int count) {
    int i;
    for (i = at; i < at + count; i++) line_free(&b->lines[i]);
    memmove(b->lines + at, b->lines + at + count, sizeof *b->lines * (size_t)(b->nlines - at - count));
    b->nlines -= count;
}

/* ---------------- construction ---------------- */

static void buf_init_opts(Buffer *b) {
    int i;
    for (i = 0; i < O_COUNT; i++) {
        b->opts[i].i = gopts[i].i;
        b->opts[i].s = gopts[i].s ? xstrdup(gopts[i].s) : NULL;
        b->local[i] = 0;
    }
}

Buffer *buf_new_empty(void) {
    Buffer *b = xcalloc(1, sizeof *b);
    buf_init_opts(b);
    lines_insert(b, 0, 1);
    line_init(&b->lines[0], "", 0);
    b->path = xstrdup("");
    b->name = xstrdup("");
    b->recording = 1;
    b->is_new = 1;
    b->last_search = NULL;
    return b;
}

/*
 * Text loading, fed in chunks so a file is never held twice in memory.
 * A UTF-8 BOM is dropped (and remembered); the first line ending decides
 * the file format: with CRLF there, every "\r\n" becomes a line break,
 * otherwise "\r"s stay in the text.  A '\r' that is not followed by '\n'
 * (including at the very end) is always kept, so saving gives back the
 * same bytes.
 */
typedef struct {
    Buffer *b;
    Line cur;        /* partial line spanning chunks */
    int have_cur;
    int dos, first;  /* first: no line break seen yet */
    int started;
    size_t total;
    int too_big;
} Loader;

static void loader_start(Loader *ld, Buffer *b) {
    int i;
    memset(ld, 0, sizeof *ld);
    ld->b = b;
    ld->first = 1;
    for (i = 0; i < b->nlines; i++) line_free(&b->lines[i]);
    b->nlines = 0;
}

/* add a complete line; `s` ends just before its '\n' (nl: one followed) */
static void loader_line(Loader *ld, const char *s, int len, int nl) {
    Buffer *b = ld->b;
    Line *l;
    if (nl && len > 0 && s[len - 1] == '\r') {
        if (ld->first) ld->dos = 1;
        if (ld->dos) len--;
    }
    if (nl) ld->first = 0;
    lines_reserve(b, b->nlines + 1);
    l = &b->lines[b->nlines++];
    if (ld->have_cur) {
        /* s is cur.s: hand the storage over, trimmed to size */
        *l = ld->cur;
        l->len = len;
        l->s[len] = 0;
        if (l->cap > len + 1 + 64) {
            l->s = xrealloc(l->s, (size_t)len + 1);
            l->cap = len + 1;
        }
        ld->have_cur = 0;
    } else {
        line_init(l, s, len);
    }
}

static void loader_feed(Loader *ld, const char *p, size_t n) {
    const char *end = p + n;
    if (ld->too_big || n == 0) return;
    if (n > (size_t)MAX_TEXT - ld->total) { ld->too_big = 1; return; }
    ld->total += n;
    if (!ld->started) {
        ld->started = 1;
        if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            ld->b->bom = 1;
            p += 3;
        }
    }
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        int seg = (int)(stop - p);
        if (ld->have_cur || !nl) {
            Line *c = &ld->cur;
            if (!ld->have_cur) {
                memset(c, 0, sizeof *c);
                ld->have_cur = 1;
            }
            line_reserve(c, c->len + seg);
            memcpy(c->s + c->len, p, (size_t)seg);
            c->len += seg;
            c->s[c->len] = 0;
            if (nl) loader_line(ld, c->s, c->len, 1);
        } else {
            loader_line(ld, p, seg, 1);
        }
        p = nl ? nl + 1 : end;
    }
}

/* returns 0, or -1 if the text is too large (the buffer is then empty) */
static int loader_finish(Loader *ld) {
    Buffer *b = ld->b;
    if (ld->too_big) {
        int i;
        for (i = 0; i < b->nlines; i++) line_free(&b->lines[i]);
        b->nlines = 0;
        if (ld->have_cur) free(ld->cur.s);
        ld->have_cur = 0;
    }
    /* the last line (without a line break, possibly empty) */
    if (ld->have_cur) loader_line(ld, ld->cur.s, ld->cur.len, 0);
    else loader_line(ld, "", 0, 0);
    /* the line array doubled while loading: give back the slack */
    if (b->cap > b->nlines + b->nlines / 8 + 64) {
        b->cap = b->nlines + 64;
        b->lines = xrealloc(b->lines, sizeof *b->lines * (size_t)b->cap);
    }
    if (!ld->first) {
        /* the file has line breaks: keep its line endings */
        free(b->opts[O_FILEFORMAT].s);
        b->opts[O_FILEFORMAT].s = xstrdup(ld->dos ? "dos" : "unix");
        b->local[O_FILEFORMAT] = 1;
    }
    b->states_valid = 0;
    return ld->too_big ? -1 : 0;
}

void buf_set_text(Buffer *b, const char *text, int len) {
    Loader ld;
    loader_start(&ld, b);
    if (len > 0) loader_feed(&ld, text, (size_t)len);
    loader_finish(&ld);
}

/* Replace b's text with the file's contents.  Returns 0; -1 if it can't
   be opened (*exists tells whether it is there at all); -2 on a read
   error or a file too large to edit (the buffer is then empty). */
static int load_file(Buffer *b, const char *path, int *exists) {
    FILE *f = plat_fopen(path, "rb");
    Loader ld;
    char *chunk;
    size_t n;
    int rc;
    if (exists) *exists = 1;
    if (!f) {
        if (exists) *exists = plat_exists(path);
        return -1;
    }
    chunk = xmalloc(1 << 20);
    loader_start(&ld, b);
    while ((n = fread(chunk, 1, 1 << 20, f)) > 0) {
        loader_feed(&ld, chunk, n);
        if (ld.too_big) break;
    }
    rc = ferror(f) ? -2 : 0;
    fclose(f);
    free(chunk);
    if (loader_finish(&ld) != 0) rc = -2;
    return rc;
}

/* for backup.c: load a file's text into b; 0 on success */
int buf_load_file(Buffer *b, const char *path) { return load_file(b, path, NULL) == 0 ? 0 : -1; }

Buffer *buf_from_string(const char *name, const char *text, int type) {
    Buffer *b = buf_new_empty();
    buf_set_text(b, text, (int)strlen(text));
    free(b->path);
    free(b->name);
    b->path = xstrdup(name);
    b->name = xstrdup(name);
    b->type = type;
    b->is_new = 0;
    if (type == BT_HELP || type == BT_LOG) {
        b->opts[O_READONLY].i = 1;
        b->local[O_READONLY] = 1;
    }
    buf_update_rules(b);
    return b;
}

void buf_set_name(Buffer *b, const char *path) {
    char *ap;
    free(b->path);
    free(b->name);
    free(b->abspath);
    b->path = xstrdup(path);
    ap = plat_realpath(path);
    if (!ap && *path) {
        /* file may not exist yet: resolve the directory */
        char *dir = path_dir(path);
        char *rd = plat_realpath(dir);
        if (rd) ap = path_join(rd, path_base(path));
        else ap = xstrdup(path);
        free(rd);
        free(dir);
    }
    b->abspath = ap;
    b->name = xstrdup(path);
    if (BOPT(b, O_BASENAME) && *path) {
        free(b->name);
        b->name = xstrdup(path_base(path));
    }
}

/* free a buffer that must not take its crash-recovery backup with it */
static void buf_discard(Buffer *b) {
    b->path[0] = 0;
    buf_free(b);
}

Buffer *buf_open(const char *path, char **err) {
    Buffer *b;
    int exists, rc;
    char *exp = expand_home(path);
    if (err) *err = NULL;
    if (plat_is_dir(exp)) {
        if (err) *err = xsprintf("%s is a directory", exp);
        free(exp);
        return NULL;
    }
    b = buf_new_empty();
    buf_set_name(b, exp);
    rc = load_file(b, exp, &exists);
    if (rc == 0) {
        b->is_new = 0;
        b->mtime = plat_mtime(exp);
    } else if (rc == -2 || exists) {
        /* never show part of a file: saving it would destroy the rest */
        if (err) {
            if (rc == -1) *err = xsprintf("Error opening %s: permission denied", exp);
            else *err = xsprintf("Error reading %s (files must be smaller than 2 GB)", exp);
        }
        buf_discard(b);
        free(exp);
        return NULL;
    }
    free(exp);
    if (backup_check_on_open(b) < 0) {
        /* 'abort': open an empty buffer instead (the backup stays) */
        buf_discard(b);
        b = buf_new_empty();
    }
    buf_update_rules(b);
    return b;
}

static void evstack_clear(EvStack *s) {
    int i;
    for (i = 0; i < s->n; i++) free(s->ev[i].text);
    s->n = 0;
}

void buf_free(Buffer *b) {
    int i;
    if (!b) return;
    if (b->type == BT_DEFAULT && *b->path) backup_remove(b);
    for (i = 0; i < b->nlines; i++) line_free(&b->lines[i]);
    free(b->lines);
    for (i = 0; i < O_COUNT; i++) free(b->opts[i].s);
    evstack_clear(&b->undo);
    evstack_clear(&b->redo);
    free(b->undo.ev);
    free(b->redo.ev);
    free(b->path);
    free(b->abspath);
    free(b->name);
    free(b->last_search);
    free(b);
}

int buf_modified(const Buffer *b) {
    if (b->type != BT_DEFAULT) return 0;
    return b->cur_id != b->saved_id;
}

void buf_mark_saved(Buffer *b) { b->saved_id = b->cur_id; }

/* ---------------- text access ---------------- */

Loc buf_end(const Buffer *b) { return loc_make(b->lines[b->nlines - 1].len, b->nlines - 1); }

Loc buf_clamp(const Buffer *b, Loc l) {
    if (l.y < 0) return loc_make(0, 0);
    if (l.y >= b->nlines) return buf_end(b);
    if (l.x < 0) l.x = 0;
    if (l.x > b->lines[l.y].len) l.x = b->lines[l.y].len;
    /* snap to char boundary */
    while (l.x > 0 && l.x < b->lines[l.y].len && ((unsigned char)b->lines[l.y].s[l.x] & 0xC0) == 0x80) l.x--;
    return l;
}

Loc buf_move_chars(const Buffer *b, Loc l, int n) {
    while (n > 0) {
        const Line *ln = &b->lines[l.y];
        if (l.x < ln->len) l.x = utf8_next(ln->s, ln->len, l.x);
        else if (l.y < b->nlines - 1) { l.y++; l.x = 0; }
        else break;
        n--;
    }
    while (n < 0) {
        if (l.x > 0) l.x = utf8_prev(b->lines[l.y].s, l.x);
        else if (l.y > 0) { l.y--; l.x = b->lines[l.y].len; }
        else break;
        n++;
    }
    return l;
}

int buf_char_at(const Buffer *b, Loc l) {
    int cp;
    const Line *ln;
    if (l.y < 0 || l.y >= b->nlines) return -1;
    ln = &b->lines[l.y];
    if (l.x < 0 || l.x >= ln->len) return -1;
    utf8_decode(ln->s + l.x, ln->len - l.x, &cp);
    return cp;
}

/* bytes in the buffer's text (lines joined with '\n') */
static size_t text_size(const Buffer *b) {
    size_t n = 0;
    int y;
    for (y = 0; y < b->nlines; y++) n += (size_t)b->lines[y].len + 1;
    return n - 1;
}

char *buf_get_text(const Buffer *b, Loc a, Loc z, int *len) {
    size_t total = 0, n;
    int y;
    char *r, *o;
    a = buf_clamp(b, a);
    z = buf_clamp(b, z);
    if (loc_cmp(a, z) > 0) { Loc t = a; a = z; z = t; }
    for (y = a.y; y <= z.y; y++) {
        int from = y == a.y ? a.x : 0;
        int to = y == z.y ? z.x : b->lines[y].len;
        total += (size_t)(to - from) + (y < z.y);
    }
    /* buf_insert keeps buffers below MAX_TEXT; never overflow an int */
    if (total > (size_t)MAX_TEXT) total = (size_t)MAX_TEXT;
    r = o = xmalloc(total + 1);
    n = total;
    for (y = a.y; y <= z.y && n > 0; y++) {
        const Line *ln = &b->lines[y];
        int from = y == a.y ? a.x : 0;
        size_t k = (size_t)((y == z.y ? z.x : ln->len) - from);
        if (k > n) k = n;
        memcpy(o, ln->s + from, k);
        o += k;
        n -= k;
        if (y < z.y && n > 0) { *o++ = '\n'; n--; }
    }
    *o = 0;
    if (len) *len = (int)(o - r);
    return r;
}

char *buf_full_text(const Buffer *b, int *len) {
    return buf_get_text(b, loc_make(0, 0), buf_end(b), len);
}

const char *buf_leading_ws(const Buffer *b, int y, int *len) {
    const Line *ln = &b->lines[y];
    int i = 0;
    while (i < ln->len && (ln->s[i] == ' ' || ln->s[i] == '\t')) i++;
    *len = i;
    return ln->s;
}

char *buf_indent_string(const Buffer *b) {
    if (BOPT(b, O_TABSTOSPACES)) {
        int n = BOPT(b, O_TABSIZE), i;
        char *s = xmalloc((size_t)n + 1);
        for (i = 0; i < n; i++) s[i] = ' ';
        s[n] = 0;
        return s;
    }
    return xstrdup("\t");
}

/* ---------------- raw edits ---------------- */

static void mark_dirty(Buffer *b, int y0, int y1) {
    int y;
    if (y0 < b->states_valid) b->states_valid = y0;
    for (y = y0; y <= y1 && y < b->nlines; y++) b->lines[y].match_valid = 0;
}

static Loc raw_insert(Buffer *b, Loc at, const char *text, int len) {
    Line *ln;
    int i, nl = 0, tail_len;
    char *tail;
    Loc end;
    at = buf_clamp(b, at);
    for (i = 0; i < len; i++) if (text[i] == '\n') nl++;
    ln = &b->lines[at.y];
    if (nl == 0) {
        line_reserve(ln, ln->len + len);
        memmove(ln->s + at.x + len, ln->s + at.x, (size_t)(ln->len - at.x));
        memcpy(ln->s + at.x, text, (size_t)len);
        ln->len += len;
        ln->s[ln->len] = 0;
        mark_dirty(b, at.y, at.y);
        return loc_make(at.x + len, at.y);
    }
    /* split */
    tail_len = ln->len - at.x;
    tail = xstrndup(ln->s + at.x, (size_t)tail_len);
    ln->len = at.x;
    ln->s[ln->len] = 0;
    lines_insert(b, at.y + 1, nl);
    {
        int seg = 0, y = at.y;
        for (i = 0; i <= len; i++) {
            if (i == len || text[i] == '\n') {
                if (y == at.y) {
                    Line *l0 = &b->lines[y];
                    line_reserve(l0, l0->len + (i - seg));
                    memcpy(l0->s + l0->len, text + seg, (size_t)(i - seg));
                    l0->len += i - seg;
                    l0->s[l0->len] = 0;
                } else {
                    line_init(&b->lines[y], text + seg, i - seg);
                }
                seg = i + 1;
                y++;
            }
        }
        end.y = at.y + nl;
        end.x = b->lines[end.y].len;
        ln = &b->lines[end.y];
        line_reserve(ln, ln->len + tail_len);
        memcpy(ln->s + ln->len, tail, (size_t)tail_len);
        ln->len += tail_len;
        ln->s[ln->len] = 0;
    }
    free(tail);
    mark_dirty(b, at.y, end.y);
    return end;
}

static void raw_remove(Buffer *b, Loc a, Loc z) {
    Line *la, *lz;
    a = buf_clamp(b, a);
    z = buf_clamp(b, z);
    if (loc_cmp(a, z) >= 0) return;
    la = &b->lines[a.y];
    if (a.y == z.y) {
        memmove(la->s + a.x, la->s + z.x, (size_t)(la->len - z.x));
        la->len -= z.x - a.x;
        la->s[la->len] = 0;
        mark_dirty(b, a.y, a.y);
        return;
    }
    lz = &b->lines[z.y];
    line_reserve(la, a.x + lz->len - z.x);
    memcpy(la->s + a.x, lz->s + z.x, (size_t)(lz->len - z.x));
    la->len = a.x + lz->len - z.x;
    la->s[la->len] = 0;
    lines_delete(b, a.y + 1, z.y - a.y);
    mark_dirty(b, a.y, a.y);
}

static void push_event(EvStack *s, TextEvent *e) {
    if (s->n == s->cap) {
        if (s->cap > INT_MAX / 2 / (int)sizeof *s->ev) {
            /* absurdly long history: forget the oldest half */
            int k, drop = s->n / 2;
            for (k = 0; k < drop; k++) free(s->ev[k].text);
            memmove(s->ev, s->ev + drop, sizeof *s->ev * (size_t)(s->n - drop));
            s->n -= drop;
        } else {
            s->cap = s->cap ? s->cap * 2 : 64;
        }
        s->ev = xrealloc(s->ev, sizeof *s->ev * (size_t)s->cap);
    }
    s->ev[s->n++] = *e;
}

static void record(Buffer *b, int type, Loc start, Loc end, const char *text, int len) {
    TextEvent e;
    if (!b->recording) return;
    e.type = type;
    e.start = start;
    e.end = end;
    e.text = xstrndup(text, (size_t)len);
    e.len = len;
    e.time = plat_now_ms();
    e.id = ++b->next_id;
    e.cnum = 0;
    e.cursor = buf_get_cursor_hook ? buf_get_cursor_hook(b, &e.cnum) : start;
    push_event(&b->undo, &e);
    b->cur_id = e.id;
    evstack_clear(&b->redo);
}

Loc buf_insert(Buffer *b, Loc at, const char *text, int len) {
    Loc end;
    if (len <= 0) return at;
    at = buf_clamp(b, at);
    /* keep every length an int (a big paste checks the whole buffer) */
    if (len > MAX_TEXT - b->lines[at.y].len ||
        (len > 65536 && text_size(b) > (size_t)(MAX_TEXT - len))) {
        editor_log("insert of %d bytes refused: buffers must stay under 2 GB", len);
        return at;
    }
    end = raw_insert(b, at, text, len);
    if (b->recording) record(b, EV_INSERT, at, end, text, len);
    if (buf_edit_hook) buf_edit_hook(b, EV_INSERT, at, end);
    return end;
}

void buf_remove(Buffer *b, Loc a, Loc z) {
    char *text;
    int len;
    a = buf_clamp(b, a);
    z = buf_clamp(b, z);
    if (loc_cmp(a, z) > 0) { Loc t = a; a = z; z = t; }
    if (loc_cmp(a, z) == 0) return;
    text = buf_get_text(b, a, z, &len);
    raw_remove(b, a, z);
    if (b->recording) record(b, EV_REMOVE, a, z, text, len);
    free(text);
    if (buf_edit_hook) buf_edit_hook(b, EV_REMOVE, a, z);
}

Loc buf_replace(Buffer *b, Loc a, Loc z, const char *text, int len) {
    buf_remove(b, a, z);
    if (loc_cmp(a, z) > 0) a = z;
    return buf_insert(b, a, text, len);
}

/* ---------------- undo / redo ---------------- */

static void apply_inverse(Buffer *b, TextEvent *e) {
    int saved = b->recording;
    b->recording = 0;
    if (e->type == EV_INSERT) {
        raw_remove(b, e->start, e->end);
        if (buf_edit_hook) buf_edit_hook(b, EV_REMOVE, e->start, e->end);
    } else {
        Loc end = raw_insert(b, e->start, e->text, e->len);
        if (buf_edit_hook) buf_edit_hook(b, EV_INSERT, e->start, end);
    }
    b->recording = saved;
}

static void apply_forward(Buffer *b, TextEvent *e) {
    int saved = b->recording;
    b->recording = 0;
    if (e->type == EV_INSERT) {
        Loc end = raw_insert(b, e->start, e->text, e->len);
        if (buf_edit_hook) buf_edit_hook(b, EV_INSERT, e->start, end);
    } else {
        raw_remove(b, e->start, e->end);
        if (buf_edit_hook) buf_edit_hook(b, EV_REMOVE, e->start, e->end);
    }
    b->recording = saved;
}

int buf_undo(Buffer *b) {
    long long start, endt;
    if (b->undo.n == 0) return 0;
    start = b->undo.ev[b->undo.n - 1].time;
    endt = start - (start % UNDO_THRESHOLD);
    while (b->undo.n > 0 && b->undo.ev[b->undo.n - 1].time >= endt) {
        TextEvent e = b->undo.ev[--b->undo.n];
        apply_inverse(b, &e);
        if (buf_set_cursor_hook) buf_set_cursor_hook(b, e.cursor, e.cnum);
        push_event(&b->redo, &e);
    }
    b->cur_id = b->undo.n ? b->undo.ev[b->undo.n - 1].id : 0;
    return 1;
}

int buf_redo(Buffer *b) {
    long long start, endt;
    if (b->redo.n == 0) return 0;
    start = b->redo.ev[b->redo.n - 1].time;
    endt = start - (start % UNDO_THRESHOLD) + UNDO_THRESHOLD;
    while (b->redo.n > 0 && b->redo.ev[b->redo.n - 1].time <= endt) {
        TextEvent e = b->redo.ev[--b->redo.n];
        if (buf_set_cursor_hook) buf_set_cursor_hook(b, e.cursor, e.cnum);
        apply_forward(b, &e);
        push_event(&b->undo, &e);
        b->cur_id = e.id;
    }
    return 1;
}

/* ---------------- saving ---------------- */

/* the file's bytes: BOM, lines, and line breaks per `fileformat` */
static long long serialized_size(const Buffer *b) {
    int dos = !strcmp(BOPTS(b, O_FILEFORMAT), "dos");
    return (long long)text_size(b) + (dos ? b->nlines - 1 : 0) + (b->bom ? 3 : 0);
}

/* write the file's bytes through put(); 0 on success */
int buf_serialize(const Buffer *b, int (*put)(void *ctx, const char *p, size_t n), void *ctx) {
    int y, dos = !strcmp(BOPTS(b, O_FILEFORMAT), "dos");
    const char *eol = dos ? "\r\n" : "\n";
    size_t eol_len = dos ? 2 : 1;
    if (b->bom && put(ctx, "\xEF\xBB\xBF", 3) != 0) return -1;
    for (y = 0; y < b->nlines; y++) {
        const Line *ln = &b->lines[y];
        if (ln->len && put(ctx, ln->s, (size_t)ln->len) != 0) return -1;
        if (y < b->nlines - 1 && put(ctx, eol, eol_len) != 0) return -1;
    }
    return 0;
}

static int put_save(void *ctx, const char *p, size_t n) { return plat_save_write(ctx, p, n); }

/* write b's file bytes to `path` safely (see plat_save_open); 0 on
   success, else -1 (-2: permission denied) with *err set */
int buf_write_file(const Buffer *b, const char *path, int flags, char **err) {
    int denied = 0;
    PlatSave *s = plat_save_open(path, serialized_size(b), flags, &denied);
    if (!s) {
        if (err) *err = xstrdup(plat_save_error());
        return denied ? -2 : -1;
    }
    if (buf_serialize(b, put_save, s) != 0) {
        if (err) *err = xstrdup(plat_save_error());
        plat_save_close(s, 0);
        return -1;
    }
    if (plat_save_close(s, 1) != 0) {
        if (err) *err = xstrdup(plat_save_error());
        return -1;
    }
    return 0;
}

int buf_save(Buffer *b, const char *path, char **err) {
    int y, rc;
    char *exp, *why = NULL;
    if (err) *err = NULL;
    if (!path || !*path) { if (err) *err = xstrdup("No filename"); return -1; }
    exp = expand_home(path);
    if (BOPT(b, O_RMTRAILINGWS)) {
        for (y = 0; y < b->nlines; y++) {
            Line *ln = &b->lines[y];
            int e = ln->len;
            while (e > 0 && (ln->s[e - 1] == ' ' || ln->s[e - 1] == '\t')) e--;
            if (e < ln->len) buf_remove(b, loc_make(e, y), loc_make(ln->len, y));
        }
    }
    if (BOPT(b, O_EOFNEWLINE) && b->lines[b->nlines - 1].len > 0) {
        raw_insert(b, buf_end(b), "\n", 1); /* not undoable, cursors stay put */
    }
    if (BOPT(b, O_MKPARENTS)) {
        char *dir = path_dir(exp);
        if (!plat_is_dir(dir)) plat_mkdir_p(dir);
        free(dir);
    }
    {
        char *dir = path_dir(exp);
        if (!plat_is_dir(dir)) {
            if (err) *err = xsprintf("Parent dirs don't exist, enable 'mkparents' for auto creation");
            free(dir);
            free(exp);
            return -1;
        }
        free(dir);
    }
    rc = buf_write_file(b, exp, 0, &why);
    if (rc != 0) {
        /* a failed save leaves the file as it was (or, when it had to be
           rewritten in place, the crash-recovery backup in place) */
        if (err) *err = rc == -2 ? xsprintf("Permission denied: %s", exp) : xsprintf("Error writing %s", why);
        free(why);
        free(exp);
        return rc;
    }
    {
        int was_unnamed = !*b->path || strcmp(b->path, path) != 0;
        if (was_unnamed) {
            buf_set_name(b, path);
            if (!strcmp(BOPTS(b, O_FILETYPE), "unknown")) buf_update_rules(b);
        }
    }
    b->is_new = 0;
    b->mtime = plat_mtime(exp);
    free(exp);
    buf_mark_saved(b);
    backup_remove(b);
    return 0;
}

int buf_reload(Buffer *b, char **err) {
    char *exp = expand_home(b->path), *txt;
    Buffer *tmp = buf_new_empty();
    int tl;
    if (err) *err = NULL;
    if (load_file(tmp, exp, NULL) != 0) {
        if (err) *err = xsprintf("Could not read %s", exp);
        buf_free(tmp);
        free(exp);
        return -1;
    }
    txt = buf_full_text(tmp, &tl);
    buf_free(tmp);
    /* replace whole content as one undoable change; cursors are restored
       (clamped) by the caller-visible hook afterwards */
    {
        void (*hook)(Buffer *, int, Loc, Loc) = buf_edit_hook;
        buf_edit_hook = NULL;
        buf_replace(b, loc_make(0, 0), buf_end(b), "", 0);
        buf_insert(b, loc_make(0, 0), txt, tl);
        buf_edit_hook = hook;
        if (hook) hook(b, EV_REMOVE, buf_end(b), buf_end(b)); /* lets panes clamp */
    }
    free(txt);
    b->mtime = plat_mtime(exp);
    free(exp);
    buf_mark_saved(b);
    return 0;
}

/* ---------------- syntax ---------------- */

void buf_invalidate_hl(Buffer *b, int from) {
    int y;
    if (from < 0) from = 0;
    if (from < b->states_valid) b->states_valid = from;
    for (y = from; y < b->nlines; y++) b->lines[y].match_valid = 0;
}

void buf_update_rules(Buffer *b) {
    const char *ft = BOPTS(b, O_FILETYPE);
    if (!strcmp(ft, "off")) {
        b->syn = NULL;
    } else if (!strcmp(ft, "unknown") || !*ft) {
        int n = b->nlines < 200 ? b->nlines : 200, i;
        const char **ls = xmalloc(sizeof *ls * (size_t)(n + 1));
        char **cut = xcalloc((size_t)n + 1, sizeof *cut);
        /* headers and signatures match at the start of a line: don't run
           every syntax's regexes over a huge one-line file */
        for (i = 0; i < n; i++) {
            if (b->lines[i].len > DETECT_BYTES) {
                int k = DETECT_BYTES;
                while (k > 0 && ((unsigned char)b->lines[i].s[k] & 0xC0) == 0x80) k--;
                cut[i] = xstrndup(b->lines[i].s, (size_t)k);
            }
            ls[i] = cut[i] ? cut[i] : b->lines[i].s;
        }
        b->syn = syntax_detect(b->path, n > 0 ? ls[0] : "", ls, n, BOPT(b, O_DETECTLIMIT));
        for (i = 0; i < n; i++) free(cut[i]);
        free(cut);
        free(ls);
        if (b->syn && strcmp(b->syn->filetype, "unknown") != 0) {
            free(b->opts[O_FILETYPE].s);
            b->opts[O_FILETYPE].s = xstrdup(b->syn->filetype);
        }
    } else {
        b->syn = syntax_by_filetype(ft);
    }
    if (b->syn) {
        char *err = NULL;
        if (syntax_load(b->syn, &err) != 0) b->syn = NULL;
        if (err) { editor_log("%s", err); free(err); }
    }
    b->states_valid = 0;
    buf_invalidate_hl(b, 0);
}

void buf_ensure_hl(Buffer *b, int y) {
    Line *ln;
    Region *prev;
    if (!b->syn || !BOPT(b, O_SYNTAX) || y >= b->nlines) return;
    while (b->states_valid < y) {
        int j = b->states_valid;
        Region *st = j > 0 ? b->lines[j - 1].state : NULL;
        Line *lj = &b->lines[j];
        if (lj->match_valid && lj->hl_start == st) {
            /* end state from the last full highlight is still right */
        } else {
            lj->state = syntax_highlight_line(b->syn, st, lj->s, lj->len, NULL);
        }
        b->states_valid++;
    }
    ln = &b->lines[y];
    prev = y > 0 ? b->lines[y - 1].state : NULL;
    if (!ln->match_valid || ln->hl_start != prev) {
        if (ln->hlcap < ln->len + 1) {
            ln->hlcap = ln->len + 16;
            ln->hl = xrealloc(ln->hl, sizeof *ln->hl * (size_t)ln->hlcap);
        }
        ln->state = syntax_highlight_line(b->syn, prev, ln->s, ln->len, ln->hl);
        ln->hl_start = prev;
        ln->match_valid = 1;
        if (b->states_valid == y) b->states_valid = y + 1;
    }
}

/* ---------------- options ---------------- */

void buf_option_changed(Buffer *b, int id) {
    if (id == O_FILETYPE || id == O_SYNTAX) buf_update_rules(b);
}

void buf_set_option(Buffer *b, int id, const OptVal *v, int local) {
    opt_copy(id, &b->opts[id], v);
    if (local) b->local[id] = 1;
    buf_option_changed(b, id);
}
