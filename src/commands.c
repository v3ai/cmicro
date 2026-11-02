/*
 * commands.c - the command bar (Ctrl-e): set, open, save, replace, goto,
 * vsplit, tab, help, run, bind, ... plus completion and help pages.
 */
#include "micro.h"
#include "platform.h"
#include "runtime.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int save_pane(Pane *p, int then_quit);
int autocomplete_suggestions(Pane *p, char ***out, int *idx);

/* ================================================================== */
/* Shell helpers (ISO C plat_system())                                      */

static char *tmpname(const char *tag) {
    char *dir = plat_temp_dir(), *r;
    r = xsprintf("%s/micro-%s-%ld-%d", dir ? dir : ".", tag, (long)time(NULL), rand());
    free(dir);
    return r;
}


/* run cmd with optional stdin text; returns malloc'd combined output */
static char *run_capture(const char *cmd, const char *input, int inlen, int *status) {
    char *out = tmpname("out"), *in = NULL, *full, *data;
    long len = 0;
    int rc;
    if (input) {
        FILE *f;
        in = tmpname("in");
        f = plat_fopen(in, "wb");
        if (f) {
            fwrite(input, 1, (size_t)inlen, f);
            fclose(f);
        }
    }
    full = plat_capture_cmd(cmd, in, out);
    rc = full ? plat_system(full) : -1;
    if (status) *status = rc;
    data = read_file(out, &len);
    plat_remove(out);
    if (in) plat_remove(in);
    free(full);
    free(out);
    free(in);
    if (data && plat_is_sep('\\')) {
        /* Windows programs print CRLF */
        char *r = data, *o = data;
        for (; *r; r++) if (*r != '\r') *o++ = *r;
        *o = 0;
    }
    return data ? data : xstrdup("");
}

void run_shell_interactive(const char *cmd) {
    int c;
    scr_fini();
    plat_term_restore();
    fflush(stdout);
    if (plat_system(cmd) == -1) fputs("failed to run command\n", stdout);
    fputs("\nPress enter to continue", stdout);
    fflush(stdout);
    while ((c = getchar()) != EOF && c != '\n')
        ;
    clearerr(stdin);
    plat_term_init();
    scr_init();
    scr_set_mouse(GOPT(O_MOUSE));
    scr_invalidate();
    history_add(HIST_SHELL, cmd);
}

/* ================================================================== */
/* Options                                                             */

static void refresh_after_option(int id) {
    switch (id) {
    case O_COLORSCHEME: {
        char *err = NULL;
        if (colorscheme_load(GOPTS(O_COLORSCHEME), &err) != 0) {
            editor_set_error("%s", err);
            free(err);
        }
        group_styles_refresh();
        scr_invalidate();
        break;
    }
    case O_MOUSE: scr_set_mouse(GOPT(O_MOUSE)); break;
    case O_TRUECOLOR: {
        scr_truecolor = scr_detect_truecolor(GOPTS(O_TRUECOLOR));
        scr_invalidate();
        break;
    }
    default: break;
    }
    editor_layout();
}

typedef struct {
    int id;
    OptVal v;
} SetCtx;

static void set_global_fn(Pane *p, void *data) {
    SetCtx *c = data;
    if (!p->buf->local[c->id]) {
        opt_copy(c->id, &p->buf->opts[c->id], &c->v);
        buf_option_changed(p->buf, c->id);
    }
}

void set_option_cmd(Pane *p, const char *name, const char *value, int local) {
    int id = opt_find(name);
    OptVal v;
    char *err = NULL;
    if (id < 0) {
        editor_set_error("Invalid option");
        return;
    }
    if (local && id >= O_FIRST_GLOBAL) {
        editor_set_error("Option %s is global-only", name);
        return;
    }
    if (opt_parse(id, value, &v, &err) != 0) {
        editor_set_error("%s", err);
        free(err);
        return;
    }
    if (id == O_COLORSCHEME) {
        char *probe = runtime_read(RT_COLORSCHEME, value);
        if (!probe) {
            editor_set_error("%s is not a valid colorscheme", value);
            free(v.s);
            return;
        }
        free(probe);
    }
    if (id == O_FILETYPE && strcmp(value, "unknown") && strcmp(value, "off") && !syntax_by_filetype(value)) {
        editor_set_error("%s is not a valid filetype", value);
        free(v.s);
        return;
    }
    if (local) {
        buf_set_option(p->buf, id, &v, 1);
        if (id == O_FILETYPE) apply_ftoptions(p->buf);
    } else {
        SetCtx c;
        opt_copy(id, &gopts[id], &v);
        if (id < O_FIRST_GLOBAL) {
            c.id = id;
            c.v = v;
            editor_all_panes(set_global_fn, &c);
        }
        settings_note_set(name, value);
        if (settings_save() != 0)
            editor_set_error("Option set for this session; settings.json not written (it could not be read or written)");
    }
    free(v.s);
    refresh_after_option(id);
}

