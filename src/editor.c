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

Editor cm_E;


Tab *cm_cur_tab(void) { return cm_E.ntabs > 0 ? cm_E.tabs[cm_E.curtab] : NULL; }

Pane *cm_cur_pane(void) {
    Tab *t = cm_cur_tab();
    return t ? t->active : NULL;
}

/* ---------------- messages ---------------- */

static void set_msg(int err, const char *fmt, va_list ap) {
    Str m;
    cm_str_init(&m);
    cm_str_vprintf(&m, fmt, ap);
    free(cm_E.msg);
    cm_E.msg = cm_str_detach(&m);
    cm_E.msg_error = err;
}

void cm_editor_set_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_msg(0, fmt, ap);
    va_end(ap);
}

void cm_editor_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_msg(1, fmt, ap);
    va_end(ap);
}

void cm_editor_clear_message(void) {
    free(cm_E.msg);
    cm_E.msg = NULL;
    cm_E.msg_error = 0;
}

/* ---------------- tabs / panes ---------------- */

void cm_editor_layout(void) {
    int i, first_line = cm_layout_top();
    int h = cm_scr_h() - first_line - cm_layout_bottom_reserved();
    if (h < 1) h = 1;
    for (i = 0; i < cm_E.ntabs; i++) cm_tab_layout(cm_E.tabs[i], 0, first_line, cm_scr_w(), h);
}

void cm_editor_add_tab(Pane *p) {
    Tab *t = cm_tab_new(p);
    cm_E.tabs = cm_xrealloc(cm_E.tabs, sizeof *cm_E.tabs * (size_t)(cm_E.ntabs + 1));
    cm_E.tabs[cm_E.ntabs++] = t;
    cm_E.curtab = cm_E.ntabs - 1;
    cm_editor_layout();
}

void cm_editor_close_tab(int i) {
    cm_tab_free(cm_E.tabs[i]);
    memmove(cm_E.tabs + i, cm_E.tabs + i + 1, sizeof *cm_E.tabs * (size_t)(cm_E.ntabs - i - 1));
    cm_E.ntabs--;
    if (cm_E.curtab >= cm_E.ntabs) cm_E.curtab = cm_E.ntabs - 1;
    if (cm_E.curtab < 0) cm_E.curtab = 0;
    cm_editor_layout();
}

void cm_editor_all_panes(void (*fn)(Pane *, void *), void *data) {
    int i, j, n;
    for (i = 0; i < cm_E.ntabs; i++) {
        Pane **a = cm_tab_panes(cm_E.tabs[i], &n);
        for (j = 0; j < n; j++) fn(a[j], data);
        free(a);
    }
}

typedef struct {
    const char *absp;
    Buffer *found;
} FindBuf;

static void find_buf_fn(Pane *p, void *data) {
    FindBuf *f = data;
    if (!f->found && p->buf->abspath && f->absp && !strcmp(p->buf->abspath, f->absp) && p->buf->type == BT_DEFAULT)
        f->found = p->buf;
}

Buffer *cm_editor_find_buffer(const char *path) {
    FindBuf f;
    char *expanded = cm_expand_home(path);
    char *absp = cm_plat_realpath(expanded);
    f.absp = absp;
    f.found = NULL;
    if (absp) cm_editor_all_panes(find_buf_fn, &f);
    free(absp);
    free(expanded);
    return f.found;
}

Pane *cm_editor_open_file_pane(const char *arg, char **err) {
    char *path = cm_xstrdup(arg);
    int line = -1, col = -1;
    Buffer *b;
    Pane *p;
    if (GOPT(O_PARSECURSOR)) {
        char *c1 = strchr(path, ':');
        if (c1 && c1[1] >= '0' && c1[1] <= '9') {
            line = cm_clamped_atoi(c1 + 1);
            {
                char *c2 = strchr(c1 + 1, ':');
                if (c2) col = cm_clamped_atoi(c2 + 1);
            }
            *c1 = 0;
        }
    }
    b = cm_editor_find_buffer(path);
    if (!b) {
        b = cm_buf_open(path, err);
        if (!b) {
            free(path);
            return NULL;
        }
        cm_settings_apply_parsed(b, path);
        cm_apply_ftoptions(b);
    }
    p = cm_pane_new(b);
    cm_cursor_restore(p);
    if (line > 0) {
        Cursor *c = ACUR(p);
        c->loc.y = cm_clampi(line - 1, 0, b->nlines - 1);
        c->loc.x = col > 0 ? cm_utf8_offset(b->lines[c->loc.y].s, b->lines[c->loc.y].len, col - 1) : 0;
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    free(path);
    return p;
}

void cm_editor_quit_pane(Pane *p, int force) {
    Tab *t = p->tab ? p->tab : cm_cur_tab();
    int ti;
    (void)force;
    cm_cursor_save(p);
    for (ti = 0; ti < cm_E.ntabs; ti++)
        if (cm_E.tabs[ti] == t) break;
    if (ti >= cm_E.ntabs) return;
    if (cm_tab_npanes(t) > 1) {
        cm_tab_close_pane(t, p);
        cm_editor_layout();
        return;
    }
    if (cm_E.ntabs > 1) {
        cm_editor_close_tab(ti);
        return;
    }
    cm_E.quit = 1;
}

/* ---------------- buffer edit hooks: keep cursors in place ---------------- */

static Loc adj_insert(Loc l, Loc s, Loc e) {
    if (cm_loc_cmp(l, s) < 0) return l;
    if (l.y == s.y) {
        l.x = e.x + (l.x - s.x);
        l.y = e.y;
    } else {
        l.y += e.y - s.y;
    }
    return l;
}

static Loc adj_remove(Loc l, Loc s, Loc e) {
    if (cm_loc_cmp(l, s) <= 0) return l;
    if (cm_loc_cmp(l, e) <= 0) return s;
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
        Loc (*f)(Loc, Loc, Loc) = h->type == TE_INSERT ? adj_insert : adj_remove;
        c->loc = f(c->loc, h->s, h->e);
        c->sel[0] = f(c->sel[0], h->s, h->e);
        c->sel[1] = f(c->sel[1], h->s, h->e);
        c->orig[0] = f(c->orig[0], h->s, h->e);
        c->orig[1] = f(c->orig[1], h->s, h->e);
        c->loc = cm_buf_clamp(h->b, c->loc);
        c->sel[0] = cm_buf_clamp(h->b, c->sel[0]);
        c->sel[1] = cm_buf_clamp(h->b, c->sel[1]);
        c->orig[0] = cm_buf_clamp(h->b, c->orig[0]);
        c->orig[1] = cm_buf_clamp(h->b, c->orig[1]);
    }
    if (p->first_line >= h->b->nlines) p->first_line = h->b->nlines - 1;
}

