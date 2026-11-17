/*
 * buffer.c - text storage, file I/O, undo/redo and highlight state.
 */
#include "micro.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_THRESHOLD 1000

void (*cm_buf_edit_hook)(Buffer *b, int type, Loc start, Loc end) = NULL;
Loc (*cm_buf_get_cursor_hook)(Buffer *b, int *num) = NULL;
void (*cm_buf_set_cursor_hook)(Buffer *b, Loc l, int num) = NULL;

int cm_loc_cmp(Loc a, Loc b) {
    if (a.y != b.y) return a.y < b.y ? -1 : 1;
    if (a.x != b.x) return a.x < b.x ? -1 : 1;
    return 0;
}

Loc cm_loc_make(int x, int y) {
    Loc l;
    l.x = x;
    l.y = y;
    return l;
}

/* ---------------- lines ---------------- */

static void line_init(Line *l, const char *s, int len) {
    memset(l, 0, sizeof *l);
    l->cap = len + 1 > 16 ? len + 1 : 16;
    l->s = cm_xmalloc((size_t)l->cap);
    if (len) memcpy(l->s, s, (size_t)len);
    l->len = len;
    l->s[len] = 0;
}

static void line_free(Line *l) {
    free(l->s);
    free(l->hl);
}

static void line_reserve(Line *l, int n) {
    if (n + 1 > l->cap) {
        while (l->cap < n + 1) l->cap *= 2;
        l->s = cm_xrealloc(l->s, (size_t)l->cap);
    }
}

static void lines_insert(Buffer *b, int at, int count) {
    if (b->nlines + count > b->cap) {
        while (b->cap < b->nlines + count) b->cap = b->cap ? b->cap * 2 : 64;
        b->lines = cm_xrealloc(b->lines, sizeof *b->lines * (size_t)b->cap);
    }
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
        b->opts[i].i = cm_gopts[i].i;
        b->opts[i].s = cm_gopts[i].s ? cm_xstrdup(cm_gopts[i].s) : NULL;
        b->local[i] = 0;
    }
}

Buffer *cm_buf_new_empty(void) {
    Buffer *b = cm_xcalloc(1, sizeof *b);
    buf_init_opts(b);
    lines_insert(b, 0, 1);
    line_init(&b->lines[0], "", 0);
    b->path = cm_xstrdup("");
    b->name = cm_xstrdup("");
    b->recording = 1;
    b->is_new = 1;
    b->last_search = NULL;
    return b;
}

void cm_buf_set_text(Buffer *b, const char *text, int len) {
    int i, start = 0, dos = 0, first = 1;
    for (i = 0; i < b->nlines; i++) line_free(&b->lines[i]);
    b->nlines = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        b->bom = 1;
        text += 3;
        len -= 3;
    }
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            int e = i;
            if (e > start && text[e - 1] == '\r') {
                if (first) dos = 1;
                if (dos) e--;
            }
            if (i < len) first = 0;
            lines_insert(b, b->nlines, 1);
            line_init(&b->lines[b->nlines - 1], text + start, e - start);
            start = i + 1;
        }
    }
    if (b->nlines == 0) {
        lines_insert(b, 0, 1);
        line_init(&b->lines[0], "", 0);
    }
    if (!first) {
        /* the file has line breaks: keep its line endings */
        free(b->opts[O_FILEFORMAT].s);
        b->opts[O_FILEFORMAT].s = cm_xstrdup(dos ? "dos" : "unix");
        b->local[O_FILEFORMAT] = 1;
    }
    b->states_valid = 0;
}

Buffer *cm_buf_from_string(const char *name, const char *text, int type) {
    Buffer *b = cm_buf_new_empty();
    cm_buf_set_text(b, text, (int)strlen(text));
    free(b->path);
    free(b->name);
    b->path = cm_xstrdup(name);
    b->name = cm_xstrdup(name);
    b->type = type;
    b->is_new = 0;
    if (type == BT_HELP || type == BT_LOG) {
        b->opts[O_READONLY].i = 1;
        b->local[O_READONLY] = 1;
    }
    cm_buf_update_rules(b);
    return b;
}

void cm_buf_set_name(Buffer *b, const char *path) {
    char *ap;
    free(b->path);
    free(b->name);
    free(b->abspath);
    b->path = cm_xstrdup(path);
    ap = cm_plat_realpath(path);
    if (!ap && *path) {
        /* file may not exist yet: resolve the directory */
        char *dir = cm_path_dir(path);
        char *rd = cm_plat_realpath(dir);
        if (rd) ap = cm_path_join(rd, cm_path_base(path));
        else ap = cm_xstrdup(path);
        free(rd);
        free(dir);
    }
    b->abspath = ap;
    b->name = cm_xstrdup(path);
    if (BOPT(b, O_BASENAME) && *path) {
        free(b->name);
        b->name = cm_xstrdup(cm_path_base(path));
    }
}