/* ================================================================== */
/* Completion                                                          */

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char **filter_prefix(const char *const *cands, int n, const char *prefix) {
    char **out = xmalloc(sizeof *out * (size_t)(n + 1));
    int i, k = 0;
    size_t pl = strlen(prefix);
    for (i = 0; i < n; i++)
        if (!strncmp(cands[i], prefix, pl)) out[k++] = xstrdup(cands[i]);
    out[k] = NULL;
    qsort(out, (size_t)k, sizeof *out, cmpstr);
    return out;
}

char **file_complete(const char *arg, int *n) {
    char *exp = expand_home(arg);
    const char *slash = path_last_sep(exp);
    char *dir, *base, **ents, **out;
    int i, k = 0, cnt = 0;
    if (slash) {
        dir = xstrndup(exp, (size_t)(slash - exp + 1));
        base = xstrdup(slash + 1);
    } else {
        dir = xstrdup("");
        base = xstrdup(exp);
    }
    ents = plat_list_dir(*dir ? dir : ".");
    if (!ents) {
        free(dir); free(base); free(exp);
        if (n) *n = 0;
        out = xmalloc(sizeof *out);
        out[0] = NULL;
        return out;
    }
    for (cnt = 0; ents[cnt]; cnt++)
        ;
    out = xmalloc(sizeof *out * (size_t)(cnt + 1));
    for (i = 0; i < cnt; i++) {
        if (ents[i][0] == '.' && base[0] != '.') continue;
        if (!strncmp(ents[i], base, strlen(base))) {
            /* keep the user's spelling of the directory part (e.g. "~/") */
            const char *uslash = path_last_sep(arg);
            char *udir = uslash ? xstrndup(arg, (size_t)(uslash - arg + 1)) : xstrdup("");
            out[k++] = xsprintf("%s%s", udir, ents[i]);
            free(udir);
        }
    }
    out[k] = NULL;
    free_strv(ents);
    free(dir);
    free(base);
    free(exp);
    if (n) *n = k;
    return out;
}

static const char *help_topics[] = {"help", "defaultkeys", "commands", "options", "keybindings", "colors"};

static const char *cmd_names[] = {
    "set", "setlocal", "toggle", "togglelocal", "reset", "show", "showkey", "run", "bind",
    "unbind", "quit", "goto", "jump", "save", "replace", "replaceall", "vsplit", "hsplit",
    "tab", "help", "eval", "log", "plugin", "reload", "reopen", "cd", "pwd", "open", "tabmove",
    "tabswitch", "term", "memusage", "retab", "raw", "textfilter", "comment",
};
#define NCMDS ((int)(sizeof cmd_names / sizeof cmd_names[0]))

const char **command_names(int *n) {
    *n = NCMDS;
    return cmd_names;
}

static char **option_value_candidates(int id, int *n) {
    char **v;
    int k = 0;
    const OptDef *d = &opt_defs[id];
    if (d->type == OT_BOOL) {
        v = xmalloc(sizeof *v * 3);
        v[k++] = xstrdup("on");
        v[k++] = xstrdup("off");
        v[k] = NULL;
    } else if (d->choices) {
        int i;
        for (i = 0; d->choices[i]; i++)
            ;
        v = xmalloc(sizeof *v * (size_t)(i + 1));
        for (i = 0; d->choices[i]; i++) v[k++] = xstrdup(d->choices[i]);
        v[k] = NULL;
    } else if (id == O_COLORSCHEME) {
        v = colorscheme_list();
        for (k = 0; v[k]; k++)
            ;
    } else if (id == O_FILETYPE) {
        v = syntax_filetypes();
        for (k = 0; v[k]; k++)
            ;
    } else {
        v = xmalloc(sizeof *v);
        v[0] = NULL;
    }
    *n = k;
    return v;
}