static void edit_hook(Buffer *b, int type, Loc s, Loc e) {
    HookCtx h;
    h.b = b;
    h.type = type;
    h.s = s;
    h.e = e;
    cm_editor_all_panes(hook_fn, &h);
}

static Loc get_cursor_hook(Buffer *b, int *num) {
    Pane *p = cm_cur_pane();
    *num = 0;
    if (p && p->buf == b) {
        *num = p->active;
        return ACUR(p)->loc;
    }
    return cm_loc_make(0, 0);
}

static void set_cursor_hook(Buffer *b, Loc l, int num) {
    Pane *p = cm_cur_pane();
    if (p && p->buf == b) {
        Cursor *c = num >= 0 && num < p->ncur ? &p->cur[num] : ACUR(p);
        c->loc = cm_buf_clamp(b, l);
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
}

void cm_editor_init_hooks(void) {
    cm_buf_edit_hook = edit_hook;
    cm_buf_get_cursor_hook = get_cursor_hook;
    cm_buf_set_cursor_hook = set_cursor_hook;
}

/* ---------------- rendering ---------------- */

static void draw_dividers(Node *n) {
    if (!n || n->kind == NODE_LEAF) return;
    if (n->kind == NODE_VSPLIT) {
        int x = n->a->x + n->a->w, y;
        const char *dc = GOPTS(O_DIVCHARS);
        int cp = '|';
        Style st = GOPT(O_DIVREVERSE) ? cm_style_reverse(cm_def_style) : cm_def_style;
        if (cm_color_has("divider")) st = cm_color_get("divider");
        if (dc && *dc) cm_utf8_decode(dc, (int)strlen(dc), &cp);
        for (y = n->y; y < n->y + n->h; y++) cm_scr_set(x, y, cp, st);
    }
    draw_dividers(n->a);
    draw_dividers(n->b);
}

void cm_editor_render(void) {
    Tab *t = cm_cur_tab();
    Pane **panes;
    int n, i;
    if (!t) return;
    cm_scr_clear();
    cm_scr_hide_cursor();
    cm_render_tabbar();
    panes = cm_tab_panes(t, &n);
    for (i = 0; i < n; i++) {
        Pane *p = panes[i];
        if (p != t->active) cm_render_pane(p, 0);
    }
    cm_render_pane(t->active, 1);
    free(panes);
    draw_dividers(t->root);
    {
        /* buffer autocomplete suggestions replace the status line */
        char **sugg;
        int idx, ns = cm_autocomplete_suggestions(t->active, &sugg, &idx);
        Pane *p = t->active;
        if (ns > 1 && BOPT(p->buf, O_STATUSLINE)) {
            Style ss = cm_color_has("statusline.suggestions") ? cm_color_get("statusline.suggestions")
                     : cm_color_has("statusline") ? cm_color_get("statusline") : cm_style_reverse(cm_def_style);
            int y = p->y + p->h - 1, x = p->x, k;
            cm_scr_fill(p->x, y, p->w, 1, ' ', ss);
            for (k = 0; k < ns && x < p->x + p->w; k++) {
                x += cm_scr_puts(x, y, sugg[k], k == idx ? cm_style_reverse(ss) : ss, p->x + p->w - x);
                x++;
            }
        }
    }
    cm_render_keymenu();
    cm_render_infobar();
    cm_scr_flush();
}

/* ---------------- events ---------------- */

static int tab_at_x(int mx) {
    int x = 0, i;
    for (i = 0; i < cm_E.ntabs; i++) {
        Pane *p = cm_E.tabs[i]->active;
        const char *nm = *p->buf->name ? p->buf->name : "No name";
        int w = 1, j = 0, len = (int)strlen(nm);
        while (j < len) {
            int cp, k = cm_utf8_decode(nm + j, len - j, &cp);
            w += cm_char_width(cp);
            j += k;
        }
        if (cm_buf_modified(p->buf)) w += 2;
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
        cm_E.showkey = 0;
        cm_editor_clear_message();
        return;
    }
    cm_str_init(&s);
    cm_str_puts(&s, "Raw event: ");
    for (i = 0; i < ev->rawlen; i++) {
        unsigned char c = (unsigned char)ev->raw[i];
        if (c == 27) cm_str_puts(&s, "\\x1b");
        else if (c < 32 || c >= 127) cm_str_printf(&s, "\\x%02x", c);
        else cm_str_putc(&s, (char)c);
    }
    kn = cm_key_name(ev->key, ev->mods);
    cm_str_printf(&s, "  (%s)", kn);
    free(kn);
    cm_editor_set_message("%s", s.s);
    cm_str_free(&s);
}

void cm_editor_handle_event(InEvent *ev) {
    Pane *p;
    const char *act;
    if (ev->type == IN_RESIZE) {
        cm_scr_resize();
        cm_editor_layout();
        return;
    }
    if (ev->type == IN_NONE) return;
    if (cm_E.showkey && ev->type == IN_KEY) {
        show_raw(ev);
        return;
    }
    if (cm_prompt_active()) {
        cm_prompt_handle(ev);
        return;
    }
    p = cm_cur_pane();
    if (!p) return;
    if (ev->type == IN_MOUSE) {
        Pane *target = p;
        if (!GOPT(O_MOUSE)) return;
        if (ev->key == KEY_MOUSE_LEFT && cm_layout_top() && ev->my == 0) {
            int ti = tab_at_x(ev->mx);
            if (ti >= 0) { cm_E.curtab = ti; cm_editor_layout(); }
            return;
        }
        if (ev->key >= KEY_WHEEL_UP && ev->key <= KEY_WHEEL_RIGHT) {
            Pane *mp = cm_tab_pane_at(cm_cur_tab(), ev->mx, ev->my);
            if (mp) target = mp;
        }
        act = cm_binding_for_event(ev, 0);
        if (act) {
            cm_E.cur_event = ev;
            cm_run_action_string(target, act, ev);
            cm_E.cur_event = NULL;
        }
        return;
    }
    if (ev->type == IN_PASTE) {
        if (BOPT(p->buf, O_READONLY)) {
            cm_editor_set_message("Warning: file is readonly");
            return;
        }
        cm_insert_text(p, ev->paste, ev->pastelen, 1);
        cm_pane_relocate(p);
        return;
    }
    if (ev->type != IN_KEY) return;
    act = cm_binding_for_event(ev, 0);
    if (act) {
        cm_E.cur_event = ev;
        cm_run_action_string(p, act, ev);
        cm_E.cur_event = NULL;
        return;
    }
    if (ev->key < KEY_SPECIAL && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        cm_insert_rune(p, ev->key);
        cm_pane_relocate(p);
    }
}

/* ---------------- external file changes ---------------- */

static void reload_yn(int yes, int canceled, void *data) {
    Buffer *b = data;
    char *err = NULL;
    if (canceled || !yes) {
        b->mtime = cm_plat_mtime(b->path);
        return;
    }
    if (cm_buf_reload(b, &err) != 0) {
        cm_editor_set_error("%s", err);
        free(err);
    }
}

void cm_editor_check_file_changes(void) {
    Pane *p = cm_cur_pane();
    Buffer *b;
    p_s64 mt;
    if (!p || cm_prompt_active()) return;
    b = p->buf;
    if (b->type != BT_DEFAULT || !*b->path || b->is_new || !strcmp(BOPTS(b, O_RELOAD), "disabled")) return;
    mt = cm_plat_mtime(b->path);
    if (mt == 0 || mt == b->mtime) return;
    if (!strcmp(BOPTS(b, O_RELOAD), "auto") && !cm_buf_modified(b)) {
        reload_yn(1, 0, b);
        return;
    }
    b->mtime = mt;
    cm_prompt_yn("The file on disk has changed. Reload file? (y,n,esc)", reload_yn, b);
}

static void autosave_fn(Pane *p, void *data) {
    (void)data;
    if (cm_buf_modified(p->buf) && *p->buf->path && p->buf->type == BT_DEFAULT) {
        char *err = NULL;
        if (cm_buf_save(p->buf, p->buf->path, &err) != 0) free(err);
    }
}

void cm_editor_autosave_tick(void) {
    p_s64 now = cm_plat_now_ms();
    int secs = GOPT(O_AUTOSAVE);
    if (secs <= 0) return;
    if (cm_E.last_autosave == 0) cm_E.last_autosave = now;
    if (now - cm_E.last_autosave >= (p_s64)secs * 1000) {
        cm_E.last_autosave = now;
        cm_editor_all_panes(autosave_fn, NULL);
    }
}
