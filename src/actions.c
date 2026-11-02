/*
 * actions.c - every bindable micro action (CursorUp, Save, Find,
 * SpawnMultiCursor, ...), plus typing, clipboard, search, autocomplete,
 * macros and the built-in "autoclose" and "comment" plugins.
 */
#include "micro.h"
#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int no_relocate;      /* set by actions that only scroll the view */

/* autocomplete state (per editor; tied to the pane that started it) */
static Pane *ac_pane;
static char **ac_sugg;
static int ac_n, ac_idx;
static Loc ac_start;   /* where the completed word starts */
static int ac_inserted; /* bytes of the current suggestion inserted after prefix */
static int ac_prefix;

static void ac_clear(void) {
    int i;
    for (i = 0; i < ac_n; i++) free(ac_sugg[i]);
    free(ac_sugg);
    ac_sugg = NULL;
    ac_n = ac_idx = 0;
    ac_pane = NULL;
}

int autocomplete_suggestions(Pane *p, char ***out, int *idx) {
    if (ac_pane != p || ac_n <= 1) return 0;
    *out = ac_sugg;
    *idx = ac_idx;
    return ac_n;
}

/* ================================================================== */
/* Helpers                                                             */

#define CUR (ACUR(p))
#define BUF (p->buf)

/* pane_merge_cursors() compares every pair of cursors; with thousands of
   cursors (SpawnMultiCursorSelect on a big selection) that dominated every
   keystroke.  Same result (the earliest cursor of each location survives,
   order is kept, the active cursor follows its survivor) in O(n log n). */
static const Cursor *merge_base;
static int merge_cmp(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = loc_cmp(merge_base[ia].loc, merge_base[ib].loc);
    return c ? c : (ia > ib) - (ia < ib);
}

static void merge_cursors(Pane *p) {
    int n = p->ncur, i, k, *idx;
    unsigned char *drop;
    if (n < 32) {
        pane_merge_cursors(p);
        return;
    }
    idx = xmalloc(sizeof *idx * (size_t)n);
    drop = xcalloc((size_t)n, 1);
    for (i = 0; i < n; i++) idx[i] = i;
    merge_base = p->cur;
    qsort(idx, (size_t)n, sizeof *idx, merge_cmp);
    for (i = 1; i < n; i++) {
        if (loc_cmp(p->cur[idx[i]].loc, p->cur[idx[i - 1]].loc) == 0) {
            int keep = idx[i - 1];
            /* idx[i - 1] holds the lowest index of this location (ties sort by index) */
            drop[idx[i]] = 1;
            if (p->active == idx[i]) p->active = keep;
            idx[i] = keep;
        }
    }
    for (i = k = 0; i < n; i++) {
        if (drop[i]) continue;
        if (p->active == i) p->active = k;
        p->cur[k++] = p->cur[i];
    }
    p->ncur = k > 0 ? k : 1;
    if (p->active >= p->ncur) p->active = p->ncur - 1;
    free(idx);
    free(drop);
}

static int readonly_check(Pane *p) {
    if (BOPT(BUF, O_READONLY)) {
        editor_set_message("Warning: file is readonly");
        return 1;
    }
    return 0;
}

static char *sel_text(Pane *p, Cursor *c, int *len) {
    return buf_get_text(p->buf, c->sel[0], c->sel[1], len);
}

static void delete_selection(Pane *p, Cursor *c) {
    Loc a = c->sel[0], z = c->sel[1];
    buf_remove(p->buf, a, z);
    c->loc = c->sel[0];
    c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
}

static void select_to(Pane *p, Cursor *c, Loc l) {
    (void)p;
    cur_set_sel_to(c, l);
}

static void begin_select(Cursor *c) {
    if (!cur_has_sel(c)) {
        c->orig[0] = c->orig[1] = c->loc;
    }
}

static int rune_under(Pane *p, Loc l) { return buf_char_at(p->buf, l); }

static int is_ws(int cp) { return cp == ' ' || cp == '\t'; }

/* micro's RuneUnder: '\n' past the end of the line */
static int ru(Pane *p, Loc l) {
    int r = buf_char_at(p->buf, l);
    return r < 0 ? '\n' : r;
}
static int wsr(int r) { return r == ' ' || r == '\t' || r == '\n'; }
static Loc next_char(Pane *p, Loc l) {
    const Line *ln = &p->buf->lines[l.y];
    return loc_make(l.x < ln->len ? utf8_next(ln->s, ln->len, l.x) : l.x, l.y);
}

/* exact ports of micro's Cursor.WordRight / WordLeft */
static void word_right(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    int len = b->lines[c->loc.y].len;
    if (c->loc.x == len) { c->loc = buf_move_chars(b, c->loc, 1); return; }
    while (wsr(ru(p, c->loc))) {
        if (c->loc.x == b->lines[c->loc.y].len) return;
        c->loc = buf_move_chars(b, c->loc, 1);
    }
    if (!is_word_char(ru(p, c->loc)) && !wsr(ru(p, c->loc)) && !is_word_char(ru(p, next_char(p, c->loc)))) {
        while (!is_word_char(ru(p, c->loc)) && !wsr(ru(p, c->loc))) {
            if (c->loc.x == b->lines[c->loc.y].len) return;
            c->loc = buf_move_chars(b, c->loc, 1);
        }
        return;
    }
    c->loc = buf_move_chars(b, c->loc, 1);
    while (is_word_char(ru(p, c->loc))) {
        if (c->loc.x == b->lines[c->loc.y].len) return;
        c->loc = buf_move_chars(b, c->loc, 1);
    }
}

static int char_before(Pane *p, Loc l) {
    if (l.x == 0) return -1;
    return rune_under(p, loc_make(utf8_prev(p->buf->lines[l.y].s, l.x), l.y));
}

static void word_left(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    if (c->loc.x == 0) { c->loc = buf_move_chars(b, c->loc, -1); return; }
    c->loc = buf_move_chars(b, c->loc, -1);
    while (wsr(ru(p, c->loc))) {
        if (c->loc.x == 0) return;
        c->loc = buf_move_chars(b, c->loc, -1);
    }
    if (!is_word_char(ru(p, c->loc)) && !wsr(ru(p, c->loc)) && c->loc.x > 0 &&
        !is_word_char(char_before(p, c->loc) < 0 ? '\n' : char_before(p, c->loc))) {
        while (!is_word_char(ru(p, c->loc)) && !wsr(ru(p, c->loc))) {
            if (c->loc.x == 0) return;
            c->loc = buf_move_chars(b, c->loc, -1);
        }
        c->loc = buf_move_chars(b, c->loc, 1);
        return;
    }
    if (c->loc.x == 0) return;
    c->loc = buf_move_chars(b, c->loc, -1);
    while (is_word_char(ru(p, c->loc))) {
        if (c->loc.x == 0) return;
        c->loc = buf_move_chars(b, c->loc, -1);
    }
    c->loc = buf_move_chars(b, c->loc, 1);
}