char **command_complete(const char *input, int cursor, int *start) {
    int argc = 0, i;
    const char *last_space;
    char *upto = xstrndup(input, (size_t)cursor);
    char **argv, **res;
    char quote = 0;
    last_space = strrchr(upto, ' ');
    if (!last_space) {
        *start = 0;
        res = filter_prefix(cmd_names, NCMDS, upto);
        free(upto);
        return res;
    }
    /* start of the last token, honoring quotes and backslash escapes */
    {
        int k, q = 0, tok = 0;
        for (k = 0; upto[k]; k++) {
            char ch = upto[k];
            if (q) {
                if (ch == q) q = 0;
                else if (ch == '\\' && q == '"' && upto[k + 1]) k++;
            } else if (ch == '\\' && upto[k + 1]) {
                k++;
            } else if (ch == '"' || ch == '\'') {
                q = ch;
            } else if (ch == ' ' || ch == '\t') {
                tok = k + 1;
            }
        }
        *start = tok;
        quote = (upto[tok] == '"' || upto[tok] == '\'') ? upto[tok] : 0;
    }
    argv = shell_split(upto, &argc);
    if (upto[strlen(upto) - 1] == ' ') {
        argv = xrealloc(argv, sizeof *argv * (size_t)(argc + 2));
        argv[argc++] = xstrdup("");
        argv[argc] = NULL;
    }
    res = NULL;
    if (argc >= 2) {
        const char *cmd = argv[0], *cur = argv[argc - 1];
        if (!strcmp(cmd, "set") || !strcmp(cmd, "setlocal") || !strcmp(cmd, "toggle") ||
            !strcmp(cmd, "togglelocal") || !strcmp(cmd, "reset") || !strcmp(cmd, "show")) {
            if (argc == 2) {
                const char *names[O_COUNT];
                for (i = 0; i < O_COUNT; i++) names[i] = opt_defs[i].name;
                res = filter_prefix(names, O_COUNT, cur);
            } else if (argc == 3 && (!strcmp(cmd, "set") || !strcmp(cmd, "setlocal"))) {
                int id = opt_find(argv[1]);
                if (id >= 0) {
                    int n;
                    char **cands = option_value_candidates(id, &n);
                    res = filter_prefix((const char *const *)cands, n, cur);
                    free_strv(cands);
                }
            }
        } else if (!strcmp(cmd, "help")) {
            res = filter_prefix(help_topics, (int)(sizeof help_topics / sizeof help_topics[0]), cur);
        } else if (!strcmp(cmd, "bind") && argc == 3) {
            int n;
            const char **an = action_names(&n);
            res = filter_prefix(an, n, cur);
        } else if (!strcmp(cmd, "open") || !strcmp(cmd, "save") || !strcmp(cmd, "cd") ||
                   !strcmp(cmd, "vsplit") || !strcmp(cmd, "hsplit") || !strcmp(cmd, "tab")) {
            res = file_complete(cur, NULL);
        }
    }
    free_strv(argv);
    free(upto);
    if (!res) {
        res = xmalloc(sizeof *res);
        res[0] = NULL;
    }
    /* re-quote candidates so they splice back into the command line */
    for (i = 0; res[i]; i++) {
        Str o;
        const char *c;
        int win = plat_is_sep('\\'); /* Windows: backslashes are path separators */
        char q = quote;
        if (!q && !strpbrk(res[i], win ? " \t\"'" : " \t\"'\\")) continue;
        if (!q && win) q = '"'; /* Windows: quote instead of escaping */
        str_init(&o);
        str_reserve(&o, 0);
        if (q) str_putc(&o, q);
        for (c = res[i]; *c; c++) {
            if (q == '"' && (*c == '"' || (*c == '\\' && !win))) str_putc(&o, '\\');
            else if (!q && strchr(" \t\"'\\", *c)) str_putc(&o, '\\');
            str_putc(&o, *c);
        }
        free(res[i]);
        res[i] = str_detach(&o);
    }
    return res;
}

/* ================================================================== */
/* Help                                                                */

void help_open(Pane *p, const char *topic) {
    char *text = runtime_read(RT_HELP, topic), *name;
    Buffer *b;
    if (!text) {
        editor_set_error("Sorry, no help for %s", topic);
        return;
    }
    name = xsprintf("%s.md", topic);
    b = buf_from_string(name, text, BT_HELP);
    free(b->name);
    b->name = xstrdup(topic);
    free(name);
    free(text);
    if (p->buf->type == BT_HELP) {
        pane_set_buffer(p, b);
    } else {
        int vert = !strcmp(GOPTS(O_HELPSPLIT), "vsplit");
        tab_split(cur_tab(), p, b, vert);
    }
    editor_layout();
}

/* ================================================================== */
/* ftoptions plugin                                                    */

void apply_ftoptions(Buffer *b) {
    const char *ft = BOPTS(b, O_FILETYPE);
    if (!GOPT(O_FTOPTIONS)) return;
    if (b->local[O_TABSTOSPACES]) return;
    if (!strcmp(ft, "go") || !strcmp(ft, "makefile")) {
        b->opts[O_TABSTOSPACES].i = 0;
    } else if (!strcmp(ft, "fish") || !strcmp(ft, "python") || !strcmp(ft, "python2") ||
               !strcmp(ft, "python3") || !strcmp(ft, "yaml") || !strcmp(ft, "nim")) {
        b->opts[O_TABSTOSPACES].i = 1;
    }
}

