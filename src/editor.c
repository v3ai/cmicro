/*
 * editor.c - global editor state, tabs, event dispatch and the frame
 * renderer.
 */
#include "micro.h"
#include "platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Editor E;

int autocomplete_suggestions(Pane *p, char ***out, int *idx);
int save_pane(Pane *p, int then_quit);

Tab *cur_tab(void) { return E.ntabs > 0 ? E.tabs[E.curtab] : NULL; }

Pane *cur_pane(void) {
    Tab *t = cur_tab();
    return t ? t->active : NULL;
}

/* ---------------- messages ---------------- */

static void set_msg(int err, const char *fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    free(E.msg);
    E.msg = xstrdup(buf);
    E.msg_error = err;
}

void editor_set_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_msg(0, fmt, ap);
    va_end(ap);
}

void editor_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_msg(1, fmt, ap);
    va_end(ap);
}

void editor_clear_message(void) {
    free(E.msg);
    E.msg = NULL;
    E.msg_error = 0;
}

/* ---------------- tabs / panes ---------------- */

void editor_layout(void) {
    int i, top = layout_top();
    int h = scr_h() - top - layout_bottom_reserved();
    if (h < 1) h = 1;
    for (i = 0; i < E.ntabs; i++) tab_layout(E.tabs[i], 0, top, scr_w(), h);
}

void editor_add_tab(Pane *p) {
    Tab *t = tab_new(p);
    E.tabs = xrealloc(E.tabs, sizeof *E.tabs * (size_t)(E.ntabs + 1));
    E.tabs[E.ntabs++] = t;
    E.curtab = E.ntabs - 1;
    editor_layout();
}

void editor_close_tab(int i) {
    tab_free(E.tabs[i]);
    memmove(E.tabs + i, E.tabs + i + 1, sizeof *E.tabs * (size_t)(E.ntabs - i - 1));
    E.ntabs--;
    if (E.curtab >= E.ntabs) E.curtab = E.ntabs - 1;
    if (E.curtab < 0) E.curtab = 0;
    editor_layout();
}

void editor_all_panes(void (*fn)(Pane *, void *), void *data) {
    int i, j, n;
    for (i = 0; i < E.ntabs; i++) {
        Pane **a = tab_panes(E.tabs[i], &n);
        for (j = 0; j < n; j++) fn(a[j], data);
        free(a);
    }
}

typedef struct {
    const char *abs;
    Buffer *found;
} FindBuf;

static void find_buf_fn(Pane *p, void *data) {
    FindBuf *f = data;
    if (!f->found && p->buf->abspath && f->abs && !strcmp(p->buf->abspath, f->abs) && p->buf->type == BT_DEFAULT)
        f->found = p->buf;
}

Buffer *editor_find_buffer(const char *path) {
    FindBuf f;
    char *exp = expand_home(path);
    char *abs = plat_realpath(exp);
    f.abs = abs;
    f.found = NULL;
    if (abs) editor_all_panes(find_buf_fn, &f);
    free(abs);
    free(exp);
    return f.found;
}