static int is_upper(int cp) { return cp >= 'A' && cp <= 'Z'; }
static int is_lower_or_digit(int cp) { return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9'); }

static void subword_right(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    const Line *ln = &b->lines[c->loc.y];
    int cp;
    if (c->loc.x >= ln->len) { c->loc = buf_move_chars(b, c->loc, 1); return; }
    while ((cp = rune_under(p, c->loc)) >= 0 && (is_ws(cp) || cp == '_')) c->loc.x = utf8_next(ln->s, ln->len, c->loc.x);
    cp = rune_under(p, c->loc);
    if (cp < 0) return;
    if (!is_word_char(cp)) {
        while ((cp = rune_under(p, c->loc)) >= 0 && !is_word_char(cp) && !is_ws(cp)) c->loc.x = utf8_next(ln->s, ln->len, c->loc.x);
        return;
    }
    if (is_upper(cp)) {
        c->loc.x = utf8_next(ln->s, ln->len, c->loc.x);
        if (is_upper(rune_under(p, c->loc))) {
            while ((cp = rune_under(p, c->loc)) >= 0 && is_upper(cp)) {
                Loc nx = loc_make(utf8_next(ln->s, ln->len, c->loc.x), c->loc.y);
                if (is_lower_or_digit(rune_under(p, nx))) return;
                c->loc = nx;
            }
            return;
        }
    }
    while ((cp = rune_under(p, c->loc)) >= 0 && is_word_char(cp) && cp != '_' && !is_upper(cp))
        c->loc.x = utf8_next(ln->s, ln->len, c->loc.x);
}

static void subword_left(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    const Line *ln = &b->lines[c->loc.y];
    int cp;
    if (c->loc.x == 0) { c->loc = buf_move_chars(b, c->loc, -1); return; }
    while ((cp = char_before(p, c->loc)) >= 0 && (is_ws(cp) || cp == '_')) c->loc.x = utf8_prev(ln->s, c->loc.x);
    cp = char_before(p, c->loc);
    if (cp < 0) return;
    if (!is_word_char(cp)) {
        while ((cp = char_before(p, c->loc)) >= 0 && !is_word_char(cp) && !is_ws(cp)) c->loc.x = utf8_prev(ln->s, c->loc.x);
        return;
    }
    if (is_upper(cp)) {
        while ((cp = char_before(p, c->loc)) >= 0 && is_upper(cp)) c->loc.x = utf8_prev(ln->s, c->loc.x);
        return;
    }
    while ((cp = char_before(p, c->loc)) >= 0 && is_word_char(cp) && cp != '_' && !is_upper(cp))
        c->loc.x = utf8_prev(ln->s, c->loc.x);
    if ((cp = char_before(p, c->loc)) >= 0 && is_upper(cp)) c->loc.x = utf8_prev(ln->s, c->loc.x);
}

static int start_of_text_x(Pane *p, int y) {
    int len;
    buf_leading_ws(p->buf, y, &len);
    return len;
}

static void select_word_at(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    const Line *ln = &b->lines[c->loc.y];
    int a = c->loc.x, z = c->loc.x, cp;
    if (ln->len == 0) return;
    cp = rune_under(p, c->loc);
    if (cp < 0 || !is_word_char(cp)) {
        int pc = char_before(p, c->loc);
        if (pc >= 0 && is_word_char(pc)) { a = utf8_prev(ln->s, a); z = a; }
        else {
            /* select single char */
            c->sel[0] = c->loc;
            c->sel[1] = loc_make(utf8_next(ln->s, ln->len, c->loc.x), c->loc.y);
            c->orig[0] = c->sel[0];
            c->orig[1] = c->sel[1];
            c->loc = c->sel[1];
            return;
        }
    }
    while (a > 0 && is_word_char(char_before(p, loc_make(a, c->loc.y)))) a = utf8_prev(ln->s, a);
    while (z < ln->len && is_word_char(rune_under(p, loc_make(z, c->loc.y)))) z = utf8_next(ln->s, ln->len, z);
    c->sel[0] = loc_make(a, c->loc.y);
    c->sel[1] = loc_make(z, c->loc.y);
    c->orig[0] = c->sel[0];
    c->orig[1] = c->sel[1];
    c->loc = c->sel[1];
}

static void select_line_at(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    c->sel[0] = loc_make(0, c->loc.y);
    if (c->loc.y + 1 < b->nlines) c->sel[1] = loc_make(0, c->loc.y + 1);
    else c->sel[1] = loc_make(b->lines[c->loc.y].len, c->loc.y);
    c->orig[0] = c->sel[0];
    c->orig[1] = c->sel[1];
    c->loc = c->sel[1];
}

/* ================================================================== */
/* Clipboard                                                           */

static int ext_tool = -1; /* 0 none, 1 wl-copy, 2 xclip, 3 xsel, 4 pbcopy */

static void detect_clip_tool(void) {
    if (ext_tool >= 0) return;
    ext_tool = 0;
    if (strcmp(plat_null_device(), "/dev/null") != 0) return; /* tools below are POSIX-only */
    if (getenv("WAYLAND_DISPLAY") && plat_system("command -v wl-copy >/dev/null 2>&1") == 0) ext_tool = 1;
    else if (getenv("DISPLAY") && plat_system("command -v xclip >/dev/null 2>&1") == 0) ext_tool = 2;
    else if (getenv("DISPLAY") && plat_system("command -v xsel >/dev/null 2>&1") == 0) ext_tool = 3;
    else if (plat_system("command -v pbcopy >/dev/null 2>&1") == 0) ext_tool = 4;
}

/* 64 unpredictable bits for temp file names: seeded once from the OS
   entropy device where there is one (it is just a file to ISO C), mixed
   with the clock and an address, then stepped with splitmix64. */
static unsigned long long rand64(void) {
    static unsigned long long state;
    static int seeded;
    unsigned long long z;
    if (!seeded) {
        FILE *f = plat_fopen("/dev/urandom", "rb");
        unsigned char rb[8];
        int i;
        state = (unsigned long long)time(NULL) ^ ((unsigned long long)clock() << 32) ^
                (unsigned long long)(size_t)&state ^ (unsigned long long)plat_now_ms();
        if (f) {
            if (fread(rb, 1, sizeof rb, f) == sizeof rb)
                for (i = 0; i < 8; i++) state ^= (unsigned long long)rb[i] << (8 * i);
            fclose(f);
        }
        seeded = 1;
    }
    z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Create a new, empty temporary file and return its malloc'd path (NULL on
   failure).  The name is unpredictable and, on POSIX systems, the file is
   created exclusively (fopen "x" = O_CREAT|O_EXCL), so a file or symlink
   planted in the shared temp directory is never opened, truncated or
   followed (on POSIX inside a private 0700 directory, see temp_dir_for).
   Optionally fills it with `data`.  Used by the clipboard tools
   here and by `run`/`textfilter` in commands.c. */
static char *private_tmp; /* POSIX: a mode 0700 directory of our own */

static void private_tmp_cleanup(void) {
    if (private_tmp) plat_remove(private_tmp); /* empty by now */
    free(private_tmp);
    private_tmp = NULL;
}

/* ISO C can't set file permissions, so on POSIX the files go into a
   directory the shell creates with umask 077 (mkdir fails rather than
   reuse anything already there): other users can't read clipboard or
   filter text while it sits on disk. */
static char *temp_dir_for(int posix) {
    static int tried;
    char *dir = plat_temp_dir();
    if (!posix) return dir;
    if (!tried) {
        char *path, *q, *cmd;
        unsigned long long r = rand64();
        tried = 1;
        path = xsprintf("%s/micro-%08lx%08lx", dir && *dir ? dir : ".",
                        (unsigned long)(r >> 32) & 0xffffffffUL, (unsigned long)r & 0xffffffffUL);
        q = plat_shell_quote(path);
        cmd = q ? xsprintf("umask 077 && mkdir %s 2>/dev/null", q) : NULL;
        if (cmd && plat_system(cmd) == 0) {
            private_tmp = path;
            path = NULL;
            atexit(private_tmp_cleanup);
        }
        free(cmd);
        free(q);
        free(path);
    }
    if (private_tmp && plat_is_dir(private_tmp)) {
        free(dir);
        return xstrdup(private_tmp);
    }
    return dir;
}

char *act_tmpfile_create(const char *tag, const char *data, int len) {
    int posix = !strcmp(plat_null_device(), "/dev/null");
    char *dir = temp_dir_for(posix);
    int tries;
    for (tries = 0; tries < 16; tries++) {
        unsigned long long r1 = rand64(), r2 = rand64();
        char *path = xsprintf("%s%smicro-%s-%08lx%08lx%08lx%08lx", dir && *dir ? dir : ".",
                              dir && *dir && plat_is_sep((unsigned char)dir[strlen(dir) - 1]) ? "" : "/",
                              tag, (unsigned long)(r1 >> 32) & 0xffffffffUL,
                              (unsigned long)r1 & 0xffffffffUL, (unsigned long)(r2 >> 32) & 0xffffffffUL,
                              (unsigned long)r2 & 0xffffffffUL);
        /* Windows: %TEMP% is per user and its C library may not know "x" */
        FILE *f = plat_fopen(path, posix ? "wbx" : "wb");
        if (f) {
            int ok = 1;
            if (data && len > 0 && fwrite(data, 1, (size_t)len, f) != (size_t)len) ok = 0;
            if (fclose(f) != 0) ok = 0;
            if (ok) {
                free(dir);
                return path;
            }
            plat_remove(path);
            free(path);
            break;
        }
        free(path);
    }
    free(dir);
    return NULL;
}

void clipboard_set(const char *s, int len, int lines) {
    free(E.clipboard);
    E.clipboard = xstrndup(s, (size_t)len);
    E.clipboard_len = len;
    E.clipboard_lines = lines;
    if (!strcmp(GOPTS(O_CLIPBOARD), "terminal")) {
        scr_osc52(s, len);
    } else if (!strcmp(GOPTS(O_CLIPBOARD), "external")) {
        if (plat_clipboard_set(s, len) == 0) return; /* native (Windows) */
        detect_clip_tool();
        if (ext_tool > 0) {
            static const char *cmds[] = {"", "wl-copy", "xclip -selection clipboard -i",
                                         "xsel --clipboard --input", "pbcopy"};
            char *tp = act_tmpfile_create("clip", s, len);
            char *q = tp ? plat_shell_quote(tp) : NULL;
            if (q) {
                char *cmd = xsprintf("%s < %s >/dev/null 2>&1", cmds[ext_tool], q); /* POSIX only */
                if (plat_system(cmd) != 0) { /* the internal copy above still works */ }
                free(cmd);
            }
            if (tp) plat_remove(tp);
            free(q);
            free(tp);
        }
    }
}

char *clipboard_get(int *len) {
    if (!strcmp(GOPTS(O_CLIPBOARD), "external")) {
        char *native = plat_clipboard_get(len);
        if (native) {
            if (E.clipboard && (*len != E.clipboard_len || memcmp(native, E.clipboard, (size_t)*len)))
                E.clipboard_lines = 0;
            return native;
        }
        detect_clip_tool();
        if (ext_tool > 0) {
            static const char *cmds[] = {"", "wl-paste --no-newline", "xclip -selection clipboard -o",
                                         "xsel --clipboard --output", "pbpaste"};
            /* the tool writes into a file we created exclusively ourselves */
            char *tp = act_tmpfile_create("clip", NULL, 0);
            char *q = tp ? plat_shell_quote(tp) : NULL;
            char *cmd = q ? xsprintf("%s > %s 2>/dev/null", cmds[ext_tool], q) : NULL;
            int rc = cmd ? plat_system(cmd) : -1;
            long l = 0;
            char *data = rc == 0 ? read_file(tp, &l) : NULL;
            free(cmd);
            free(q);
            if (tp) plat_remove(tp);
            free(tp);
            if (data) {
                *len = (int)l;
                if (E.clipboard && (l != E.clipboard_len || memcmp(data, E.clipboard, (size_t)l)))
                    E.clipboard_lines = 0;
                return data;
            }
        }
    }
    if (!E.clipboard) { *len = 0; return xstrdup(""); }
    *len = E.clipboard_len;
    return xstrndup(E.clipboard, (size_t)E.clipboard_len);
}

/* ================================================================== */
/* Search                                                              */

static Regex *search_cache;
static char *search_cache_pat;
static int search_cache_flags;
static int search_cache_lit; /* pattern is a plain string: 1 exact, 2 ASCII case-insensitive */
static char *search_cache_fold; /* lowercased pattern for search_cache_lit == 2 */

static const char *mem_find(const char *hay, size_t hl, const char *needle, size_t nl);

static Regex *get_search_re(Buffer *b, const char *pat, int regex, char **err) {
    int flags = (BOPT(b, O_IGNORECASE) ? RE_ICASE : 0) | (regex ? 0 : RE_LITERAL);
    if (err) *err = NULL;
    if (search_cache && search_cache_pat && !strcmp(search_cache_pat, pat) && search_cache_flags == flags)
        return search_cache;
    re_free(search_cache);
    free(search_cache_pat);
    search_cache = re_compile(pat, flags, err);
    search_cache_pat = search_cache ? xstrdup(pat) : NULL;
    search_cache_flags = flags;
    /* searching a big file line by line is dominated by regex start-up per
       line; a plain string is found with memchr/memcmp instead */
    search_cache_lit = 0;
    free(search_cache_fold);
    search_cache_fold = NULL;
    if (search_cache && *pat && ((flags & RE_LITERAL) || !strpbrk(pat, "\\.+*?()|[]{}^$"))) {
        search_cache_lit = flags & RE_ICASE ? 2 : 1;
        if (search_cache_lit == 2) {
            /* the regex engine folds ASCII letters only; so do we */
            char *q;
            search_cache_fold = xstrdup(pat);
            for (q = search_cache_fold; *q; q++)
                if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
        }
    }
    return search_cache;
}

/* re_search() for the cached search pattern, with the plain-string fast path */
static int search_in(const Regex *re, const char *s, int len, int start, int *caps) {
    if (re == search_cache && search_cache_lit == 1) {
        size_t nl = strlen(search_cache_pat);
        const char *f;
        if (start < 0 || start > len) return 0;
        f = mem_find(s + start, (size_t)(len - start), search_cache_pat, nl);
        if (!f) return 0;
        caps[0] = (int)(f - s);
        caps[1] = caps[0] + (int)nl;
        return 1;
    }
    if (re == search_cache && search_cache_lit == 2) {
        const char *nd = search_cache_fold;
        int nl = (int)strlen(nd), i, j;
        unsigned char c0 = (unsigned char)nd[0], c0u = c0 >= 'a' && c0 <= 'z' ? (unsigned char)(c0 - 'a' + 'A') : c0;
        if (start < 0) return 0;
        for (i = start; i <= len - nl; i++) {
            unsigned char h = (unsigned char)s[i];
            if (h != c0 && h != c0u) continue;
            for (j = 1; j < nl; j++) {
                unsigned char a = (unsigned char)s[i + j];
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
                if (a != (unsigned char)nd[j]) break;
            }
            if (j == nl) {
                caps[0] = i;
                caps[1] = i + nl;
                return 1;
            }
        }
        return 0;
    }
    return re_search(re, s, len, start, caps, 1);
}

static int line_find(Regex *re, const Line *ln, int from, int *ms, int *me) {
    int caps[2];
    if (from > ln->len) return 0;
    if (!search_in(re, ln->s, ln->len, from, caps)) return 0;
    *ms = caps[0];
    *me = caps[1];
    return 1;
}

/* last match in line with start < limit (limit = -1: anywhere) */
static int line_find_last(Regex *re, const Line *ln, int limit, int *ms, int *me) {
    int pos = 0, found = 0, caps[2];
    while (pos <= ln->len && search_in(re, ln->s, ln->len, pos, caps)) {
        if (limit >= 0 && caps[0] >= limit) break;
        *ms = caps[0];
        *me = caps[1];
        found = 1;
        pos = caps[1] > caps[0] ? caps[1] : (caps[1] < ln->len ? utf8_next(ln->s, ln->len, caps[1]) : ln->len + 1);
    }
    return found;
}

int search_find(Pane *p, const char *pat, int regex, Loc from, int down, Loc *ms, Loc *me, char **err) {
    Buffer *b = p->buf;
    Regex *re = get_search_re(b, pat, regex, err);
    int y, i, s, e;
    if (!re) return 0;
    if (down) {
        for (i = 0; i <= b->nlines; i++) {
            y = (from.y + i) % b->nlines;
            if (i == 0) {
                if (line_find(re, &b->lines[y], from.x, &s, &e)) {
                    /* skip an empty match at the starting point */
                    if (s == e && s == from.x && line_find(re, &b->lines[y], s < b->lines[y].len ? utf8_next(b->lines[y].s, b->lines[y].len, s) : s + 1, &s, &e)) {}
                    else if (s == e && s == from.x) goto next;
                    *ms = loc_make(s, y);
                    *me = loc_make(e, y);
                    return 1;
                }
            } else if (i == b->nlines) {
                if (line_find(re, &b->lines[y], 0, &s, &e) && s < from.x) {
                    *ms = loc_make(s, y);
                    *me = loc_make(e, y);
                    return 1;
                }
            } else if (line_find(re, &b->lines[y], 0, &s, &e)) {
                *ms = loc_make(s, y);
                *me = loc_make(e, y);
                return 1;
            }
        next:;
        }
    } else {
        for (i = 0; i <= b->nlines; i++) {
            y = ((from.y - i) % b->nlines + b->nlines) % b->nlines;
            if (i == 0) {
                if (line_find_last(re, &b->lines[y], from.x, &s, &e)) {
                    *ms = loc_make(s, y);
                    *me = loc_make(e, y);
                    return 1;
                }
            } else if (i == b->nlines) {
                if (line_find_last(re, &b->lines[y], -1, &s, &e) && s >= from.x) {
                    *ms = loc_make(s, y);
                    *me = loc_make(e, y);
                    return 1;
                }
            } else if (line_find_last(re, &b->lines[y], -1, &s, &e)) {
                *ms = loc_make(s, y);
                *me = loc_make(e, y);
                return 1;
            }
        }
    }
    return 0;
}

void search_highlight_ranges(Pane *p, int y, unsigned char *mask, int len) {
    Buffer *b = p->buf;
    Regex *re;
    const Line *ln;
    int pos = 0, caps[2];
    if (!b->last_search || y < 0 || y >= b->nlines) return;
    re = get_search_re(b, b->last_search, b->last_search_regex, NULL);
    ln = &b->lines[y];
    if (!re) return;
    if (len > ln->len) len = ln->len;
    while (pos <= ln->len && search_in(re, ln->s, ln->len, pos, caps)) {
        if (caps[1] > caps[0]) {
            int a = caps[0] < 0 ? 0 : caps[0], z = caps[1] > len ? len : caps[1];
            if (z > a) memset(mask + a, 1, (size_t)(z - a));
            pos = caps[1];
        } else {
            pos = caps[1] < ln->len ? utf8_next(ln->s, ln->len, caps[1]) : ln->len + 1;
        }
    }
}

static int do_find(Pane *p, int down) {
    Buffer *b = p->buf;
    Cursor *c = CUR;
    Loc from, ms, me;
    char *err = NULL;
    if (!b->last_search || !*b->last_search) return 0;
    if (down) from = cur_has_sel(c) ? c->sel[1] : c->loc;
    else from = cur_has_sel(c) ? c->sel[0] : c->loc;
    if (search_find(p, b->last_search, b->last_search_regex, from, down, &ms, &me, &err)) {
        c->sel[0] = c->orig[0] = ms;
        c->sel[1] = c->orig[1] = me;
        c->loc = me;
        cur_store_vx(p, c);
        b->hl_search = 1;
    } else if (err) {
        editor_set_error("%s", err);
        free(err);
    } else {
        editor_set_message("No matches found");
    }
    return 1;
}

/* ================================================================== */
/* Insertion                                                           */

static int autoclose_open(int r) { return r == '"' || r == '\'' || r == '`' || r == '(' || r == '{' || r == '['; }
static int autoclose_close_of(int r) {
    switch (r) {
    case '(': return ')';
    case '{': return '}';
    case '[': return ']';
    default: return r;
    }
}
static int autoclose_is_close(int r) { return r == '"' || r == '\'' || r == '`' || r == ')' || r == '}' || r == ']'; }

static void insert_rune_at_cursor(Pane *p, Cursor *c, int r) {
    Buffer *b = p->buf;
    char buf[4];
    int n = utf8_encode(r, buf);
    if (cur_has_sel(c)) delete_selection(p, c);
    else if (p->overwrite && c->loc.x < b->lines[c->loc.y].len) {
        Loc nx = buf_move_chars(b, c->loc, 1);
        buf_remove(b, c->loc, nx);
    }
    /* autoclose plugin: typing a closing char that's already there skips over it */
    if (BOPT(b, O_AUTOCLOSE) && !GOPT(O_PASTE) && autoclose_is_close(r) && rune_under(p, c->loc) == r) {
        int prev = char_before(p, c->loc);
        if (!(r == '"' || r == '\'' || r == '`') || prev >= 0) {
            c->loc = buf_move_chars(b, c->loc, 1);
            cur_store_vx(p, c);
            return;
        }
    }
    buf_insert(b, c->loc, buf, n);
    if (BOPT(b, O_AUTOCLOSE) && !GOPT(O_PASTE) && autoclose_open(r)) {
        int next = rune_under(p, c->loc);
        int prev;
        Loc before = buf_move_chars(b, c->loc, -1);
        prev = char_before(p, before);
        if ((r == '"' || r == '\'' || r == '`') && prev >= 0 && (is_word_char(prev) || prev == r)) {
            /* don't autoclose quotes right after a word char */
        } else if (next < 0 || !is_word_char(next)) {
            int cl = autoclose_close_of(r);
            Loc save = c->loc;
            n = utf8_encode(cl, buf);
            buf_insert(b, c->loc, buf, n);
            c->loc = save;
        }
    }
    cur_store_vx(p, c);
}

int insert_rune(Pane *p, int r) {
    int i, saved = p->active;
    ac_clear();
    if (readonly_check(p)) return 0;
    for (i = 0; i < p->ncur; i++) {
        p->active = i;
        insert_rune_at_cursor(p, &p->cur[i], r);
    }
    p->active = saved;
    merge_cursors(p);
    macro_record(1, NULL, 0, r);
    return 1;
}

void insert_text(Pane *p, const char *s, int len, int paste) {
    int i, saved = p->active;
    ac_clear();
    if (readonly_check(p)) return;
    for (i = 0; i < p->ncur; i++) {
        Cursor *c = &p->cur[i];
        p->active = i;
        if (cur_has_sel(c)) delete_selection(p, c);
        buf_insert(p->buf, c->loc, s, len);
        cur_store_vx(p, c);
    }
    p->active = saved;
    if (paste) macro_record(2, s, len, 0);
}

/* ================================================================== */
/* Macros                                                              */

void macro_record(int kind, const char *s, int len, int rune) {
    MacroItem *m;
    if (!E.recording_macro || E.playing_macro) return;
    if (E.nmacro == E.cmacro) {
        E.cmacro = E.cmacro ? E.cmacro * 2 : 32;
        E.macro = xrealloc(E.macro, sizeof *E.macro * (size_t)E.cmacro);
    }
    m = &E.macro[E.nmacro++];
    m->kind = kind;
    m->s = s ? xstrndup(s, (size_t)len) : NULL;
    m->len = len;
    m->rune = rune;
}

/* ================================================================== */
/* Brace matching                                                      */

int find_matching_brace(Buffer *b, Loc cur, int left, Loc *brace, Loc *match) {
    static const char *pairs[] = {"()", "{}", "[]"};
    int k, cand;
    for (cand = 0; cand < 2; cand++) {
        Loc at = cur;
        int r;
        if (cand == 1) {
            if (!left || cur.x == 0) break;
            at.x = utf8_prev(b->lines[cur.y].s, cur.x);
        }
        r = buf_char_at(b, at);
        if (r < 0) continue;
        for (k = 0; k < 3; k++) {
            int open = pairs[k][0], close = pairs[k][1], depth = 0;
            if (r == open) {
                Loc l = at;
                int y = l.y, x = l.x;
                for (; y < b->nlines; y++, x = 0) {
                    const Line *ln = &b->lines[y];
                    for (; x < ln->len; x++) {
                        if (ln->s[x] == open) depth++;
                        else if (ln->s[x] == close) {
                            if (--depth == 0) {
                                *brace = at;
                                *match = loc_make(x, y);
                                return 1;
                            }
                        }
                    }
                    if (y - at.y > 5000) return 0;
                }
                return 0;
            } else if (r == close) {
                int y = at.y, x = at.x;
                for (; y >= 0; y--) {
                    const Line *ln = &b->lines[y];
                    if (y != at.y) x = ln->len - 1;
                    for (; x >= 0; x--) {
                        if (ln->s[x] == close) depth++;
                        else if (ln->s[x] == open) {
                            if (--depth == 0) {
                                *brace = at;
                                *match = loc_make(x, y);
                                return 1;
                            }
                        }
                    }
                    if (at.y - y > 5000) return 0;
                }
                return 0;
            }
        }
    }
    return 0;
}

/* ================================================================== */
/* Actions                                                             */

static int a_none(Pane *p) { (void)p; return 1; }

static int a_cursor_up(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    cur_up(p, c, 1);
    return 1;
}
static int a_cursor_down(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 0);
    cur_down(p, c, 1);
    return 1;
}
static int a_cursor_left(Pane *p) {
    Cursor *c = CUR;
    if (cur_has_sel(c)) { cur_deselect(c, 1); cur_store_vx(p, c); }
    else cur_left(p, c);
    return 1;
}
static int a_cursor_right(Pane *p) {
    Cursor *c = CUR;
    if (cur_has_sel(c)) { cur_deselect(c, 0); cur_store_vx(p, c); }
    else cur_right(p, c);
    return 1;
}
static int page_size(Pane *p) {
    int h = pane_text_height(p) - BOPT(p->buf, O_PAGEOVERLAP);
    return h < 1 ? 1 : h;
}
static int a_cursor_page_up(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    pane_scroll(p, -page_size(p));
    cur_up(p, c, page_size(p));
    return 1;
}
static int a_cursor_page_down(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 0);
    pane_scroll(p, page_size(p));
    cur_down(p, c, page_size(p));
    return 1;
}
static int a_cursor_start(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    c->loc = loc_make(0, 0);
    cur_store_vx(p, c);
    return 1;
}
static int a_cursor_end(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    c->loc = buf_end(p->buf);
    cur_store_vx(p, c);
    return 1;
}
static int view_row_loc(Pane *p, int r, Loc *out) {
    return pane_screen_to_loc(p, p->x + pane_gutter_width(p), p->y + r, out);
}
static int a_cursor_to_view_top(Pane *p) {
    Cursor *c = CUR;
    int m = BOPT(p->buf, O_SCROLLMARGIN);
    cur_deselect(c, 1);
    view_row_loc(p, p->top == 0 ? 0 : m, &c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_cursor_to_view_center(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    view_row_loc(p, pane_text_height(p) / 2, &c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_cursor_to_view_bottom(Pane *p) {
    Cursor *c = CUR;
    int m = BOPT(p->buf, O_SCROLLMARGIN);
    cur_deselect(c, 1);
    view_row_loc(p, pane_text_height(p) - 1 - m, &c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_start(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    c->loc = loc_make(0, 0);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_end(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    c->loc = buf_end(p->buf);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_up(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    cur_up(p, c, 1);
    select_to(p, c, c->loc);
    return 1;
}
static int a_select_down(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    cur_down(p, c, 1);
    select_to(p, c, c->loc);
    return 1;
}
static int a_select_left(Pane *p) {
    Cursor *c = CUR;
    Loc end = buf_end(p->buf);
    begin_select(c);
    if (loc_cmp(c->loc, end) > 0) c->loc = end;
    cur_left(p, c);
    select_to(p, c, c->loc);
    return 1;
}
static int a_select_right(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    cur_right(p, c);
    select_to(p, c, c->loc);
    return 1;
}
static int a_word_right(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 0);
    word_right(p, c);
    cur_store_vx(p, c);
    return 1;
}
static int a_word_left(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    word_left(p, c);
    cur_store_vx(p, c);
    return 1;
}
static int a_subword_right(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 0);
    subword_right(p, c);
    cur_store_vx(p, c);
    return 1;
}
static int a_subword_left(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    subword_left(p, c);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_word_right(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    word_right(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_word_left(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    word_left(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_subword_right(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    subword_right(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_subword_left(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    subword_left(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int delete_sel_if(Pane *p) {
    Cursor *c = CUR;
    if (cur_has_sel(c)) {
        if (readonly_check(p)) return 0;
        delete_selection(p, c);
        cur_store_vx(p, c);
    }
    return 1;
}
static int a_delete_word_right(Pane *p) { a_select_word_right(p); return delete_sel_if(p); }
static int a_delete_word_left(Pane *p) { a_select_word_left(p); return delete_sel_if(p); }
static int a_delete_subword_right(Pane *p) { a_select_subword_right(p); return delete_sel_if(p); }
static int a_delete_subword_left(Pane *p) { a_select_subword_left(p); return delete_sel_if(p); }

static int a_select_line(Pane *p) {
    select_line_at(p, CUR);
    cur_store_vx(p, CUR);
    return 1;
}
static int a_select_to_start_of_line(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    c->loc.x = 0;
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_start_of_text(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    c->loc.x = start_of_text_x(p, c->loc.y);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_start_of_text_toggle(Pane *p) {
    Cursor *c = CUR;
    int st = start_of_text_x(p, c->loc.y);
    begin_select(c);
    c->loc.x = c->loc.x == st ? 0 : st;
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_end_of_line(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    c->loc.x = p->buf->lines[c->loc.y].len;
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int is_blank_line(Buffer *b, int y) {
    const Line *ln = &b->lines[y];
    int i;
    for (i = 0; i < ln->len; i++) if (!is_ws((unsigned char)ln->s[i])) return 0;
    return 1;
}
static void paragraph_prev(Pane *p, Cursor *c) {
    int y = c->loc.y;
    while (y > 0 && is_blank_line(p->buf, y)) y--;
    while (y > 0 && !is_blank_line(p->buf, y)) y--;
    c->loc = loc_make(0, y);
}
static void paragraph_next(Pane *p, Cursor *c) {
    Buffer *b = p->buf;
    int y = c->loc.y;
    while (y < b->nlines - 1 && is_blank_line(b, y)) y++;
    while (y < b->nlines - 1 && !is_blank_line(b, y)) y++;
    c->loc = loc_make(y == b->nlines - 1 ? b->lines[y].len : 0, y);
}
static int a_paragraph_previous(Pane *p) { cur_deselect(CUR, 1); paragraph_prev(p, CUR); cur_store_vx(p, CUR); return 1; }
static int a_paragraph_next(Pane *p) { cur_deselect(CUR, 0); paragraph_next(p, CUR); cur_store_vx(p, CUR); return 1; }
static int a_select_to_paragraph_previous(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    paragraph_prev(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}
static int a_select_to_paragraph_next(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    paragraph_next(p, c);
    select_to(p, c, c->loc);
    cur_store_vx(p, c);
    return 1;
}

static int a_insert_newline(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    int wslen, cx;
    const char *ln;
    char *ws;
    if (readonly_check(p)) return 0;
    if (cur_has_sel(c)) delete_selection(p, c);
    /* autoclose: "{|}" + Enter -> block */
    if (BOPT(b, O_AUTOCLOSE) && !GOPT(O_PASTE) && c->loc.x > 0) {
        int cr = char_before(p, c->loc), nr = rune_under(p, c->loc);
        if ((cr == '(' && nr == ')') || (cr == '{' && nr == '}') || (cr == '[' && nr == ']')) {
            char *ind = buf_indent_string(b);
            int l;
            const char *s = buf_leading_ws(b, c->loc.y, &l);
            char *wsv = xstrndup(s, (size_t)l);
            char *txt = xsprintf("\n%s%s\n%s", wsv, ind, wsv);
            Loc start = c->loc;
            buf_insert(b, c->loc, txt, (int)strlen(txt));
            c->loc = loc_make(l + (int)strlen(ind), start.y + 1);
            free(txt);
            free(wsv);
            free(ind);
            cur_store_vx(p, c);
            return 1;
        }
    }
    ln = buf_leading_ws(b, c->loc.y, &wslen);
    ws = xstrndup(ln, (size_t)wslen);
    cx = c->loc.x;
    buf_insert(b, c->loc, "\n", 1);
    if (BOPT(b, O_AUTOINDENT) && !GOPT(O_PASTE)) {
        if (cx < wslen) ws[cx] = 0;
        buf_insert(b, c->loc, ws, (int)strlen(ws));
        if (is_blank_line(b, c->loc.y - 1) && b->lines[c->loc.y - 1].len > 0 && !BOPT(b, O_KEEPAUTOINDENT)) {
            int y = c->loc.y - 1;
            buf_remove(b, loc_make(0, y), loc_make(b->lines[y].len, y));
        }
    }
    free(ws);
    cur_store_vx(p, c);
    return 1;
}

static int a_backspace(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    if (readonly_check(p)) return 0;
    if (cur_has_sel(c)) {
        delete_selection(p, c);
    } else if (c->loc.x > 0 || c->loc.y > 0) {
        int ts = BOPT(b, O_TABSIZE) > 0 ? BOPT(b, O_TABSIZE) : 1, i, allsp = 1;
        const Line *ln = &b->lines[c->loc.y];
        Loc start;
        for (i = 0; i < c->loc.x; i++) if (ln->s[i] != ' ') { allsp = 0; break; }
        /* autoclose: delete an empty pair "(|)" */
        if (BOPT(b, O_AUTOCLOSE) && !GOPT(O_PASTE) && c->loc.x > 0) {
            int cr = char_before(p, c->loc), nr = rune_under(p, c->loc);
            if (nr >= 0 && autoclose_open(cr) && autoclose_close_of(cr) == nr) {
                Loc nx = buf_move_chars(b, c->loc, 1);
                buf_remove(b, c->loc, nx);
            }
        }
        if (BOPT(b, O_TABSTOSPACES) && allsp && c->loc.x > 0 && c->loc.x % ts == 0)
            start = loc_make(c->loc.x - ts, c->loc.y);
        else
            start = buf_move_chars(b, c->loc, -1);
        buf_remove(b, start, c->loc);
    }
    cur_store_vx(p, c);
    return 1;
}

static int a_delete(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    if (readonly_check(p)) return 0;
    if (cur_has_sel(c)) delete_selection(p, c);
    else if (loc_cmp(c->loc, buf_end(b)) < 0) buf_remove(b, c->loc, buf_move_chars(b, c->loc, 1));
    return 1;
}

static int a_insert_tab(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    char *ind;
    int tb, until;
    if (readonly_check(p)) return 0;
    if (cur_has_sel(c)) delete_selection(p, c);
    ind = buf_indent_string(b);
    tb = (int)strlen(ind);
    if (tb < 1) { free(ind); ind = xstrdup(" "); tb = 1; } /* tabsize 0 */
    until = tb - (pane_visual_x(p, c->loc.y, c->loc.x) % tb);
    buf_insert(b, c->loc, ind, until);
    free(ind);
    cur_store_vx(p, c);
    return 1;
}

static int sel_line_range(Pane *p, Cursor *c, int *y0, int *y1) {
    if (!cur_has_sel(c)) {
        *y0 = *y1 = c->loc.y;
        return 0;
    }
    *y0 = c->sel[0].y;
    *y1 = c->sel[1].y;
    if (c->sel[1].x == 0 && *y1 > *y0) (*y1)--;
    (void)p;
    return 1;
}

static void indent_lines(Pane *p, int y0, int y1) {
    Buffer *b = BUF;
    char *ind = buf_indent_string(b);
    int y;
    for (y = y0; y <= y1; y++)
        if (b->lines[y].len > 0 || y0 == y1) buf_insert(b, loc_make(0, y), ind, (int)strlen(ind));
    free(ind);
}

/* micro: remove up to len(IndentString) leading whitespace characters */
static void outdent_line(Pane *p, int y) {
    Buffer *b = BUF;
    const Line *ln = &b->lines[y];
    char *ind = buf_indent_string(b);
    int want = (int)strlen(ind), n = 0;
    free(ind);
    while (n < want && n < ln->len && (ln->s[n] == ' ' || ln->s[n] == '\t')) n++;
    if (n) buf_remove(b, loc_make(0, y), loc_make(n, y));
}

static int a_indent_selection(Pane *p) {
    Cursor *c = CUR;
    int y0, y1;
    Loc s0, s1;
    if (!cur_has_sel(c)) return 0;
    if (readonly_check(p)) return 0;
    sel_line_range(p, c, &y0, &y1);
    s0 = c->sel[0];
    s1 = c->sel[1];
    indent_lines(p, y0, y1);
    /* keep whole lines selected */
    {
        char *ind = buf_indent_string(BUF);
        int il = (int)strlen(ind);
        free(ind);
        c->sel[0] = loc_make(s0.x > 0 ? s0.x + il : 0, s0.y);
        c->sel[1] = loc_make(s1.x > 0 || s1.y == y1 ? s1.x + il : 0, s1.y);
        if (s1.x == 0 && s1.y > y1) c->sel[1] = loc_make(0, s1.y);
        c->sel[0] = buf_clamp(BUF, c->sel[0]);
        c->sel[1] = buf_clamp(BUF, c->sel[1]);
        c->orig[0] = c->sel[0];
        c->orig[1] = c->sel[1];
        c->loc = c->sel[1];
    }
    return 1;
}

static int a_outdent_selection(Pane *p) {
    Cursor *c = CUR;
    int y0, y1, y;
    Loc s0, s1;
    if (!cur_has_sel(c)) return 0;
    if (readonly_check(p)) return 0;
    sel_line_range(p, c, &y0, &y1);
    s0 = c->sel[0];
    s1 = c->sel[1];
    (void)s0;
    (void)s1;
    /* the edit hook shifts the selection along with the text */
    for (y = y0; y <= y1; y++) outdent_line(p, y);
    c->orig[0] = c->sel[0];
    c->orig[1] = c->sel[1];
    c->loc = c->sel[1];
    return 1;
}

static int a_outdent_line(Pane *p) {
    if (cur_has_sel(CUR)) return 0;
    if (readonly_check(p)) return 0;
    outdent_line(p, CUR->loc.y);
    cur_store_vx(p, CUR);
    return 1;
}

static int a_indent_line(Pane *p) {
    if (cur_has_sel(CUR)) return 0;
    if (readonly_check(p)) return 0;
    indent_lines(p, CUR->loc.y, CUR->loc.y);
    cur_store_vx(p, CUR);
    return 1;
}

/* is the text the last completion inserted still there, under the cursor?
   (anything else, e.g. a reload from disk, makes the saved offsets stale) */
static int ac_state_valid(Pane *p) {
    Buffer *b = BUF;
    const Line *ln;
    const char *w;
    int n;
    if (ac_pane != p || ac_n <= 0 || ac_idx < 0 || ac_idx >= ac_n) return 0;
    if (ac_start.y < 0 || ac_start.y >= b->nlines || ac_start.x < 0) return 0;
    ln = &b->lines[ac_start.y];
    w = ac_sugg[ac_idx];
    n = ac_prefix + ac_inserted;
    if (n < 0 || (int)strlen(w) != n || ac_start.x > ln->len - n) return 0;
    if (memcmp(ln->s + ac_start.x, w, (size_t)n) != 0) return 0;
    return loc_cmp(CUR->loc, loc_make(ac_start.x + n, ac_start.y)) == 0;
}

static void ac_switch(Pane *p, int dir) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    Loc a = loc_make(ac_start.x + ac_prefix, ac_start.y);
    const char *w;
    buf_remove(b, a, loc_make(a.x + ac_inserted, a.y));
    ac_idx = (ac_idx + dir + ac_n) % ac_n;
    w = ac_sugg[ac_idx];
    ac_inserted = (int)strlen(w) - ac_prefix;
    buf_insert(b, a, w + ac_prefix, ac_inserted);
    c->loc = loc_make(a.x + ac_inserted, a.y);
    cur_store_vx(p, c);
}

/* autocomplete from words in the buffer (micro's BufferComplete) */
static int a_autocomplete(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    int x, start, y, i;
    size_t plen;
    char *prefix;
    if (cur_has_sel(c)) return 0;
    if (ac_pane == p && ac_n > 0) {
        if (ac_state_valid(p)) {
            if (readonly_check(p)) return 0;
            ac_switch(p, 1); /* cycle forward */
            return 1;
        }
        ac_clear();
    }
    if (BOPT(b, O_READONLY)) return 0; /* let Tab fall through to the next action */
    x = c->loc.x;
    if (x == 0) return 0;
    {
        int r = rune_under(p, c->loc), pr = char_before(p, c->loc);
        if (!is_word_char(pr) || (r >= 0 && is_word_char(r))) return 0;
    }
    start = x;
    while (start > 0 && is_word_char(char_before(p, loc_make(start, c->loc.y))))
        start = utf8_prev(b->lines[c->loc.y].s, start);
    prefix = xstrndup(b->lines[c->loc.y].s + start, (size_t)(x - start));
    plen = (size_t)(x - start);
    ac_clear();
    /* scan the buffer from the cursor outwards so nearer words come first */
    for (i = 0; i < b->nlines; i++) {
        int k;
        for (k = 0; k < 2; k++) {
            const Line *ln;
            int pos = 0;
            y = k == 0 ? c->loc.y - i : c->loc.y + i;
            if (y < 0 || y >= b->nlines || (k == 1 && i == 0)) continue;
            ln = &b->lines[y];
            while (pos < ln->len) {
                int cp, n = utf8_decode(ln->s + pos, ln->len - pos, &cp);
                if (is_word_char(cp) && (pos == 0 || !is_word_char(char_before(p, loc_make(pos, y))))) {
                    int e = pos, j, dup = 0;
                    while (e < ln->len) {
                        int c2, n2 = utf8_decode(ln->s + e, ln->len - e, &c2);
                        if (!is_word_char(c2)) break;
                        e += n2;
                    }
                    if ((size_t)(e - pos) > plen && !memcmp(ln->s + pos, prefix, plen) &&
                        !(y == c->loc.y && pos == start)) {
                        char *w = xstrndup(ln->s + pos, (size_t)(e - pos));
                        for (j = 0; j < ac_n; j++) if (!strcmp(ac_sugg[j], w)) { dup = 1; break; }
                        if (!dup) {
                            ac_sugg = xrealloc(ac_sugg, sizeof *ac_sugg * (size_t)(ac_n + 1));
                            ac_sugg[ac_n++] = w;
                        } else free(w);
                    }
                    pos = e;
                    continue;
                }
                pos += n;
            }
        }
        if (ac_n > 50) break;
    }
    if (ac_n == 0) {
        free(prefix);
        return 0;
    }
    ac_pane = p;
    ac_idx = 0;
    ac_start = loc_make(start, c->loc.y);
    ac_prefix = (int)strlen(prefix);
    ac_inserted = (int)strlen(ac_sugg[0]) - ac_prefix;
    buf_insert(b, c->loc, ac_sugg[0] + ac_prefix, ac_inserted);
    free(prefix);
    cur_store_vx(p, c);
    return 1;
}

static int a_cycle_autocomplete_back(Pane *p) {
    if (ac_pane != p || ac_n == 0) return 0;
    if (!ac_state_valid(p)) { ac_clear(); return 0; }
    if (readonly_check(p)) return 0;
    ac_switch(p, -1);
    return 1;
}

/* ---- file ---- */

typedef struct {
    Pane *p;
    int then_quit;
} SaveCtx;

static void quit_after_save(Pane *p);
static int a_quit(Pane *p);

typedef struct {
    Pane *p;
    char *path;
    int then_quit;
} SudoCtx;

static void sudo_yn(int yes, int canceled, void *data) {
    SudoCtx *s = data;
    char *err = NULL;
    if (yes && !canceled) {
        if (buf_save_sudo(s->p->buf, s->path, &err) == 0) {
            editor_set_message("Saved %s", s->path);
            if (s->then_quit) quit_after_save(s->p);
        } else {
            editor_set_error("%s", err);
            free(err);
        }
    }
    free(s->path);
    free(s);
}

/* returns 1 if saved now; on permission errors offers sudo (micro's autosu) */
static int save_to(Pane *p, const char *path, int then_quit) {
    char *err = NULL;
    int rc = buf_save(p->buf, path, &err);
    if (rc == 0) {
        editor_set_message("Saved %s", path);
        return 1;
    }
    if (rc == -2) {
        SudoCtx *s = xmalloc(sizeof *s);
        s->p = p;
        s->path = xstrdup(path);
        s->then_quit = then_quit;
        free(err);
        if (!plat_has_sudo()) {
            editor_set_error("Permission denied. Save with sudo not supported on Windows");
            free(s->path);
            free(s);
            return 0;
        }
        if (BOPT(p->buf, O_AUTOSU)) sudo_yn(1, 0, s);
        else {
            char *msg = xsprintf("Permission denied. Do you want to save this file using %s? (y,n)", GOPTS(O_SUCMD));
            prompt_yn(msg, sudo_yn, s);
            free(msg);
        }
        return 0;
    }
    editor_set_error("%s", err);
    free(err);
    return 0;
}

static void saveas_done(const char *resp, int canceled, void *data) {
    SaveCtx *ctx = data;
    Pane *p = ctx->p;
    char *err = NULL;
    (void)err;
    if (!canceled && resp && *resp) {
        if (save_to(p, resp, ctx->then_quit) && ctx->then_quit) quit_after_save(p);
    }
    free(ctx);
}

int save_pane(Pane *p, int then_quit) {
    Buffer *b = BUF;
    char *err = NULL;
    if (b->type != BT_DEFAULT) return 0;
    if (BOPT(b, O_READONLY)) {
        editor_set_message("Cannot save readonly buffer");
        return 0;
    }
    if (!*b->path) {
        SaveCtx *ctx = xmalloc(sizeof *ctx);
        ctx->p = p;
        ctx->then_quit = then_quit;
        prompt_text("Filename: ", "", HIST_OPEN, saveas_done, NULL, ctx);
        return 0;
    }
    (void)err;
    return save_to(p, b->path, then_quit);
}

static int a_save(Pane *p) {
    if (!buf_modified(BUF) && *BUF->path && !BUF->is_new) {
        /* micro saves anyway; keep it cheap but honest */
    }
    save_pane(p, 0);
    return 1;
}

static int a_save_as(Pane *p) {
    SaveCtx *ctx = xmalloc(sizeof *ctx);
    ctx->p = p;
    ctx->then_quit = 0;
    prompt_text("Filename: ", "", HIST_OPEN, saveas_done, NULL, ctx);
    return 1;
}

static void save_all_fn(Pane *p, void *data) {
    (void)data;
    if (buf_modified(p->buf) && *p->buf->path) save_pane(p, 0);
}
static int a_save_all(Pane *p) {
    (void)p;
    editor_all_panes(save_all_fn, NULL);
    return 1;
}

static void open_done(const char *resp, int canceled, void *data) {
    Pane *p = data;
    if (canceled || !resp) return;
    command_run(p, resp);
}
static int a_open_file(Pane *p) {
    prompt_text("> ", "open ", HIST_COMMAND, open_done, NULL, p);
    return 1;
}

/* ---- find ---- */

typedef struct {
    Pane *p;
    int regex;
    Loc origin;
    int top, toprow, left;
    Cursor saved;
} FindCtx;

static void find_changed(const char *resp, void *data) {
    FindCtx *f = data;
    Pane *p = f->p;
    Cursor *c = CUR;
    Loc ms, me;
    if (!BOPT(p->buf, O_INCSEARCH)) return;
    if (!*resp) {
        *c = f->saved;
        p->top = f->top;
        p->toprow = f->toprow;
        return;
    }
    if (search_find(p, resp, f->regex, f->origin, 1, &ms, &me, NULL)) {
        c->sel[0] = c->orig[0] = ms;
        c->sel[1] = c->orig[1] = me;
        c->loc = me;
        pane_relocate(p);
    } else {
        *c = f->saved;
        c->sel[0] = c->sel[1] = c->loc;
    }
}

static void find_done(const char *resp, int canceled, void *data) {
    FindCtx *f = data;
    Pane *p = f->p;
    Cursor *c = CUR;
    if (canceled || !resp || !*resp) {
        *c = f->saved;
        p->top = f->top;
        p->toprow = f->toprow;
        p->left = f->left;
        free(f);
        return;
    }
    free(p->buf->last_search);
    p->buf->last_search = xstrdup(resp);
    p->buf->last_search_regex = f->regex;
    p->buf->hl_search = 1;
    {
        Loc ms, me;
        char *err = NULL;
        if (search_find(p, resp, f->regex, f->origin, 1, &ms, &me, &err)) {
            c->sel[0] = c->orig[0] = ms;
            c->sel[1] = c->orig[1] = me;
            c->loc = me;
            cur_store_vx(p, c);
            pane_relocate(p);
        } else if (err) {
            *c = f->saved;
            editor_set_error("%s", err);
            free(err);
        } else {
            *c = f->saved;
            editor_set_message("No matches found");
        }
    }
    free(f);
}

static int start_find(Pane *p, int regex) {
    FindCtx *f = xcalloc(1, sizeof *f);
    Cursor *c = CUR;
    f->p = p;
    f->regex = regex;
    f->saved = *c;
    f->origin = cur_has_sel(c) ? c->sel[0] : c->loc;
    f->top = p->top;
    f->toprow = p->toprow;
    f->left = p->left;
    prompt_text(regex ? "Find (regex): " : "Find (literal): ", "", HIST_FIND, find_done, find_changed, f);
    return 1;
}
static int a_find(Pane *p) { return start_find(p, 1); }
static int a_find_literal(Pane *p) { return start_find(p, 0); }
static int a_find_next(Pane *p) { return do_find(p, 1); }
static int a_find_previous(Pane *p) { return do_find(p, 0); }
static int a_diff_next(Pane *p) { (void)p; return 0; }
static int a_diff_previous(Pane *p) { (void)p; return 0; }

static int a_center(Pane *p) {
    pane_center(p);
    no_relocate = 1;
    return 1;
}

static int a_undo(Pane *p) {
    if (buf_undo(BUF)) editor_set_message("Undid action");
    merge_cursors(p);
    cur_store_vx(p, CUR);
    return 1;
}
static int a_redo(Pane *p) {
    if (buf_redo(BUF)) editor_set_message("Redid action");
    merge_cursors(p);
    cur_store_vx(p, CUR);
    return 1;
}

/* ---- clipboard actions ---- */

static int a_copy(Pane *p) {
    Cursor *c = CUR;
    char *t;
    int len;
    if (!cur_has_sel(c)) return 0;
    t = sel_text(p, c, &len);
    clipboard_set(t, len, 0);
    free(t);
    E.cut_line_streak = 0;
    editor_set_message("Copied selection");
    return 1;
}

/* micro's selectLines(): extend the selection to whole lines */
static int select_lines(Pane *p, Cursor *c) {
    Buffer *b = BUF;
    int n;
    if (cur_has_sel(c)) {
        Loc s0 = c->sel[0], e = c->sel[1];
        if (e.x == 0) e = buf_move_chars(b, e, -1);
        c->sel[0] = loc_make(0, s0.y);
        c->sel[1] = e.y + 1 < b->nlines ? loc_make(0, e.y + 1) : buf_end(b);
        c->orig[0] = c->sel[0];
        c->orig[1] = c->sel[1];
        c->loc = c->sel[1];
    } else {
        select_line_at(p, c);
    }
    n = c->sel[1].y - c->sel[0].y;
    if (n == 0 && cur_has_sel(c)) n = 1;
    return n;
}

static int a_copy_line(Pane *p) {
    Cursor *c = CUR;
    Cursor save = *c;
    char *t;
    int len, n = select_lines(p, c);
    if (n == 0) { *c = save; return 0; }
    t = sel_text(p, c, &len);
    clipboard_set(t, len, 1);
    free(t);
    *c = save;
    E.cut_line_streak = 0;
    if (n > 1) editor_set_message("Copied %d lines", n);
    else editor_set_message("Copied line");
    return 1;
}

static int a_cut(Pane *p) {
    Cursor *c = CUR;
    char *t;
    int len;
    if (!cur_has_sel(c)) return 0;
    if (readonly_check(p)) return 0;
    t = sel_text(p, c, &len);
    clipboard_set(t, len, 0);
    free(t);
    delete_selection(p, c);
    cur_store_vx(p, c);
    E.cut_line_streak = 0;
    editor_set_message("Cut selection");
    return 1;
}

static int a_cut_line(Pane *p) {
    Cursor *c = CUR;
    char *t;
    int len, n, total;
    if (readonly_check(p)) return 0;
    n = select_lines(p, c);
    if (n == 0) return 0;
    total = n;
    t = sel_text(p, c, &len);
    if (E.cut_line_streak && E.clipboard) {
        char *cat = xmalloc((size_t)(E.clipboard_len + len + 1));
        int k;
        memcpy(cat, E.clipboard, (size_t)E.clipboard_len);
        memcpy(cat + E.clipboard_len, t, (size_t)len + 1);
        for (k = 0; k < E.clipboard_len; k++) if (E.clipboard[k] == '\n') total++;
        clipboard_set(cat, E.clipboard_len + len, 1);
        free(cat);
    } else {
        clipboard_set(t, len, 1);
    }
    free(t);
    E.cut_line_streak = 1;
    delete_selection(p, c);
    cur_store_vx(p, c);
    if (total > 1) editor_set_message("Cut %d lines", total);
    else editor_set_message("Cut line");
    return 1;
}

/* first occurrence of needle in hay (both may contain NUL bytes) */
static const char *mem_find(const char *hay, size_t hl, const char *needle, size_t nl) {
    const char *p = hay, *end = hay + hl;
    if (nl == 0) return hay;
    while ((size_t)(end - p) >= nl) {
        p = memchr(p, needle[0], (size_t)(end - p) - nl + 1);
        if (!p) return NULL;
        if (!memcmp(p, needle, nl)) return p;
        p++;
    }
    return NULL;
}

/* micro's smartpaste: re-indent pasted lines to the current line's
   indentation.  Returns malloc'd text (or NULL to paste `clip` as is). */
static char *smart_paste(Pane *p, Cursor *c, const char *clip, int len, int *outlen) {
    Buffer *b = BUF;
    int pl = 0, wl;
    char *needle;
    const char *lws, *s, *end = clip + len, *f;
    Str o;
    if (!BOPT(b, O_SMARTPASTE) || c->loc.x <= 0 || !memchr(clip, '\n', (size_t)len)) return NULL;
    while (pl < len && (clip[pl] == ' ' || clip[pl] == '\t')) pl++;
    if (pl == 1 && clip[0] == ' ') return NULL;
    needle = xmalloc((size_t)pl + 1);
    needle[0] = '\n';
    memcpy(needle + 1, clip, (size_t)pl);
    if (!mem_find(clip, (size_t)len, needle, (size_t)pl + 1)) {
        free(needle);
        return NULL;
    }
    lws = buf_leading_ws(b, c->loc.y, &wl);
    str_init(&o);
    str_reserve(&o, len);
    for (s = clip + pl; (f = mem_find(s, (size_t)(end - s), needle, (size_t)pl + 1)) != NULL;
         s = f + pl + 1) {
        str_putn(&o, s, (int)(f - s));
        str_putc(&o, '\n');
        str_putn(&o, lws, wl);
    }
    str_putn(&o, s, (int)(end - s));
    free(needle);
    *outlen = o.len;
    return str_detach(&o);
}

static void paste_text(Pane *p, Cursor *c, const char *clip, int len) {
    Buffer *b = BUF;
    int tl = len;
    char *txt = smart_paste(p, c, clip, len, &tl);
    if (cur_has_sel(c)) delete_selection(p, c);
    buf_insert(b, c->loc, txt ? txt : clip, txt ? tl : len);
    free(txt);
    cur_store_vx(p, c);
    E.cut_line_streak = 0;
}

static int a_paste(Pane *p) {
    int len;
    char *clip;
    if (readonly_check(p)) return 0;
    clip = clipboard_get(&len);
    if (p->ncur > 1) {
        /* if the clipboard has exactly one line per cursor, distribute them */
        int nl = 1, i;
        for (i = 0; i < len; i++) if (clip[i] == '\n') nl++;
        if (len > 0 && clip[len - 1] == '\n') nl--;
        if (nl == p->ncur) {
            const char *s = clip, *end = clip + len, *e;
            int k;
            for (k = 0; k < p->active && s < end; k++) {
                e = memchr(s, '\n', (size_t)(end - s));
                s = e ? e + 1 : end;
            }
            e = memchr(s, '\n', (size_t)(end - s));
            paste_text(p, CUR, s, e ? (int)(e - s) : (int)(end - s));
            free(clip);
            editor_set_message("Pasted clipboard");
            return 1;
        }
    }
    paste_text(p, CUR, clip, len);
    free(clip);
    editor_set_message("Pasted clipboard");
    return 1;
}

static int a_paste_primary(Pane *p) { return a_paste(p); }

static int a_duplicate(Pane *p) {
    Cursor *c = CUR;
    char *t;
    int len;
    if (!cur_has_sel(c)) return 0;
    if (readonly_check(p)) return 0;
    t = sel_text(p, c, &len);
    buf_insert(BUF, c->sel[1], t, len);
    free(t);
    editor_set_message("Duplicated selection");
    return 1;
}

static int a_duplicate_line(Pane *p) {
    Cursor *c = CUR;
    Buffer *b = BUF;
    if (readonly_check(p)) return 0;
    if (cur_has_sel(c)) {
        Cursor save = *c;
        Loc s0 = c->sel[0], e = c->sel[1];
        char *t, *nt;
        int len;
        if (e.x == 0) e = buf_move_chars(b, e, -1);
        /* one insertion of "\n" + lines s0.y..e.y after line e.y (inserting
           line by line would shift the rest of the buffer once per line) */
        t = buf_get_text(b, loc_make(0, s0.y), loc_make(b->lines[e.y].len, e.y), &len);
        nt = xmalloc((size_t)len + 2);
        nt[0] = '\n';
        memcpy(nt + 1, t, (size_t)len + 1);
        free(t);
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
        c->loc = loc_make(b->lines[e.y].len, e.y);
        buf_insert(b, c->loc, nt, len + 1);
        free(nt);
        *c = save;
        if (s0.y < e.y) editor_set_message("Duplicated %d lines", e.y - s0.y + 1);
        else editor_set_message("Duplicated line");
        return 1;
    }
    {
        const Line *ln = &b->lines[c->loc.y];
        int len = ln->len;
        char *t = xmalloc((size_t)len + 1);
        t[0] = '\n';
        memcpy(t + 1, ln->s, (size_t)len);
        c->loc.x = len;
        buf_insert(b, c->loc, t, len + 1);
        free(t);
    }
    cur_store_vx(p, c);
    editor_set_message("Duplicated line");
    return 1;
}

static int a_delete_line(Pane *p) {
    Cursor *c = CUR;
    int n;
    if (readonly_check(p)) return 0;
    n = select_lines(p, c);
    if (n == 0) return 0;
    delete_selection(p, c);
    cur_store_vx(p, c);
    if (n > 1) editor_set_message("Deleted %d lines", n);
    else editor_set_message("Deleted line");
    return 1;
}

static int move_lines(Pane *p, int up) {
    Cursor *c = CUR;
    Buffer *b = BUF;
    Cursor save;
    int y0, y1, len, d;
    char *t;
    if (readonly_check(p)) return 0;
    sel_line_range(p, c, &y0, &y1);
    if (up ? y0 == 0 : y1 >= b->nlines - 1) {
        editor_set_message(up ? "Cannot move further up" : "Cannot move further down");
        return 1;
    }
    save = *c;
    /* the moved line is copied with its length: it may contain NUL bytes */
    if (up) {
        const Line *ln = &b->lines[y0 - 1];
        len = ln->len;
        t = xmalloc((size_t)len + 1);
        t[0] = '\n';
        memcpy(t + 1, ln->s, (size_t)len);
        buf_remove(b, loc_make(0, y0 - 1), loc_make(0, y0));
        buf_insert(b, loc_make(b->lines[y1 - 1].len, y1 - 1), t, len + 1);
        d = -1;
    } else {
        const Line *ln = &b->lines[y1 + 1];
        len = ln->len;
        t = xmalloc((size_t)len + 1);
        memcpy(t, ln->s, (size_t)len);
        t[len] = '\n';
        buf_remove(b, loc_make(b->lines[y1].len, y1), loc_make(b->lines[y1 + 1].len, y1 + 1));
        buf_insert(b, loc_make(0, y0), t, len + 1);
        d = 1;
    }
    free(t);
    *c = save;
    c->loc = buf_clamp(b, loc_make(c->loc.x, c->loc.y + d));
    c->sel[0] = buf_clamp(b, loc_make(c->sel[0].x, c->sel[0].y + d));
    c->sel[1] = buf_clamp(b, loc_make(c->sel[1].x, c->sel[1].y + d));
    c->orig[0] = buf_clamp(b, loc_make(c->orig[0].x, c->orig[0].y + d));
    c->orig[1] = buf_clamp(b, loc_make(c->orig[1].x, c->orig[1].y + d));
    return 1;
}
static int a_move_lines_up(Pane *p) { return move_lines(p, 1); }
static int a_move_lines_down(Pane *p) { return move_lines(p, 0); }

static int a_select_all(Pane *p) {
    Cursor *c = CUR;
    c->sel[0] = c->orig[0] = loc_make(0, 0);
    c->sel[1] = c->orig[1] = buf_end(BUF);
    c->loc = c->sel[1];
    cur_store_vx(p, c);
    return 1;
}

static int a_start(Pane *p) { p->top = 0; p->toprow = 0; no_relocate = 1; return 1; }
static int a_end(Pane *p) {
    pane_scroll(p, BUF->nlines * 4 + 1000);
    no_relocate = 1;
    return 1;
}
static int a_page_up(Pane *p) { pane_scroll(p, -page_size(p)); no_relocate = 1; return 1; }
static int a_page_down(Pane *p) { pane_scroll(p, page_size(p)); no_relocate = 1; return 1; }
static int a_half_page_up(Pane *p) { pane_scroll(p, -pane_text_height(p) / 2); no_relocate = 1; return 1; }
static int a_half_page_down(Pane *p) { pane_scroll(p, pane_text_height(p) / 2); no_relocate = 1; return 1; }
static int a_select_page_up(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    cur_up(p, c, page_size(p));
    select_to(p, c, c->loc);
    return 1;
}
static int a_select_page_down(Pane *p) {
    Cursor *c = CUR;
    begin_select(c);
    cur_down(p, c, page_size(p));
    select_to(p, c, c->loc);
    return 1;
}
static int a_start_of_text(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    c->loc.x = start_of_text_x(p, c->loc.y);
    cur_store_vx(p, c);
    return 1;
}
static int a_start_of_text_toggle(Pane *p) {
    Cursor *c = CUR;
    int st;
    cur_deselect(c, 1);
    st = start_of_text_x(p, c->loc.y);
    c->loc.x = c->loc.x == st ? 0 : st;
    cur_store_vx(p, c);
    return 1;
}
static int a_start_of_line(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    c->loc.x = 0;
    cur_store_vx(p, c);
    return 1;
}
static int a_end_of_line(Pane *p) {
    Cursor *c = CUR;
    cur_deselect(c, 1);
    c->loc.x = BUF->lines[c->loc.y].len;
    cur_store_vx(p, c);
    return 1;
}

static int a_toggle_help(Pane *p) {
    if (p->buf->type == BT_HELP) editor_quit_pane(p, 1);
    else help_open(p, "help");
    return 1;
}
static int a_toggle_keymenu(Pane *p) {
    (void)p;
    GOPT(O_KEYMENU) = !GOPT(O_KEYMENU);
    editor_layout();
    return 1;
}
static int a_toggle_diff_gutter(Pane *p) {
    BOPT(BUF, O_DIFFGUTTER) = !BOPT(BUF, O_DIFFGUTTER);
    editor_set_message(BOPT(BUF, O_DIFFGUTTER) ? "Enabled diff gutter" : "Disabled diff gutter");
    return 1;
}
static int a_toggle_ruler(Pane *p) {
    BOPT(BUF, O_RULER) = !BOPT(BUF, O_RULER);
    BUF->local[O_RULER] = 1;
    editor_set_message(BOPT(BUF, O_RULER) ? "Enabled ruler" : "Disabled ruler");
    return 1;
}
static int a_toggle_highlight_search(Pane *p) {
    BOPT(BUF, O_HLSEARCH) = !BOPT(BUF, O_HLSEARCH);
    BUF->local[O_HLSEARCH] = 1;
    if (BOPT(BUF, O_HLSEARCH)) BUF->hl_search = 1;
    return 1;
}
static int a_unhighlight_search(Pane *p) {
    BUF->hl_search = 0;
    return 1;
}
static int a_reset_search(Pane *p) {
    free(BUF->last_search);
    BUF->last_search = NULL;
    BUF->hl_search = 0;
    return 1;
}
static int a_clear_status(Pane *p) { (void)p; editor_clear_message(); return 1; }

void run_shell_interactive(const char *cmd);

static void shellmode_done(const char *resp, int canceled, void *data) {
    (void)data;
    if (canceled || !resp || !*resp) return;
    run_shell_interactive(resp);
}

static int a_shell_mode(Pane *p) {
    (void)p;
    prompt_text("$ ", "", HIST_SHELL, shellmode_done, NULL, NULL);
    return 1;
}

static void command_done(const char *resp, int canceled, void *data) {
    (void)data;
    if (canceled || !resp) return;
    command_run(cur_pane(), resp);
}
static int a_command_mode(Pane *p) {
    (void)p;
    prompt_text("> ", "", HIST_COMMAND, command_done, NULL, NULL);
    return 1;
}
static int a_toggle_overwrite(Pane *p) { p->overwrite = !p->overwrite; return 1; }
static int a_escape(Pane *p) { (void)p; ac_clear(); return 1; }

static void quit_after_save(Pane *p) { editor_quit_pane(p, 1); }

static void quit_yn(int yes, int canceled, void *data) {
    Pane *p = data;
    if (canceled) return;
    if (yes) {
        if (save_pane(p, 1)) editor_quit_pane(p, 1);
    } else {
        editor_quit_pane(p, 1);
    }
}

static int a_quit(Pane *p) {
    Buffer *b = BUF;
    if (buf_modified(b) && b->refs <= 1) {
        char *msg = xsprintf("Save changes to %s before closing? (y,n,esc)",
                             *b->name ? b->name : "No name");
        prompt_yn(msg, quit_yn, p);
        free(msg);
        return 1;
    }
    editor_quit_pane(p, 1);
    return 1;
}

static void any_modified_fn(Pane *p, void *data) {
    if (buf_modified(p->buf)) *(int *)data = 1;
}
static void quitall_yn(int yes, int canceled, void *data) {
    (void)data;
    if (!canceled && yes) E.quit = 1;
}
static int a_quit_all(Pane *p) {
    int any = 0;
    (void)p;
    editor_all_panes(any_modified_fn, &any);
    if (any) prompt_yn("Quit micro? (all open buffers will be closed without saving) (y,n,esc)", quitall_yn, NULL);
    else E.quit = 1;
    return 1;
}
static int a_force_quit(Pane *p) { editor_quit_pane(p, 1); return 1; }

static int a_add_tab(Pane *p) {
    (void)p;
    editor_add_tab(pane_new(buf_new_empty()));
    return 1;
}
static int a_previous_tab(Pane *p) {
    (void)p;
    if (E.curtab == 0) return 0;
    E.curtab--;
    editor_layout();
    return 1;
}
static int a_next_tab(Pane *p) {
    (void)p;
    if (E.curtab >= E.ntabs - 1) return 0;
    E.curtab++;
    editor_layout();
    return 1;
}
static int a_first_tab(Pane *p) { (void)p; E.curtab = 0; editor_layout(); return 1; }
static int a_last_tab(Pane *p) { (void)p; E.curtab = E.ntabs - 1; editor_layout(); return 1; }

static int split_index(Tab *t, Pane *p, Pane ***arr, int *n) {
    int i;
    *arr = tab_panes(t, n);
    for (i = 0; i < *n; i++) if ((*arr)[i] == p) return i;
    return 0;
}
static int a_next_split(Pane *p) {
    Tab *t = cur_tab();
    Pane **a;
    int n, i = split_index(t, p, &a, &n);
    if (i >= n - 1) { free(a); return 0; }
    t->active = a[i + 1];
    free(a);
    return 1;
}
static int a_previous_split(Pane *p) {
    Tab *t = cur_tab();
    Pane **a;
    int n, i = split_index(t, p, &a, &n);
    if (i == 0) { free(a); return 0; }
    t->active = a[i - 1];
    free(a);
    return 1;
}
static int a_first_split(Pane *p) {
    Tab *t = cur_tab();
    Pane **a;
    int n;
    split_index(t, p, &a, &n);
    t->active = a[0];
    free(a);
    return 1;
}
static int a_last_split(Pane *p) {
    Tab *t = cur_tab();
    Pane **a;
    int n;
    split_index(t, p, &a, &n);
    t->active = a[n - 1];
    free(a);
    return 1;
}
static int a_unsplit(Pane *p) {
    if (tab_npanes(cur_tab()) <= 1) return 0;
    return a_quit(p);
}
static int a_vsplit(Pane *p) {
    tab_split(cur_tab(), p, buf_new_empty(), 1);
    editor_layout();
    return 1;
}
static int a_hsplit(Pane *p) {
    tab_split(cur_tab(), p, buf_new_empty(), 0);
    editor_layout();
    return 1;
}

static int a_toggle_macro(Pane *p) {
    int i;
    (void)p;
    E.recording_macro = !E.recording_macro;
    if (E.recording_macro) {
        for (i = 0; i < E.nmacro; i++) free(E.macro[i].s);
        E.nmacro = 0;
        editor_set_message("Recording");
    } else {
        editor_set_message("Stopped recording");
    }
    return 1;
}

static int a_play_macro(Pane *p) {
    int i;
    if (E.recording_macro) return 0;
    E.playing_macro = 1;
    for (i = 0; i < E.nmacro; i++) {
        MacroItem *m = &E.macro[i];
        Pane *cp = cur_pane();
        if (prompt_active()) break; /* never run recorded actions under a prompt */
        if (m->kind == 0) run_action_string(cp, m->s, NULL);
        else if (m->kind == 1) insert_rune(cp, m->rune);
        else insert_text(cp, m->s, m->len, 1);
    }
    E.playing_macro = 0;
    (void)p;
    return 1;
}

static int a_suspend(Pane *p) {
    (void)p;
    if (!plat_can_suspend()) {
        editor_set_message("Suspend is not supported on this platform");
        return 0;
    }
    scr_fini();
    plat_suspend();
    scr_init();
    scr_set_mouse(GOPT(O_MOUSE));
    scr_invalidate();
    return 1;
}

static int a_scroll_up(Pane *p) {
    pane_scroll(p, -BOPT(BUF, O_SCROLLSPEED));
    no_relocate = 1;
    return 1;
}
static int a_scroll_down(Pane *p) {
    pane_scroll(p, BOPT(BUF, O_SCROLLSPEED));
    no_relocate = 1;
    return 1;
}

/* ---- multiple cursors ---- */

static int a_spawn_multi_cursor(Pane *p) {
    Cursor *sp = &p->cur[p->ncur - 1];
    char *sel, *q, *pat;
    int len;
    Loc ms, me;
    char *err = NULL;
    if (!cur_has_sel(sp)) {
        select_word_at(p, sp);
        p->multi_word = 1;
        return 1;
    }
    sel = sel_text(p, sp, &len);
    q = re_quote(sel);
    pat = p->multi_word ? xsprintf("\\b%s\\b", q) : xstrdup(q);
    if (search_find(p, pat, 1, sp->sel[1], 1, &ms, &me, &err)) {
        int i = pane_add_cursor(p, me);
        Cursor *c = &p->cur[i];
        c->sel[0] = c->orig[0] = ms;
        c->sel[1] = c->orig[1] = me;
        c->loc = me;
        p->active = i;
        merge_cursors(p);
    } else {
        editor_set_message("No matches found");
    }
    free(err);
    free(sel);
    free(q);
    free(pat);
    return 1;
}

static int a_spawn_multi_cursor_select(Pane *p) {
    Cursor *c = CUR;
    int y0, y1, y;
    if (!cur_has_sel(c)) return 0;
    y0 = c->sel[0].y;
    y1 = c->sel[1].y;
    if (y0 == y1) return 0;
    pane_reset_cursors(p);
    p->cur[0].loc = loc_make(0, y0);
    cur_deselect(&p->cur[0], 1);
    p->cur[0].loc = loc_make(0, y0);
    p->cur[0].sel[0] = p->cur[0].sel[1] = p->cur[0].orig[0] = p->cur[0].orig[1] = p->cur[0].loc;
    for (y = y0 + 1; y <= y1; y++) pane_add_cursor(p, loc_make(0, y));
    p->active = p->ncur - 1;
    editor_set_message("Added cursors from selection");
    return 1;
}

static int spawn_vertical(Pane *p, int dir) {
    Cursor *last = &p->cur[p->ncur - 1];
    Cursor tmp = *last;
    int i;
    if (dir < 0) {
        if (tmp.loc.y == 0) return 0;
        cur_up(p, &tmp, 1);
    } else {
        if (tmp.loc.y >= p->buf->nlines - 1) return 0;
        cur_down(p, &tmp, 1);
    }
    i = pane_add_cursor(p, tmp.loc); /* may realloc p->cur: don't touch `last` after */
    p->cur[i].lastvx = tmp.lastvx;
    p->active = i;
    merge_cursors(p);
    return 1;
}
static int a_spawn_multi_cursor_up(Pane *p) { return spawn_vertical(p, -1); }
static int a_spawn_multi_cursor_down(Pane *p) { return spawn_vertical(p, 1); }

static int a_remove_multi_cursor(Pane *p) {
    if (p->ncur > 1) pane_remove_cursor(p, p->ncur - 1);
    else p->multi_word = 0;
    p->active = p->ncur - 1;
    return 1;
}
static int a_remove_all_multi_cursors(Pane *p) {
    pane_reset_cursors(p);
    p->multi_word = 0;
    return 1;
}
static int a_skip_multi_cursor(Pane *p) {
    Cursor *last = &p->cur[p->ncur - 1];
    char *sel, *q, *pat;
    int len;
    Loc ms, me;
    if (!cur_has_sel(last)) return 1;
    sel = sel_text(p, last, &len);
    q = re_quote(sel);
    pat = p->multi_word ? xsprintf("\\b%s\\b", q) : xstrdup(q);
    if (search_find(p, pat, 1, last->sel[1], 1, &ms, &me, NULL)) {
        last->sel[0] = last->orig[0] = ms;
        last->sel[1] = last->orig[1] = me;
        last->loc = me;
        merge_cursors(p);
    } else editor_set_message("No matches found");
    free(sel);
    free(q);
    free(pat);
    return 1;
}
static int a_skip_multi_cursor_back(Pane *p) {
    Cursor *last = &p->cur[p->ncur - 1];
    char *sel, *q, *pat;
    int len;
    Loc ms, me;
    if (!cur_has_sel(last)) return 1;
    sel = sel_text(p, last, &len);
    q = re_quote(sel);
    pat = p->multi_word ? xsprintf("\\b%s\\b", q) : xstrdup(q);
    if (search_find(p, pat, 1, last->sel[0], 0, &ms, &me, NULL)) {
        last->sel[0] = last->orig[0] = ms;
        last->sel[1] = last->orig[1] = me;
        last->loc = me;
        merge_cursors(p);
    } else editor_set_message("No matches found");
    free(sel);
    free(q);
    free(pat);
    return 1;
}

static int a_jump_to_matching_brace(Pane *p) {
    Loc br, m;
    Cursor *c = CUR;
    if (!find_matching_brace(BUF, c->loc, 1, &br, &m)) return 0;
    /* brace under the cursor: jump past its match; brace left of it: onto the match */
    if (loc_cmp(br, c->loc) == 0) c->loc = buf_move_chars(BUF, m, 1);
    else c->loc = m;
    c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    cur_store_vx(p, c);
    return 1;
}

static void jumpline_done(const char *resp, int canceled, void *data) {
    (void)data;
    if (canceled || !resp) return;
    {
        char *cmd = xsprintf("goto %s", resp);
        command_run(cur_pane(), cmd);
        free(cmd);
    }
}
static int a_jump_line(Pane *p) {
    (void)p;
    prompt_text("Jump to line:col: ", "", HIST_GOTO, jumpline_done, NULL, NULL);
    return 1;
}
static int a_deselect(Pane *p) {
    int i;
    for (i = 0; i < p->ncur; i++) {
        Cursor *c = &p->cur[i];
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    return 1;
}
static int a_clear_info(Pane *p) { (void)p; editor_clear_message(); return 1; }

/* ---- mouse ---- */

static Pane *mouse_pane(Pane *p) {
    InEvent *ev = E.cur_event;
    Pane *mp;
    if (!ev) return p;
    mp = tab_pane_at(cur_tab(), ev->mx, ev->my);
    if (mp && mp != p) cur_tab()->active = mp;
    return mp ? mp : p;
}

static int a_mouse_press(Pane *p) {
    InEvent *ev = E.cur_event;
    Cursor *c;
    Loc l;
    long long now = plat_now_ms();
    if (!ev) return 0;
    p = mouse_pane(p);
    if (ev->my >= p->y + pane_text_height(p)) return 1; /* status line */
    pane_screen_to_loc(p, ev->mx, ev->my, &l);
    pane_reset_cursors(p);
    c = CUR;
    if (now - p->last_click < 400 && loc_cmp(l, p->click_loc) == 0) {
        p->clicks = p->clicks % 3 + 1;
    } else {
        p->clicks = 1;
    }
    p->last_click = now;
    p->click_loc = l;
    c->loc = l;
    if (p->clicks == 2) {
        select_word_at(p, c);
        p->drag_mode = DRAG_WORD;
    } else if (p->clicks == 3) {
        select_line_at(p, c);
        p->drag_mode = DRAG_LINE;
    } else {
        if (ev->mods & MOD_SHIFT) {
            begin_select(c);
            if (!cur_has_sel(c)) c->orig[0] = c->orig[1] = c->sel[0];
            select_to(p, c, l);
        } else {
            c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = l;
        }
        p->drag_mode = DRAG_CHAR;
    }
    p->dragging = 1;
    cur_store_vx(p, c);
    return 1;
}

static int a_mouse_drag(Pane *p) {
    InEvent *ev = E.cur_event;
    Cursor *c = CUR;
    Loc l;
    if (!ev || !p->dragging) return 0;
    pane_screen_to_loc(p, ev->mx, ev->my, &l);
    if (p->drag_mode == DRAG_WORD) {
        Cursor t = *c;
        t.loc = l;
        select_word_at(p, &t);
        if (loc_cmp(l, c->orig[0]) < 0) l = t.sel[0];
        else l = t.sel[1];
    } else if (p->drag_mode == DRAG_LINE) {
        if (loc_cmp(l, c->orig[0]) < 0) l = loc_make(0, l.y);
        else l = l.y + 1 < BUF->nlines ? loc_make(0, l.y + 1) : loc_make(BUF->lines[l.y].len, l.y);
    }
    c->loc = l;
    select_to(p, c, l);
    cur_store_vx(p, c);
    /* scroll if dragging past edges */
    if (ev->my < p->y) pane_scroll(p, -1);
    else if (ev->my >= p->y + pane_text_height(p)) pane_scroll(p, 1);
    return 1;
}

static int a_mouse_release(Pane *p) {
    p->dragging = 0;
    return 1;
}

static int a_mouse_multi_cursor(Pane *p) {
    InEvent *ev = E.cur_event;
    Loc l;
    int i;
    if (!ev) return 0;
    p = mouse_pane(p);
    pane_screen_to_loc(p, ev->mx, ev->my, &l);
    for (i = 0; i < p->ncur; i++) {
        if (loc_cmp(p->cur[i].loc, l) == 0 && p->ncur > 1) {
            pane_remove_cursor(p, i);
            return 1;
        }
    }
    i = pane_add_cursor(p, l);
    p->active = i;
    return 1;
}

/* ---- comment plugin ---- */

static const struct {
    const char *ft, *fmt;
} comment_types[] = {
    {"apacheconf", "# %s"}, {"batch", ":: %s"}, {"c", "// %s"}, {"c++", "// %s"}, {"cmake", "# %s"},
    {"conf", "# %s"}, {"crystal", "# %s"}, {"csharp", "// %s"}, {"css", "/* %s */"}, {"d", "// %s"},
    {"dart", "// %s"}, {"dockerfile", "# %s"}, {"elm", "-- %s"}, {"fish", "# %s"}, {"gdscript", "# %s"},
    {"glsl", "// %s"}, {"go", "// %s"}, {"haskell", "-- %s"}, {"html", "<!-- %s -->"}, {"ini", "; %s"},
    {"java", "// %s"}, {"javascript", "// %s"}, {"jinja2", "{# %s #}"}, {"json", "// %s"},
    {"julia", "# %s"}, {"kotlin", "// %s"}, {"lua", "-- %s"}, {"markdown", "<!-- %s -->"},
    {"nginx", "# %s"}, {"nim", "# %s"}, {"objective-c", "// %s"}, {"ocaml", "(* %s *)"},
    {"pascal", "{ %s }"}, {"perl", "# %s"}, {"php", "// %s"}, {"pony", "// %s"},
    {"powershell", "# %s"}, {"proto", "// %s"}, {"python", "# %s"}, {"python3", "# %s"},
    {"ruby", "# %s"}, {"rust", "// %s"}, {"scala", "// %s"}, {"shell", "# %s"}, {"sql", "-- %s"},
    {"swift", "// %s"}, {"tex", "% %s"}, {"toml", "# %s"}, {"twig", "{# %s #}"},
    {"typescript", "// %s"}, {"v", "// %s"}, {"xml", "<!-- %s -->"}, {"yaml", "# %s"},
    {"zig", "// %s"}, {"zscript", "// %s"}, {"zsh", "# %s"},
};

static const char *comment_fmt(Buffer *b) {
    size_t i;
    const char *ct = BOPTS(b, O_COMMENTTYPE);
    if (ct && *ct && strstr(ct, "%s")) return ct;
    for (i = 0; i < sizeof comment_types / sizeof comment_types[0]; i++)
        if (!strcmp(comment_types[i].ft, BOPTS(b, O_FILETYPE))) return comment_types[i].fmt;
    return "# %s";
}

static int a_comment(Pane *p) {
    Buffer *b = BUF;
    Cursor *c = CUR;
    const char *fmt = comment_fmt(b);
    const char *ps = strstr(fmt, "%s");
    char *pre = xstrndup(fmt, (size_t)(ps - fmt)); /* e.g. "// " */
    const char *post = ps + 2;                     /* text after the comment body */
    int y0, y1, y, all = 1, minind = 1 << 30, any = 0;
    char *pre_t;
    size_t pl, ptl = strlen(post);
    if (readonly_check(p)) { free(pre); return 0; }
    sel_line_range(p, c, &y0, &y1);
    pre_t = xstrdup(pre);
    pl = strlen(pre_t);
    while (pl > 0 && pre_t[pl - 1] == ' ') pre_t[--pl] = 0; /* "//" */
    for (y = y0; y <= y1; y++) {
        int wl;
        const char *s = buf_leading_ws(b, y, &wl);
        if (wl == b->lines[y].len) continue;
        any = 1;
        if (wl < minind) minind = wl;
        if (strncmp(s + wl, pre_t, pl) != 0) all = 0;
    }
    if (!any) { free(pre); free(pre_t); return 1; }
    for (y = y0; y <= y1; y++) {
        int wl;
        const char *s = buf_leading_ws(b, y, &wl);
        if (wl == b->lines[y].len) continue;
        if (all) {
            /* uncomment */
            int rm = (int)pl;
            int len = b->lines[y].len;
            if (!strncmp(s + wl, pre, strlen(pre))) rm = (int)strlen(pre);
            if (ptl > 0) {
                const char *ln = b->lines[y].s;
                size_t pt2 = ptl;
                if (len >= (int)ptl && !strncmp(ln + len - ptl, post, ptl)) {
                    buf_remove(b, loc_make(len - (int)pt2, y), loc_make(len, y));
                } else {
                    const char *post_t = post;
                    while (*post_t == ' ') post_t++;
                    pt2 = strlen(post_t);
                    if (len >= (int)pt2 && !strncmp(ln + len - pt2, post_t, pt2))
                        buf_remove(b, loc_make(len - (int)pt2, y), loc_make(len, y));
                }
            }
            buf_remove(b, loc_make(wl, y), loc_make(wl + rm, y));
        } else {
            if (ptl > 0) buf_insert(b, loc_make(b->lines[y].len, y), post, (int)ptl);
            buf_insert(b, loc_make(minind, y), pre, (int)strlen(pre));
        }
    }
    free(pre);
    free(pre_t);
    if (!cur_has_sel(c)) cur_store_vx(p, c);
    return 1;
}

/* ================================================================== */
/* Action table                                                        */

typedef struct {
    const char *name;
    ActionFn fn;
    int multi;
} ActionDef;

static const ActionDef actions[] = {
    {"CursorUp", a_cursor_up, 1}, {"CursorDown", a_cursor_down, 1},
    {"CursorPageUp", a_cursor_page_up, 1}, {"CursorPageDown", a_cursor_page_down, 1},
    {"CursorLeft", a_cursor_left, 1}, {"CursorRight", a_cursor_right, 1},
    {"CursorStart", a_cursor_start, 1}, {"CursorEnd", a_cursor_end, 1},
    {"CursorToViewTop", a_cursor_to_view_top, 0}, {"CursorToViewCenter", a_cursor_to_view_center, 0},
    {"CursorToViewBottom", a_cursor_to_view_bottom, 0},
    {"SelectToStart", a_select_to_start, 1}, {"SelectToEnd", a_select_to_end, 1},
    {"SelectUp", a_select_up, 1}, {"SelectDown", a_select_down, 1},
    {"SelectLeft", a_select_left, 1}, {"SelectRight", a_select_right, 1},
    {"WordRight", a_word_right, 1}, {"WordLeft", a_word_left, 1},
    {"SubWordRight", a_subword_right, 1}, {"SubWordLeft", a_subword_left, 1},
    {"SelectWordRight", a_select_word_right, 1}, {"SelectWordLeft", a_select_word_left, 1},
    {"SelectSubWordRight", a_select_subword_right, 1}, {"SelectSubWordLeft", a_select_subword_left, 1},
    {"DeleteWordRight", a_delete_word_right, 1}, {"DeleteWordLeft", a_delete_word_left, 1},
    {"DeleteSubWordRight", a_delete_subword_right, 1}, {"DeleteSubWordLeft", a_delete_subword_left, 1},
    {"SelectLine", a_select_line, 1}, {"SelectToStartOfLine", a_select_to_start_of_line, 1},
    {"SelectToStartOfText", a_select_to_start_of_text, 1},
    {"SelectToStartOfTextToggle", a_select_to_start_of_text_toggle, 1},
    {"SelectToEndOfLine", a_select_to_end_of_line, 1},
    {"ParagraphPrevious", a_paragraph_previous, 1}, {"ParagraphNext", a_paragraph_next, 1},
    {"SelectToParagraphPrevious", a_select_to_paragraph_previous, 1},
    {"SelectToParagraphNext", a_select_to_paragraph_next, 1},
    {"InsertNewline", a_insert_newline, 1}, {"Backspace", a_backspace, 1}, {"Delete", a_delete, 1},
    {"InsertTab", a_insert_tab, 1}, {"Save", a_save, 0}, {"SaveAll", a_save_all, 0},
    {"SaveAs", a_save_as, 0}, {"Find", a_find, 0}, {"FindLiteral", a_find_literal, 0},
    {"FindNext", a_find_next, 1}, {"FindPrevious", a_find_previous, 1},
    {"DiffNext", a_diff_next, 0}, {"DiffPrevious", a_diff_previous, 0}, {"Center", a_center, 0},
    {"Undo", a_undo, 0}, {"Redo", a_redo, 0}, {"Copy", a_copy, 1}, {"CopyLine", a_copy_line, 1},
    {"Cut", a_cut, 1}, {"CutLine", a_cut_line, 1}, {"Duplicate", a_duplicate, 1},
    {"DuplicateLine", a_duplicate_line, 1}, {"DeleteLine", a_delete_line, 1},
    {"MoveLinesUp", a_move_lines_up, 1}, {"MoveLinesDown", a_move_lines_down, 1},
    {"IndentSelection", a_indent_selection, 1}, {"OutdentSelection", a_outdent_selection, 1},
    {"Autocomplete", a_autocomplete, 0}, {"CycleAutocompleteBack", a_cycle_autocomplete_back, 0},
    {"OutdentLine", a_outdent_line, 1}, {"IndentLine", a_indent_line, 1}, {"Paste", a_paste, 1},
    {"PastePrimary", a_paste_primary, 1}, {"SelectAll", a_select_all, 0}, {"OpenFile", a_open_file, 0},
    {"Start", a_start, 0}, {"End", a_end, 0}, {"PageUp", a_page_up, 0}, {"PageDown", a_page_down, 0},
    {"SelectPageUp", a_select_page_up, 1}, {"SelectPageDown", a_select_page_down, 1},
    {"HalfPageUp", a_half_page_up, 0}, {"HalfPageDown", a_half_page_down, 0},
    {"StartOfText", a_start_of_text, 1}, {"StartOfTextToggle", a_start_of_text_toggle, 1},
    {"StartOfLine", a_start_of_line, 1}, {"EndOfLine", a_end_of_line, 1},
    {"ToggleHelp", a_toggle_help, 0}, {"ToggleKeyMenu", a_toggle_keymenu, 0},
    {"ToggleDiffGutter", a_toggle_diff_gutter, 0}, {"ToggleRuler", a_toggle_ruler, 0},
    {"ToggleHighlightSearch", a_toggle_highlight_search, 0},
    {"UnhighlightSearch", a_unhighlight_search, 0}, {"ResetSearch", a_reset_search, 0},
    {"ClearStatus", a_clear_status, 0}, {"ShellMode", a_shell_mode, 0},
    {"CommandMode", a_command_mode, 0}, {"ToggleOverwriteMode", a_toggle_overwrite, 0},
    {"Escape", a_escape, 0}, {"Quit", a_quit, 0}, {"QuitAll", a_quit_all, 0},
    {"ForceQuit", a_force_quit, 0}, {"AddTab", a_add_tab, 0}, {"PreviousTab", a_previous_tab, 0},
    {"NextTab", a_next_tab, 0}, {"FirstTab", a_first_tab, 0}, {"LastTab", a_last_tab, 0},
    {"NextSplit", a_next_split, 0}, {"PreviousSplit", a_previous_split, 0},
    {"FirstSplit", a_first_split, 0}, {"LastSplit", a_last_split, 0}, {"Unsplit", a_unsplit, 0},
    {"VSplit", a_vsplit, 0}, {"HSplit", a_hsplit, 0}, {"ToggleMacro", a_toggle_macro, 0},
    {"PlayMacro", a_play_macro, 0}, {"Suspend", a_suspend, 0}, {"ScrollUp", a_scroll_up, 0},
    {"ScrollDown", a_scroll_down, 0}, {"SpawnMultiCursor", a_spawn_multi_cursor, 0},
    {"SpawnMultiCursorUp", a_spawn_multi_cursor_up, 0},
    {"SpawnMultiCursorDown", a_spawn_multi_cursor_down, 0},
    {"SpawnMultiCursorSelect", a_spawn_multi_cursor_select, 0},
    {"RemoveMultiCursor", a_remove_multi_cursor, 0},
    {"RemoveAllMultiCursors", a_remove_all_multi_cursors, 0},
    {"SkipMultiCursor", a_skip_multi_cursor, 0}, {"SkipMultiCursorBack", a_skip_multi_cursor_back, 0},
    {"JumpToMatchingBrace", a_jump_to_matching_brace, 1}, {"JumpLine", a_jump_line, 0},
    {"Deselect", a_deselect, 0}, {"ClearInfo", a_clear_info, 0}, {"None", a_none, 0},
    {"MousePress", a_mouse_press, 0}, {"MouseDrag", a_mouse_drag, 0},
    {"MouseRelease", a_mouse_release, 0}, {"MouseMultiCursor", a_mouse_multi_cursor, 0},
    {"Comment", a_comment, 0},
};

#define NACTIONS ((int)(sizeof actions / sizeof actions[0]))

ActionFn action_find(const char *name) {
    int i;
    for (i = 0; i < NACTIONS; i++)
        if (!strcmp(actions[i].name, name)) return actions[i].fn;
    return NULL;
}

int action_is_multi(const char *name) {
    int i;
    for (i = 0; i < NACTIONS; i++)
        if (!strcmp(actions[i].name, name)) return actions[i].multi;
    return 0;
}

const char **action_names(int *n) {
    static const char *names[sizeof actions / sizeof actions[0]];
    int i;
    for (i = 0; i < NACTIONS; i++) names[i] = actions[i].name;
    *n = NACTIONS;
    return names;
}

static void cmd_edit_done(const char *resp, int canceled, void *data) {
    (void)data;
    if (canceled || !resp) return;
    command_run(cur_pane(), resp);
}

/* run one action by name on pane p (handles multi-cursor iteration) */
static int run_one(Pane *p, const char *name) {
    ActionFn fn;
    int ok = 1, i;
    if (starts_with(name, "command:")) {
        command_run(p, name + 8);
        return 1;
    }
    if (starts_with(name, "command-edit:")) {
        prompt_text("> ", name + 13, HIST_COMMAND, cmd_edit_done, NULL, NULL);
        return 1;
    }
    if (!strcmp(name, "lua:comment.comment")) name = "Comment";
    fn = action_find(name);
    if (!fn) {
        editor_set_error("Unknown action: %s", name);
        return 0;
    }
    if (strcmp(name, "Autocomplete") && strcmp(name, "CycleAutocompleteBack")) ac_clear();
    if (action_is_multi(name) && p->ncur > 1) {
        int saved = p->active;
        for (i = 0; i < p->ncur; i++) {
            p->active = i;
            ok = fn(p) && ok;
        }
        p->active = saved < p->ncur ? saved : p->ncur - 1;
        merge_cursors(p);
    } else {
        if (!action_is_multi(name) || p->ncur <= 1) {
            ok = fn(p);
        }
    }
    /* like micro, macros record only cursor/edit actions and typed text */
    if (action_is_multi(name)) macro_record(0, name, (int)strlen(name), 0);
    return ok;
}

/* is p still a pane of some tab? (actions can close panes and tabs) */
static int pane_alive(Pane *p) {
    int i;
    if (!p) return 0;
    for (i = 0; i < E.ntabs; i++)
        if (tab_find_node(E.tabs[i]->root, p)) return 1;
    return 0;
}

int run_action_string(Pane *p, const char *acts, InEvent *ev) {
    /* work on a private copy: the string usually belongs to the bindings
       table, and an action (e.g. "command:bind ...") may replace or free it */
    char *copy = xstrdup(acts), *s = copy;
    int ok = 1, tab0 = E.curtab, ntabs0 = E.ntabs;
    char sep = 0;
    Pane *active0 = cur_pane();
    (void)ev;
    no_relocate = 0;
    while (*s) {
        char *e = s, save;
        /* command: actions may contain separators; take the rest of the string */
        if (starts_with(s, "command:") || starts_with(s, "command-edit:")) {
            e = s + strlen(s);
        } else {
            while (*e && *e != ',' && *e != '|' && *e != '&') e++;
        }
        if (sep == '&' && !ok) break;
        if (sep == '|' && ok) break;
        if (cur_pane() != active0) p = active0 = cur_pane(); /* an action switched panes */
        else if (!pane_alive(p)) p = cur_pane();             /* ...or closed the target */
        if (!p) break;
        save = *e;
        *e = 0;
        {
            int had_prompt = prompt_active();
            ok = run_one(p, s);
            /* an action opened a prompt: the rest of the chain must not run
               underneath it (it could close the pane the prompt refers to) */
            if (!had_prompt && prompt_active()) { save = 0; }
        }
        if (E.quit || !save) break;
        sep = save;
        s = e + 1;
    }
    free(copy);
    if (cur_pane() != active0 || !pane_alive(p)) p = cur_pane();
    if (p && !no_relocate && E.curtab == tab0 && E.ntabs == ntabs0) pane_relocate(p);
    return ok;
}