/* ================================================================== */
/* Replace                                                             */

static char *expand_repl(const char *repl, const char *line, const int *caps, int ncap, int literal) {
    Str o;
    const char *s = repl;
    str_init(&o);
    str_reserve(&o, 0);
    if (literal) {
        str_puts(&o, repl);
        return str_detach(&o);
    }
    while (*s) {
        if (*s == '$') {
            int g = -1;
            if (s[1] == '$') { str_putc(&o, '$'); s += 2; continue; }
            if (s[1] == '{') {
                const char *e = strchr(s + 2, '}');
                if (e && isdigit((unsigned char)s[2])) { g = atoi(s + 2); s = e + 1; }
            } else if (isdigit((unsigned char)s[1])) {
                g = 0;
                s++;
                while (isdigit((unsigned char)*s)) g = g * 10 + (*s++ - '0');
            }
            if (g >= 0) {
                if (g < ncap && caps[2 * g] >= 0) str_putn(&o, line + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
                continue;
            }
            str_putc(&o, *s++);
        } else {
            str_putc(&o, *s++);
        }
    }
    return str_detach(&o);
}

static Regex *compile_search(Buffer *b, const char *pat, int literal, char **err) {
    return re_compile(pat, (BOPT(b, O_IGNORECASE) ? RE_ICASE : 0) | (literal ? RE_LITERAL : 0), err);
}

static int replace_all(Pane *p, const char *search, const char *repl, int literal, Loc a, Loc z) {
    Buffer *b = p->buf;
    char *err = NULL;
    Regex *re = compile_search(b, search, literal, &err);
    int y, count = 0, ncap;
    int *caps;
    if (!re) {
        editor_set_error("%s", err);
        free(err);
        return -1;
    }
    ncap = re_ncaps(re);
    caps = xmalloc(sizeof(int) * (size_t)(2 * ncap));
    for (y = a.y; y <= z.y && y < b->nlines; y++) {
        Line *ln = &b->lines[y];
        int from = y == a.y ? a.x : 0;
        int to = y == z.y ? z.x : ln->len;
        int pos = from, changed = 0;
        Str out;
        str_init(&out);
        str_reserve(&out, 0);
        str_putn(&out, ln->s, from);
        while (pos <= to && re_search(re, ln->s, to, pos, caps, ncap)) {
            char *r = expand_repl(repl, ln->s, caps, ncap, literal);
            str_putn(&out, ln->s + pos, caps[0] - pos);
            str_puts(&out, r);
            free(r);
            count++;
            changed = 1;
            if (caps[1] == caps[0]) {
                if (caps[1] < to) {
                    int nx = utf8_next(ln->s, to, caps[1]);
                    str_putn(&out, ln->s + caps[1], nx - caps[1]);
                    pos = nx;
                } else {
                    pos = to; /* the tail starting at `to` is copied below */
                    break;
                }
            } else {
                pos = caps[1];
            }
        }
        if (changed) {
            int tail = pos < ln->len ? pos : ln->len;
            str_putn(&out, ln->s + tail, ln->len - tail);
            {
                int newlen = out.len;
                int delta = newlen - ln->len;
                buf_replace(b, loc_make(0, y), loc_make(ln->len, y), out.s, out.len);
                if (y == z.y) z.x += delta;
            }
        }
        str_free(&out);
    }
    free(caps);
    re_free(re);
    return count;
}

typedef struct {
    Pane *p;
    char *search, *repl;
    int literal;
    Regex *re;
    Loc pos;     /* where to continue searching */
    Loc origin;
    int wrapped;
    int count;
    Loc ms, me;
} ReplCtx;

static void repl_finish(ReplCtx *r) {
    if (r->count == 0) editor_set_message("Nothing matched %s", r->search);
    else editor_set_message("Replaced %d occurrence%s of %s", r->count, r->count == 1 ? "" : "s", r->search);
    {
        Cursor *c = ACUR(r->p);
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    re_free(r->re);
    free(r->search);
    free(r->repl);
    free(r);
}

static void repl_step(ReplCtx *r);

static void repl_answer(int yes, int canceled, void *data) {
    ReplCtx *r = data;
    Buffer *b = r->p->buf;
    if (canceled) { repl_finish(r); return; }
    if (yes) {
        int ncap = re_ncaps(r->re), *caps = xmalloc(sizeof(int) * (size_t)(2 * ncap));
        Line *ln = &b->lines[r->ms.y];
        char *rep;
        Loc end;
        re_search(r->re, ln->s, ln->len, r->ms.x, caps, ncap);
        rep = expand_repl(r->repl, ln->s, caps, ncap, r->literal);
        free(caps);
        end = buf_replace(b, r->ms, r->me, rep, (int)strlen(rep));
        if (r->origin.y == r->ms.y && r->origin.x > r->ms.x)
            r->origin.x += (int)strlen(rep) - (r->me.x - r->ms.x);
        free(rep);
        r->count++;
        r->pos = end;
        if (loc_cmp(r->me, r->ms) == 0) {
            r->pos = buf_move_chars(b, end, 1);
            if (loc_cmp(r->pos, end) == 0) { repl_finish(r); return; } /* end of buffer */
        }
    } else {
        r->pos = loc_cmp(r->me, r->ms) == 0 ? buf_move_chars(b, r->me, 1) : r->me;
        if (loc_cmp(r->me, r->ms) == 0 && loc_cmp(r->pos, r->me) == 0) { repl_finish(r); return; }
    }
    repl_step(r);
}

static void repl_step(ReplCtx *r) {
    Buffer *b = r->p->buf;
    Cursor *c = ACUR(r->p);
    int caps[2], y;
    for (;;) {
        for (y = r->pos.y; y < b->nlines; y++) {
            Line *ln = &b->lines[y];
            int from = y == r->pos.y ? r->pos.x : 0;
            if (from > ln->len) continue;
            if (re_search(r->re, ln->s, ln->len, from, caps, 1)) {
                Loc ms = loc_make(caps[0], y);
                if (r->wrapped && loc_cmp(ms, r->origin) >= 0) { repl_finish(r); return; }
                r->ms = ms;
                r->me = loc_make(caps[1], y);
                c->sel[0] = c->orig[0] = r->ms;
                c->sel[1] = c->orig[1] = r->me;
                c->loc = r->me;
                pane_relocate(r->p);
                prompt_yn("Perform replacement (y,n,esc)", repl_answer, r);
                return;
            }
            if (r->wrapped && y >= r->origin.y) { repl_finish(r); return; }
        }
        if (r->wrapped) { repl_finish(r); return; }
        r->wrapped = 1;
        r->pos = loc_make(0, 0);
    }
}

static void cmd_replace(Pane *p, int argc, char **argv, int all) {
    int literal = 0, doall = all, i, k = 0;
    char *args[2] = {NULL, NULL};
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) doall = 1;
        else if (!strcmp(argv[i], "-l")) literal = 1;
        else if (k < 2) args[k++] = argv[i];
    }
    if (k < 2) {
        editor_set_error("Not enough arguments");
        return;
    }
    if (doall) {
        Cursor *c = ACUR(p);
        Loc a = loc_make(0, 0), z = buf_end(p->buf);
        int n;
        if (cur_has_sel(c)) { a = c->sel[0]; z = c->sel[1]; }
        n = replace_all(p, args[0], args[1], literal, a, z);
        if (n == 0) editor_set_message("Nothing matched %s", args[0]);
        else if (n > 0) editor_set_message("Replaced %d occurrence%s of %s", n, n == 1 ? "" : "s", args[0]);
        c->loc = buf_clamp(p->buf, c->loc);
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    } else {
        ReplCtx *r = xcalloc(1, sizeof *r);
        char *err = NULL;
        r->p = p;
        r->search = xstrdup(args[0]);
        r->repl = xstrdup(args[1]);
        r->literal = literal;
        r->re = compile_search(p->buf, args[0], literal, &err);
        if (!r->re) {
            editor_set_error("%s", err);
            free(err);
            free(r->search);
            free(r->repl);
            free(r);
            return;
        }
        r->pos = r->origin = cur_has_sel(ACUR(p)) ? ACUR(p)->sel[0] : ACUR(p)->loc;
        repl_step(r);
    }
}