Buffer *cm_buf_open(const char *path, char **err) {
    Buffer *b;
    char *data;
    long len = 0;
    char *expanded = cm_expand_home(path);
    if (err) *err = NULL;
    if (cm_plat_is_dir(expanded)) {
        if (err) *err = cm_xsprintf("%s is a directory", expanded);
        free(expanded);
        return NULL;
    }
    b = cm_buf_new_empty();
    cm_buf_set_name(b, expanded);
    data = cm_read_file(expanded, &len);
    if (data) {
        cm_buf_set_text(b, data, (int)len);
        free(data);
        b->is_new = 0;
        b->mtime = cm_plat_mtime(expanded);
        if (!cm_plat_is_writable(expanded)) {
            /* micro marks unwritable files read-only-ish via a message */
        }
    } else if (cm_plat_exists(expanded)) {
        if (err) *err = cm_xsprintf("Error opening %s: permission denied", expanded);
        cm_buf_free(b);
        free(expanded);
        return NULL;
    }
    free(expanded);
    if (cm_backup_check_on_open(b) < 0) {
        /* 'abort': open an empty buffer instead */
        cm_buf_free(b);
        b = cm_buf_new_empty();
    }
    cm_buf_update_rules(b);
    return b;
}

static void evstack_clear(EvStack *s) {
    int i;
    for (i = 0; i < s->n; i++) free(s->ev[i].text);
    s->n = 0;
}

void cm_buf_free(Buffer *b) {
    int i;
    if (!b) return;
    if (b->type == BT_DEFAULT && *b->path) cm_backup_remove(b);
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

int cm_buf_modified(const Buffer *b) {
    if (b->type != BT_DEFAULT) return 0;
    return b->cur_id != b->saved_id;
}

void cm_buf_mark_saved(Buffer *b) { b->saved_id = b->cur_id; }

/* ---------------- text access ---------------- */

Loc cm_buf_end(const Buffer *b) { return cm_loc_make(b->lines[b->nlines - 1].len, b->nlines - 1); }

Loc cm_buf_clamp(const Buffer *b, Loc l) {
    if (l.y < 0) return cm_loc_make(0, 0);
    if (l.y >= b->nlines) return cm_buf_end(b);
    if (l.x < 0) l.x = 0;
    if (l.x > b->lines[l.y].len) l.x = b->lines[l.y].len;
    /* snap to char boundary */
    while (l.x > 0 && l.x < b->lines[l.y].len && ((unsigned char)b->lines[l.y].s[l.x] & 0xC0) == 0x80) l.x--;
    return l;
}

Loc cm_buf_move_chars(const Buffer *b, Loc l, int n) {
    while (n > 0) {
        const Line *ln = &b->lines[l.y];
        if (l.x < ln->len) l.x = cm_utf8_next(ln->s, ln->len, l.x);
        else if (l.y < b->nlines - 1) { l.y++; l.x = 0; }
        else break;
        n--;
    }
    while (n < 0) {
        if (l.x > 0) l.x = cm_utf8_prev(b->lines[l.y].s, l.x);
        else if (l.y > 0) { l.y--; l.x = b->lines[l.y].len; }
        else break;
        n++;
    }
    return l;
}

int cm_buf_char_at(const Buffer *b, Loc l) {
    int cp;
    const Line *ln;
    if (l.y < 0 || l.y >= b->nlines) return -1;
    ln = &b->lines[l.y];
    if (l.x < 0 || l.x >= ln->len) return -1;
    cm_utf8_decode(ln->s + l.x, ln->len - l.x, &cp);
    return cp;
}

char *cm_buf_get_text(const Buffer *b, Loc a, Loc z, int *len) {
    Str s;
    int y;
    cm_str_init(&s);
    cm_str_reserve(&s, 0);
    a = cm_buf_clamp(b, a);
    z = cm_buf_clamp(b, z);
    if (cm_loc_cmp(a, z) > 0) { Loc t = a; a = z; z = t; }
    for (y = a.y; y <= z.y; y++) {
        const Line *ln = &b->lines[y];
        int from = y == a.y ? a.x : 0;
        int to = y == z.y ? z.x : ln->len;
        cm_str_putn(&s, ln->s + from, to - from);
        if (y < z.y) cm_str_putc(&s, '\n');
    }
    if (len) *len = s.len;
    return cm_str_detach(&s);
}

char *cm_buf_full_text(const Buffer *b, int *len) {
    return cm_buf_get_text(b, cm_loc_make(0, 0), cm_buf_end(b), len);
}

const char *cm_buf_leading_ws(const Buffer *b, int y, int *len) {
    const Line *ln = &b->lines[y];
    int i = 0;
    while (i < ln->len && (ln->s[i] == ' ' || ln->s[i] == '\t')) i++;
    *len = i;
    return ln->s;
}

char *cm_buf_indent_string(const Buffer *b) {
    if (BOPT(b, O_TABSTOSPACES)) {
        int n = BOPT(b, O_TABSIZE), i;
        char *s = cm_xmalloc((size_t)n + 1);
        for (i = 0; i < n; i++) s[i] = ' ';
        s[n] = 0;
        return s;
    }
    return cm_xstrdup("\t");
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
    at = cm_buf_clamp(b, at);
    for (i = 0; i < len; i++) if (text[i] == '\n') nl++;
    ln = &b->lines[at.y];
    if (nl == 0) {
        line_reserve(ln, ln->len + len);
        memmove(ln->s + at.x + len, ln->s + at.x, (size_t)(ln->len - at.x));
        memcpy(ln->s + at.x, text, (size_t)len);
        ln->len += len;
        ln->s[ln->len] = 0;
        mark_dirty(b, at.y, at.y);
        return cm_loc_make(at.x + len, at.y);
    }
    /* split */
    tail_len = ln->len - at.x;
    tail = cm_xstrndup(ln->s + at.x, (size_t)tail_len);
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
    a = cm_buf_clamp(b, a);
    z = cm_buf_clamp(b, z);
    if (cm_loc_cmp(a, z) >= 0) return;
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
        s->cap = s->cap ? s->cap * 2 : 64;
        s->ev = cm_xrealloc(s->ev, sizeof *s->ev * (size_t)s->cap);
    }
    s->ev[s->n++] = *e;
}