Pane *editor_open_file_pane(const char *arg, char **err) {
    char *path = xstrdup(arg);
    int line = -1, col = -1;
    Buffer *b;
    Pane *p;
    if (GOPT(O_PARSECURSOR)) {
        char *c1 = strchr(path, ':');
        if (c1 && c1[1] >= '0' && c1[1] <= '9') {
            line = atoi(c1 + 1);
            {
                char *c2 = strchr(c1 + 1, ':');
                if (c2) col = atoi(c2 + 1);
            }
            *c1 = 0;
        }
    }
    b = editor_find_buffer(path);
    if (!b) {
        b = buf_open(path, err);
        if (!b) {
            free(path);
            return NULL;
        }
        settings_apply_parsed(b, path);
        apply_ftoptions(b);
    }
    p = pane_new(b);
    cursor_restore(p);
    if (line > 0) {
        Cursor *c = ACUR(p);
        c->loc.y = clampi(line - 1, 0, b->nlines - 1);
        c->loc.x = col > 0 ? utf8_offset(b->lines[c->loc.y].s, b->lines[c->loc.y].len, col - 1) : 0;
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    free(path);
    return p;
}

void editor_quit_pane(Pane *p, int force) {
    Tab *t = p->tab ? p->tab : cur_tab();
    int ti;
    (void)force;
    cursor_save(p);
    for (ti = 0; ti < E.ntabs; ti++)
        if (E.tabs[ti] == t) break;
    if (ti >= E.ntabs) return;
    if (tab_npanes(t) > 1) {
        tab_close_pane(t, p);
        editor_layout();
        return;
    }
    if (E.ntabs > 1) {
        editor_close_tab(ti);
        return;
    }
    E.quit = 1;
}

/* ---------------- buffer edit hooks: keep cursors in place ---------------- */

static Loc adj_insert(Loc l, Loc s, Loc e) {
    if (loc_cmp(l, s) < 0) return l;
    if (l.y == s.y) {
        l.x = e.x + (l.x - s.x);
        l.y = e.y;
    } else {
        l.y += e.y - s.y;
    }
    return l;
}

static Loc adj_remove(Loc l, Loc s, Loc e) {
    if (loc_cmp(l, s) <= 0) return l;
    if (loc_cmp(l, e) <= 0) return s;
    if (l.y == e.y) {
        l.x = s.x + (l.x - e.x);
        l.y = s.y;
    } else {
        l.y -= e.y - s.y;
    }
    return l;
}

typedef struct {
    Buffer *b;
    int type;
    Loc s, e;
} HookCtx;

static void hook_fn(Pane *p, void *data) {
    HookCtx *h = data;
    int i;
    if (p->buf != h->b) return;
    for (i = 0; i < p->ncur; i++) {
        Cursor *c = &p->cur[i];
        Loc (*f)(Loc, Loc, Loc) = h->type == EV_INSERT ? adj_insert : adj_remove;
        c->loc = f(c->loc, h->s, h->e);
        c->sel[0] = f(c->sel[0], h->s, h->e);
        c->sel[1] = f(c->sel[1], h->s, h->e);
        c->orig[0] = f(c->orig[0], h->s, h->e);
        c->orig[1] = f(c->orig[1], h->s, h->e);
        c->loc = buf_clamp(h->b, c->loc);
        c->sel[0] = buf_clamp(h->b, c->sel[0]);
        c->sel[1] = buf_clamp(h->b, c->sel[1]);
        c->orig[0] = buf_clamp(h->b, c->orig[0]);
        c->orig[1] = buf_clamp(h->b, c->orig[1]);
    }
    if (p->top >= h->b->nlines) p->top = h->b->nlines - 1;
}

static void edit_hook(Buffer *b, int type, Loc s, Loc e) {
    HookCtx h;
    h.b = b;
    h.type = type;
    h.s = s;
    h.e = e;
    editor_all_panes(hook_fn, &h);
}

static Loc get_cursor_hook(Buffer *b, int *num) {
    Pane *p = cur_pane();
    *num = 0;
    if (p && p->buf == b) {
        *num = p->active;
        return ACUR(p)->loc;
    }
    return loc_make(0, 0);
}

static void set_cursor_hook(Buffer *b, Loc l, int num) {
    Pane *p = cur_pane();
    if (p && p->buf == b) {
        Cursor *c = num >= 0 && num < p->ncur ? &p->cur[num] : ACUR(p);
        c->loc = buf_clamp(b, l);
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
}

void editor_init_hooks(void) {
    buf_edit_hook = edit_hook;
    buf_get_cursor_hook = get_cursor_hook;
    buf_set_cursor_hook = set_cursor_hook;
}

/* ---------------- rendering ---------------- */

static void draw_dividers(Node *n) {
    if (!n || n->kind == NODE_LEAF) return;
    if (n->kind == NODE_VSPLIT) {
        int x = n->a->x + n->a->w, y;
        const char *dc = GOPTS(O_DIVCHARS);
        int cp = '|';
        Style st = GOPT(O_DIVREVERSE) ? style_reverse(def_style) : def_style;
        if (color_has("divider")) st = color_get("divider");
        if (dc && *dc) utf8_decode(dc, (int)strlen(dc), &cp);
        for (y = n->y; y < n->y + n->h; y++) scr_set(x, y, cp, st);
    }
    draw_dividers(n->a);
    draw_dividers(n->b);
}

void editor_render(void) {
    Tab *t = cur_tab();
    Pane **panes;
    int n, i;
    if (!t) return;
    scr_clear();
    scr_hide_cursor();
    render_tabbar();
    panes = tab_panes(t, &n);
    for (i = 0; i < n; i++) {
        Pane *p = panes[i];
        if (p != t->active) render_pane(p, 0);
    }
    render_pane(t->active, 1);
    free(panes);
    draw_dividers(t->root);
    {
        /* buffer autocomplete suggestions replace the status line */
        char **sugg;
        int idx, ns = autocomplete_suggestions(t->active, &sugg, &idx);
        Pane *p = t->active;
        if (ns > 1 && BOPT(p->buf, O_STATUSLINE)) {
            Style ss = color_has("statusline.suggestions") ? color_get("statusline.suggestions")
                     : color_has("statusline") ? color_get("statusline") : style_reverse(def_style);
            int y = p->y + p->h - 1, x = p->x, k;
            scr_fill(p->x, y, p->w, 1, ' ', ss);
            for (k = 0; k < ns && x < p->x + p->w; k++) {
                x += scr_puts(x, y, sugg[k], k == idx ? style_reverse(ss) : ss, p->x + p->w - x);
                x++;
            }
        }
    }
    render_keymenu();
    render_infobar();
    scr_flush();
}

/* ---------------- events ---------------- */

static int tab_at_x(int mx) {
    int x = 0, i;
    for (i = 0; i < E.ntabs; i++) {
        Pane *p = E.tabs[i]->active;
        const char *nm = *p->buf->name ? p->buf->name : "No name";
        int w = 1, j = 0, len = (int)strlen(nm);
        while (j < len) {
            int cp, k = utf8_decode(nm + j, len - j, &cp);
            w += char_width(cp);
            j += k;
        }
        if (buf_modified(p->buf)) w += 2;
        w += 3;
        if (mx >= x && mx < x + w) return i;
        x += w;
    }
    return -1;
}

static void show_raw(InEvent *ev) {
    Str s;
    int i;
    char *kn;
    if (ev->type == IN_KEY && ev->key == 'q' && ev->mods == MOD_CTRL) {
        E.showkey = 0;
        editor_clear_message();
        return;
    }
    str_init(&s);
    str_puts(&s, "Raw event: ");
    for (i = 0; i < ev->rawlen; i++) {
        unsigned char c = (unsigned char)ev->raw[i];
        if (c == 27) str_puts(&s, "\\x1b");
        else if (c < 32 || c >= 127) str_printf(&s, "\\x%02x", c);
        else str_putc(&s, (char)c);
    }
    kn = key_name(ev->key, ev->mods);
    str_printf(&s, "  (%s)", kn);
    free(kn);
    editor_set_message("%s", s.s);
    str_free(&s);
}

void editor_handle_event(InEvent *ev) {
    Pane *p;
    const char *act;
    if (ev->type == IN_RESIZE) {
        scr_resize();
        editor_layout();
        return;
    }
    if (ev->type == IN_NONE) return;
    if (E.showkey && ev->type == IN_KEY) {
        show_raw(ev);
        return;
    }
    if (prompt_active()) {
        prompt_handle(ev);
        return;
    }
    p = cur_pane();
    if (!p) return;
    if (ev->type == IN_MOUSE) {
        Pane *target = p;
        if (!GOPT(O_MOUSE)) return;
        if (ev->key == KEY_MOUSE_LEFT && layout_top() && ev->my == 0) {
            int ti = tab_at_x(ev->mx);
            if (ti >= 0) { E.curtab = ti; editor_layout(); }
            return;
        }
        if (ev->key >= KEY_WHEEL_UP && ev->key <= KEY_WHEEL_RIGHT) {
            Pane *mp = tab_pane_at(cur_tab(), ev->mx, ev->my);
            if (mp) target = mp;
        }
        act = binding_for_event(ev, 0);
        if (act) {
            E.cur_event = ev;
            run_action_string(target, act, ev);
            E.cur_event = NULL;
        }
        return;
    }
    if (ev->type == IN_PASTE) {
        if (BOPT(p->buf, O_READONLY)) {
            editor_set_message("Warning: file is readonly");
            return;
        }
        insert_text(p, ev->paste, ev->pastelen, 1);
        pane_relocate(p);
        return;
    }
    if (ev->type != IN_KEY) return;
    act = binding_for_event(ev, 0);
    if (act) {
        E.cur_event = ev;
        run_action_string(p, act, ev);
        E.cur_event = NULL;
        return;
    }
    if (ev->key < KEY_SPECIAL && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        insert_rune(p, ev->key);
        pane_relocate(p);
    }
}

/* ---------------- external file changes ---------------- */

static void reload_yn(int yes, int canceled, void *data) {
    Buffer *b = data;
    char *err = NULL;
    if (canceled || !yes) {
        b->mtime = plat_mtime(b->path);
        return;
    }
    if (buf_reload(b, &err) != 0) {
        editor_set_error("%s", err);
        free(err);
    }
}

void editor_check_external_changes(void) {
    Pane *p = cur_pane();
    Buffer *b;
    long long mt;
    if (!p || prompt_active()) return;
    b = p->buf;
    if (b->type != BT_DEFAULT || !*b->path || b->is_new || !strcmp(BOPTS(b, O_RELOAD), "disabled")) return;
    mt = plat_mtime(b->path);
    if (mt == 0 || mt == b->mtime) return;
    if (!strcmp(BOPTS(b, O_RELOAD), "auto") && !buf_modified(b)) {
        reload_yn(1, 0, b);
        return;
    }
    b->mtime = mt;
    prompt_yn("The file on disk has changed. Reload file? (y,n,esc)", reload_yn, b);
}

static void autosave_fn(Pane *p, void *data) {
    (void)data;
    if (buf_modified(p->buf) && *p->buf->path && p->buf->type == BT_DEFAULT) {
        char *err = NULL;
        if (buf_save(p->buf, p->buf->path, &err) != 0) free(err);
    }
}

void editor_autosave_tick(void) {
    long long now = plat_now_ms();
    int secs = GOPT(O_AUTOSAVE);
    if (secs <= 0) return;
    if (E.last_autosave == 0) E.last_autosave = now;
    if (now - E.last_autosave >= (long long)secs * 1000) {
        E.last_autosave = now;
        editor_all_panes(autosave_fn, NULL);
    }
}