/* ================================================================== */
/* Opening files                                                       */

/* parse "file:line:col" and "+line:col" */
static void parse_cursor_spec(const char *s, int *line, int *col) {
    *line = -1;
    *col = -1;
    if (!s || !*s) return;
    *line = atoi(s);
    s = strchr(s, ':');
    if (s) *col = atoi(s + 1);
}

static void goto_line_col(Pane *p, int line, int col) {
    Cursor *c = ACUR(p);
    Buffer *b = p->buf;
    if (line < 0) return;
    if (line == 0) line = 1;
    cur_deselect(c, 1); /* before moving: deselecting jumps to the selection start */
    c->loc.y = clampi(line - 1, 0, b->nlines - 1);
    c->loc.x = col > 0 ? utf8_offset(b->lines[c->loc.y].s, b->lines[c->loc.y].len, col - 1) : 0;
    c->loc = buf_clamp(b, c->loc);
    c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    cur_store_vx(p, c);
    pane_relocate(p);
}

static void open_into(Pane *p, const char *arg) {
    char *path = xstrdup(arg), *err = NULL;
    int line = -1, col = -1;
    Buffer *b;
    if (GOPT(O_PARSECURSOR)) {
        char *c1 = strchr(path, ':');
        if (c1 && isdigit((unsigned char)c1[1])) {
            parse_cursor_spec(c1 + 1, &line, &col);
            *c1 = 0;
        }
    }
    b = editor_find_buffer(path);
    if (!b) {
        b = buf_open(path, &err);
        if (!b) {
            editor_set_error("%s", err);
            free(err);
            free(path);
            return;
        }
        settings_apply_parsed(b, path);
        apply_ftoptions(b);
    }
    cursor_save(p);
    pane_set_buffer(p, b);
    cursor_restore(p);
    if (line > 0) goto_line_col(p, line, col);
    free(path);
}