static void record(Buffer *b, int type, Loc start, Loc end, const char *text, int len) {
    TextEvent e;
    if (!b->recording) return;
    e.type = type;
    e.start = start;
    e.end = end;
    e.text = cm_xstrndup(text, (size_t)len);
    e.len = len;
    e.stamp = cm_plat_now_ms();
    e.id = ++b->next_id;
    e.cnum = 0;
    e.cursor = cm_buf_get_cursor_hook ? cm_buf_get_cursor_hook(b, &e.cnum) : start;
    push_event(&b->undo, &e);
    b->cur_id = e.id;
    evstack_clear(&b->redo);
}

Loc cm_buf_insert(Buffer *b, Loc at, const char *text, int len) {
    Loc end;
    if (len <= 0) return at;
    at = cm_buf_clamp(b, at);
    end = raw_insert(b, at, text, len);
    if (b->recording) record(b, TE_INSERT, at, end, text, len);
    if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_INSERT, at, end);
    return end;
}

void cm_buf_remove(Buffer *b, Loc a, Loc z) {
    char *text;
    int len;
    a = cm_buf_clamp(b, a);
    z = cm_buf_clamp(b, z);
    if (cm_loc_cmp(a, z) > 0) { Loc t = a; a = z; z = t; }
    if (cm_loc_cmp(a, z) == 0) return;
    text = cm_buf_get_text(b, a, z, &len);
    raw_remove(b, a, z);
    if (b->recording) record(b, TE_REMOVE, a, z, text, len);
    free(text);
    if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_REMOVE, a, z);
}

Loc cm_buf_replace(Buffer *b, Loc a, Loc z, const char *text, int len) {
    cm_buf_remove(b, a, z);
    if (cm_loc_cmp(a, z) > 0) a = z;
    return cm_buf_insert(b, a, text, len);
}

/* ---------------- undo / redo ---------------- */

static void apply_inverse(Buffer *b, TextEvent *e) {
    int saved = b->recording;
    b->recording = 0;
    if (e->type == TE_INSERT) {
        raw_remove(b, e->start, e->end);
        if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_REMOVE, e->start, e->end);
    } else {
        Loc end = raw_insert(b, e->start, e->text, e->len);
        if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_INSERT, e->start, end);
    }
    b->recording = saved;
}