typedef struct {
    Pane *p;
    char *path;
} OpenCtx;

static void open_yn(int yes, int canceled, void *data) {
    OpenCtx *o = data;
    if (!canceled) {
        if (yes) save_pane(o->p, 0);
        open_into(o->p, o->path);
    }
    free(o->path);
    free(o);
}

/* ================================================================== */
/* Misc commands                                                       */

static void reopen_yn(int yes, int canceled, void *data) {
    Pane *p = data;
    char *err = NULL;
    if (canceled) return;
    if (yes) save_pane(p, 0);
    if (buf_reload(p->buf, &err) != 0) {
        editor_set_error("%s", err);
        free(err);
    }
    ACUR(p)->loc = buf_clamp(p->buf, ACUR(p)->loc);
}

static void retab(Pane *p) {
    Buffer *b = p->buf;
    int y, ts = BOPT(b, O_TABSIZE), spaces = BOPT(b, O_TABSTOSPACES);
    for (y = 0; y < b->nlines; y++) {
        Line *ln = &b->lines[y];
        int i = 0, vx = 0;
        Str ws;
        while (i < ln->len && (ln->s[i] == ' ' || ln->s[i] == '\t')) {
            vx += ln->s[i] == '\t' ? ts - vx % ts : 1;
            i++;
        }
        if (i == 0) continue;
        str_init(&ws);
        str_reserve(&ws, 0);
        if (spaces) {
            int k;
            for (k = 0; k < vx; k++) str_putc(&ws, ' ');
        } else {
            int k;
            for (k = 0; k < vx / ts; k++) str_putc(&ws, '\t');
            for (k = 0; k < vx % ts; k++) str_putc(&ws, ' ');
        }
        if (ws.len != i || memcmp(ws.s, ln->s, (size_t)i))
            buf_replace(b, loc_make(0, y), loc_make(i, y), ws.s, ws.len);
        str_free(&ws);
    }
}

static void show_log(Pane *p);

void command_run(Pane *p, const char *line) {
    int argc = 0;
    char **argv;
    const char *cmd;
    if (!p) return;
    while (*line == ' ') line++;
    if (!*line) return;
    argv = shell_split(line, &argc);
    if (argc == 0) { free_strv(argv); return; }
    cmd = argv[0];

    if (!strcmp(cmd, "set") || !strcmp(cmd, "setlocal")) {
        if (argc < 3) editor_set_error("Not enough arguments");
        else set_option_cmd(p, argv[1], argv[2], !strcmp(cmd, "setlocal"));
    } else if (!strcmp(cmd, "toggle") || !strcmp(cmd, "togglelocal")) {
        int id = argc >= 2 ? opt_find(argv[1]) : -1;
        if (argc < 2) editor_set_error("Not enough arguments");
        else if (id < 0) editor_set_error("Invalid option");
        else if (opt_defs[id].type != OT_BOOL) editor_set_error("Option not toggleable");
        else {
            int local = !strcmp(cmd, "togglelocal");
            int cur = id < O_FIRST_GLOBAL ? (local ? p->buf->opts[id].i : gopts[id].i) : gopts[id].i;
            set_option_cmd(p, argv[1], cur ? "off" : "on", local);
        }
    } else if (!strcmp(cmd, "reset")) {
        int id = argc >= 2 ? opt_find(argv[1]) : -1;
        if (id < 0) editor_set_error(argc < 2 ? "Not enough arguments" : "Invalid option");
        else {
            set_option_cmd(p, argv[1], opt_defs[id].def, 0);
            settings_note_reset(argv[1]);
            settings_save();
        }
    } else if (!strcmp(cmd, "show")) {
        int id = argc >= 2 ? opt_find(argv[1]) : -1;
        if (id < 0) editor_set_error(argc < 2 ? "Not enough arguments" : "Invalid option");
        else {
            char *v = opt_to_string(id, id < O_FIRST_GLOBAL ? &p->buf->opts[id] : &gopts[id]);
            editor_set_message("%s", v);
            free(v);
        }
    } else if (!strcmp(cmd, "showkey")) {
        int key, mods;
        if (argc < 2) editor_set_error("Please provide a key to show");
        else if (!key_parse(argv[1], &key, &mods)) editor_set_error("%s is not a valid key", argv[1]);
        else {
            InEvent ev;
            const char *a;
            memset(&ev, 0, sizeof ev);
            ev.type = IN_KEY;
            ev.key = key;
            ev.mods = mods;
            a = binding_for_event(&ev, 0);
            if (a) editor_set_message("%s", a);
            else editor_set_message("%s has no binding", argv[1]);
        }
    } else if (!strcmp(cmd, "run")) {
        if (argc < 2) editor_set_error("Not enough arguments");
        else {
            const char *rest = strstr(line, "run") + 3;
            int st;
            char *out, *q;
            while (*rest == ' ') rest++;
            out = run_capture(rest, NULL, 0, &st);
            for (q = out; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
            while (q > out && q[-1] == ' ') *--q = 0;
            editor_set_message("%s", out);
            free(out);
        }
    } else if (!strcmp(cmd, "bind")) {
        if (argc < 3) editor_set_error("Not enough arguments");
        else if (bind_key(argv[1], argv[2], 1) != 0) editor_set_error("Invalid key or bindings are locked");
    } else if (!strcmp(cmd, "unbind")) {
        if (argc < 2) editor_set_error("Not enough arguments");
        else unbind_key(argv[1], 1);
    } else if (!strcmp(cmd, "quit")) {
        run_action_string(p, "Quit", NULL);
    } else if (!strcmp(cmd, "goto") || !strcmp(cmd, "jump")) {
        if (argc < 2) editor_set_error("Not enough arguments");
        else {
            int line_n, col = -1, rel = !strcmp(cmd, "jump");
            const char *a = argv[1];
            char *colon = strchr(a, ':');
            if (!isdigit((unsigned char)a[0]) && a[0] != '-' && a[0] != '+') {
                editor_set_error("Invalid line number");
            } else {
                line_n = atoi(a);
                if (colon) col = atoi(colon + 1);
                if (rel) line_n = ACUR(p)->loc.y + 1 + line_n;
                else if (line_n < 0) line_n = p->buf->nlines + 1 + line_n;
                goto_line_col(p, line_n, col);
            }
        }
    } else if (!strcmp(cmd, "save")) {
        if (argc >= 2) {
            char *err = NULL;
            if (buf_save(p->buf, argv[1], &err) == 0) editor_set_message("Saved %s", argv[1]);
            else { editor_set_error("%s", err); free(err); }
        } else {
            save_pane(p, 0);
        }
    } else if (!strcmp(cmd, "replace") || !strcmp(cmd, "replaceall")) {
        cmd_replace(p, argc, argv, !strcmp(cmd, "replaceall"));
    } else if (!strcmp(cmd, "vsplit") || !strcmp(cmd, "hsplit")) {
        int vert = !strcmp(cmd, "vsplit");
        if (argc >= 2) {
            char *err = NULL;
            Buffer *b = editor_find_buffer(argv[1]);
            if (!b) {
                b = buf_open(argv[1], &err);
                if (b) { settings_apply_parsed(b, argv[1]); apply_ftoptions(b); }
            }
            if (!b) { editor_set_error("%s", err); free(err); }
            else tab_split(cur_tab(), p, b, vert);
        } else {
            tab_split(cur_tab(), p, buf_new_empty(), vert);
        }
        editor_layout();
    } else if (!strcmp(cmd, "tab")) {
        if (argc == 1) editor_add_tab(pane_new(buf_new_empty()));
        else {
            int i;
            for (i = 1; i < argc; i++) {
                char *err = NULL;
                Pane *np = editor_open_file_pane(argv[i], &err);
                if (np) editor_add_tab(np);
                else { editor_set_error("%s", err); free(err); }
            }
        }
    } else if (!strcmp(cmd, "tabmove")) {
        if (argc < 2) editor_set_error("Not enough arguments: provide an index, starting at 1");
        else {
            int target, cur = E.curtab;
            Tab *t;
            if (argv[1][0] == '+' || argv[1][0] == '-') target = cur + atoi(argv[1]);
            else target = atoi(argv[1]) - 1;
            target = clampi(target, 0, E.ntabs - 1);
            t = E.tabs[cur];
            if (target < cur) memmove(E.tabs + target + 1, E.tabs + target, sizeof *E.tabs * (size_t)(cur - target));
            else memmove(E.tabs + cur, E.tabs + cur + 1, sizeof *E.tabs * (size_t)(target - cur));
            E.tabs[target] = t;
            E.curtab = target;
        }
    } else if (!strcmp(cmd, "tabswitch")) {
        if (argc < 2) editor_set_error("Not enough arguments");
        else {
            int i, found = 0;
            for (i = 0; i < E.ntabs; i++) {
                if (!strcmp(E.tabs[i]->active->buf->name, argv[1])) { E.curtab = i; found = 1; break; }
            }
            if (!found) {
                int n = atoi(argv[1]);
                if (n >= 1 && n <= E.ntabs) E.curtab = n - 1;
                else editor_set_error("Invalid tab index");
            }
            editor_layout();
        }
    } else if (!strcmp(cmd, "help")) {
        help_open(p, argc >= 2 ? argv[1] : "help");
    } else if (!strcmp(cmd, "eval") || !strcmp(cmd, "plugin") || !strcmp(cmd, "term")) {
        editor_set_error("%s is not supported (no Lua plugins / terminal emulator in this build)", cmd);
    } else if (!strcmp(cmd, "log")) {
        show_log(p);
    } else if (!strcmp(cmd, "reload")) {
        char *err = NULL;
        if (colorscheme_load(GOPTS(O_COLORSCHEME), &err) != 0) { editor_set_error("%s", err); free(err); }
        group_styles_refresh();
        scr_invalidate();
        editor_set_message("Reloaded colorscheme");
    } else if (!strcmp(cmd, "reopen")) {
        if (!*p->buf->path) editor_set_error("No file to reopen");
        else if (buf_modified(p->buf)) prompt_yn("Save file before reopen?", reopen_yn, p);
        else reopen_yn(0, 0, p);
    } else if (!strcmp(cmd, "cd")) {
        if (argc < 2) editor_set_error("Not enough arguments");
        else {
            char *d = expand_home(argv[1]);
            if (plat_chdir(d) != 0) editor_set_error("Error with cd: %s is not a directory", d);
            free(d);
        }
    } else if (!strcmp(cmd, "pwd")) {
        char *d = plat_getcwd();
        editor_set_message("%s", d ? d : "?");
        free(d);
    } else if (!strcmp(cmd, "open")) {
        if (argc < 2) editor_set_error("No filename");
        else if (buf_modified(p->buf) && p->buf->refs <= 1) {
            OpenCtx *o = xmalloc(sizeof *o);
            char *msg = xsprintf("Save changes to %s before closing? (y,n,esc)", *p->buf->name ? p->buf->name : "No name");
            o->p = p;
            o->path = xstrdup(argv[1]);
            prompt_yn(msg, open_yn, o);
            free(msg);
        } else {
            open_into(p, argv[1]);
        }
    } else if (!strcmp(cmd, "memusage")) {
        long total = 0;
        int y;
        for (y = 0; y < p->buf->nlines; y++) total += p->buf->lines[y].cap;
        editor_set_message("Current buffer text storage: %ld KB (ISO C cannot query process memory)", total / 1024);
    } else if (!strcmp(cmd, "retab")) {
        retab(p);
    } else if (!strcmp(cmd, "raw")) {
        E.showkey = 1;
        editor_set_message("Press a key to see its raw escape sequence (Ctrl-q to stop)");
    } else if (!strcmp(cmd, "textfilter")) {
        Cursor *c = ACUR(p);
        if (argc < 2) editor_set_error("usage: textfilter arguments");
        else if (!cur_has_sel(c)) editor_set_error("No selection");
        else {
            const char *rest = strstr(line, "textfilter") + 10;
            int len, st;
            char *sel = buf_get_text(p->buf, c->sel[0], c->sel[1], &len);
            char *out = run_capture(rest, sel, len, &st);
            Loc end = buf_replace(p->buf, c->sel[0], c->sel[1], out, (int)strlen(out));
            c->loc = end;
            c->sel[1] = c->orig[1] = end;
            free(sel);
            free(out);
        }
    } else if (!strcmp(cmd, "comment")) {
        run_action_string(p, "Comment", NULL);
    } else {
        editor_set_error("Unknown command: %s", cmd);
    }
    free_strv(argv);
}

/* ---------------- log ---------------- */

static Str logbuf;

void editor_log(const char *fmt, ...) {
    va_list ap;
    char tmp[1024];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    str_puts(&logbuf, tmp);
    str_putc(&logbuf, '\n');
}

static void show_log(Pane *p) {
    Buffer *b = buf_from_string("Log", logbuf.s ? logbuf.s : "", BT_LOG);
    tab_split(cur_tab(), p, b, 0);
    editor_layout();
}