static void apply_forward(Buffer *b, TextEvent *e) {
    int saved = b->recording;
    b->recording = 0;
    if (e->type == TE_INSERT) {
        Loc end = raw_insert(b, e->start, e->text, e->len);
        if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_INSERT, e->start, end);
    } else {
        raw_remove(b, e->start, e->end);
        if (cm_buf_edit_hook) cm_buf_edit_hook(b, TE_REMOVE, e->start, e->end);
    }
    b->recording = saved;
}

int cm_buf_undo(Buffer *b) {
    p_s64 start, endt;
    if (b->undo.n == 0) return 0;
    start = b->undo.ev[b->undo.n - 1].stamp;
    endt = start - (start % UNDO_THRESHOLD);
    while (b->undo.n > 0 && b->undo.ev[b->undo.n - 1].stamp >= endt) {
        TextEvent e = b->undo.ev[--b->undo.n];
        apply_inverse(b, &e);
        if (cm_buf_set_cursor_hook) cm_buf_set_cursor_hook(b, e.cursor, e.cnum);
        push_event(&b->redo, &e);
    }
    b->cur_id = b->undo.n ? b->undo.ev[b->undo.n - 1].id : 0;
    return 1;
}

int cm_buf_redo(Buffer *b) {
    p_s64 start, endt;
    if (b->redo.n == 0) return 0;
    start = b->redo.ev[b->redo.n - 1].stamp;
    endt = start - (start % UNDO_THRESHOLD) + UNDO_THRESHOLD;
    while (b->redo.n > 0 && b->redo.ev[b->redo.n - 1].stamp <= endt) {
        TextEvent e = b->redo.ev[--b->redo.n];
        if (cm_buf_set_cursor_hook) cm_buf_set_cursor_hook(b, e.cursor, e.cnum);
        apply_forward(b, &e);
        push_event(&b->undo, &e);
        b->cur_id = e.id;
    }
    return 1;
}

/* ---------------- saving ---------------- */

int cm_buf_save(Buffer *b, const char *path, char **err) {
    FILE *f;
    int y, dos;
    char *expanded;
    if (err) *err = NULL;
    if (!path || !*path) { if (err) *err = cm_xstrdup("No filename"); return -1; }
    expanded = cm_expand_home(path);
    if (BOPT(b, O_RMTRAILINGWS)) {
        for (y = 0; y < b->nlines; y++) {
            Line *ln = &b->lines[y];
            int e = ln->len;
            while (e > 0 && (ln->s[e - 1] == ' ' || ln->s[e - 1] == '\t')) e--;
            if (e < ln->len) cm_buf_remove(b, cm_loc_make(e, y), cm_loc_make(ln->len, y));
        }
    }
    if (BOPT(b, O_EOFNEWLINE) && b->lines[b->nlines - 1].len > 0) {
        raw_insert(b, cm_buf_end(b), "\n", 1); /* not undoable, cursors stay put */
    }
    if (BOPT(b, O_MKPARENTS)) {
        char *dir = cm_path_dir(expanded);
        if (!cm_plat_is_dir(dir)) cm_plat_mkdir_p(dir);
        free(dir);
    }
    {
        char *dir = cm_path_dir(expanded);
        if (!cm_plat_is_dir(dir)) {
            if (err) *err = cm_xsprintf("Parent dirs don't exist, enable 'mkparents' for auto creation");
            free(dir);
            free(expanded);
            return -1;
        }
        free(dir);
    }
    f = fopen(expanded, "wb");
    if (!f) {
        int denied = cm_plat_exists(expanded) ? !cm_plat_is_writable(expanded) : 1;
        if (err) *err = cm_xsprintf("Permission denied: %s", expanded);
        free(expanded);
        return denied ? -2 : -1;
    }
    dos = !strcmp(BOPTS(b, O_FILEFORMAT), "dos");
    if (b->bom) fwrite("\xEF\xBB\xBF", 1, 3, f);
    for (y = 0; y < b->nlines; y++) {
        fwrite(b->lines[y].s, 1, (size_t)b->lines[y].len, f);
        if (y < b->nlines - 1) {
            if (dos) fputc('\r', f);
            fputc('\n', f);
        }
    }
    if (fclose(f) != 0) {
        if (err) *err = cm_xsprintf("Error writing %s", expanded);
        free(expanded);
        return -1;
    }
    {
        int was_unnamed = !*b->path || strcmp(b->path, path) != 0;
        if (was_unnamed) {
            cm_buf_set_name(b, path);
            if (!strcmp(BOPTS(b, O_FILETYPE), "unknown")) cm_buf_update_rules(b);
        }
    }
    b->is_new = 0;
    b->mtime = cm_plat_mtime(expanded);
    free(expanded);
    cm_buf_mark_saved(b);
    cm_backup_remove(b);
    return 0;
}

int cm_buf_reload(Buffer *b, char **err) {
    long len;
    char *data;
    char *expanded = cm_expand_home(b->path);
    if (err) *err = NULL;
    data = cm_read_file(expanded, &len);
    if (!data) {
        if (err) *err = cm_xsprintf("Could not read %s", expanded);
        free(expanded);
        return -1;
    }
    /* replace whole content as one undoable change; cursors are restored
       (clamped) by the caller-visible hook afterwards */
    {
        void (*hook)(Buffer *, int, Loc, Loc) = cm_buf_edit_hook;
        cm_buf_edit_hook = NULL;
        cm_buf_replace(b, cm_loc_make(0, 0), cm_buf_end(b), "", 0);
        cm_buf_edit_hook = hook;
    }
    {
        Buffer *tmp = cm_buf_new_empty();
        char *txt;
        int tl;
        void (*hook)(Buffer *, int, Loc, Loc) = cm_buf_edit_hook;
        cm_buf_set_text(tmp, data, (int)len);
        txt = cm_buf_full_text(tmp, &tl);
        cm_buf_edit_hook = NULL;
        cm_buf_insert(b, cm_loc_make(0, 0), txt, tl);
        cm_buf_edit_hook = hook;
        if (hook) hook(b, TE_REMOVE, cm_buf_end(b), cm_buf_end(b)); /* lets panes clamp */
        free(txt);
        cm_buf_free(tmp);
    }
    free(data);
    b->mtime = cm_plat_mtime(expanded);
    free(expanded);
    cm_buf_mark_saved(b);
    return 0;
}

/* ---------------- syntax ---------------- */

void cm_buf_invalidate_hl(Buffer *b, int from) {
    int y;
    if (from < 0) from = 0;
    if (from < b->states_valid) b->states_valid = from;
    for (y = from; y < b->nlines; y++) b->lines[y].match_valid = 0;
}

void cm_buf_update_rules(Buffer *b) {
    const char *ft = BOPTS(b, O_FILETYPE);
    if (!strcmp(ft, "off")) {
        b->syn = NULL;
    } else if (!strcmp(ft, "unknown") || !*ft) {
        int n = b->nlines < 200 ? b->nlines : 200, i;
        const char **ls = cm_xmalloc(sizeof *ls * (size_t)(n + 1));
        for (i = 0; i < n; i++) ls[i] = b->lines[i].s;
        b->syn = cm_syntax_detect(b->path, b->lines[0].s, ls, n, BOPT(b, O_DETECTLIMIT));
        free(ls);
        if (b->syn && strcmp(b->syn->filetype, "unknown") != 0) {
            free(b->opts[O_FILETYPE].s);
            b->opts[O_FILETYPE].s = cm_xstrdup(b->syn->filetype);
        }
    } else {
        b->syn = cm_syntax_by_filetype(ft);
    }
    if (b->syn) {
        char *err = NULL;
        if (cm_syntax_load(b->syn, &err) != 0) b->syn = NULL;
        if (err) { cm_editor_log("%s", err); free(err); }
    }
    b->states_valid = 0;
    cm_buf_invalidate_hl(b, 0);
}

void cm_buf_ensure_hl(Buffer *b, int y) {
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
            lj->state = cm_syntax_highlight_line(b->syn, st, lj->s, lj->len, NULL);
        }
        b->states_valid++;
    }
    ln = &b->lines[y];
    prev = y > 0 ? b->lines[y - 1].state : NULL;
    if (!ln->match_valid || ln->hl_start != prev) {
        if (ln->hlcap < ln->len + 1) {
            ln->hlcap = ln->len + 16;
            ln->hl = cm_xrealloc(ln->hl, sizeof *ln->hl * (size_t)ln->hlcap);
        }
        ln->state = cm_syntax_highlight_line(b->syn, prev, ln->s, ln->len, ln->hl);
        ln->hl_start = prev;
        ln->match_valid = 1;
        if (b->states_valid == y) b->states_valid = y + 1;
    }
}

/* ---------------- options ---------------- */

void cm_buf_option_changed(Buffer *b, int id) {
    if (id == O_FILETYPE || id == O_SYNTAX) cm_buf_update_rules(b);
}

void cm_buf_set_option(Buffer *b, int id, const OptVal *v, int local) {
    cm_opt_copy(id, &b->opts[id], v);
    if (local) b->local[id] = 1;
    cm_buf_option_changed(b, id);
}
