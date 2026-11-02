/*
 * render.c - drawing buffer panes, status lines, the tab bar, the info
 * bar (messages / prompts) and the key menu.
 *
 * Everything here draws through screen.c, which sanitizes every glyph
 * (control characters never reach the terminal); panes are clipped to
 * their own rectangle.
 */
#include "micro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int find_matching_brace(Buffer *b, Loc cur, int left, Loc *brace, Loc *match);

static const char *keydisplay[] = {
    "^Q Quit, ^S Save, ^O Open, ^G Help, ^E Command Bar, ^K Cut Line",
    "^F Find, ^Z Undo, ^Y Redo, ^A Select All, ^D Duplicate Line, ^T New Tab",
};

int layout_top(void) {
    return (E.ntabs > 1 || GOPT(O_TABALWAYS)) ? 1 : 0;
}

int layout_bottom_reserved(void) {
    int n = 0;
    if (GOPT(O_INFOBAR) || prompt_active()) n++;
    if (GOPT(O_KEYMENU)) n += 2;
    return n;
}

static int in_selection(Pane *p, Loc l) {
    int i;
    for (i = 0; i < p->ncur; i++) {
        Cursor *c = &p->cur[i];
        if (cur_has_sel(c) && loc_cmp(l, c->sel[0]) >= 0 && loc_cmp(l, c->sel[1]) < 0) return 1;
    }
    return 0;
}

static int other_cursor_at(Pane *p, Loc l, int active) {
    int i;
    for (i = 0; i < p->ncur; i++) {
        if (i == p->active && active) continue;
        if (loc_cmp(p->cur[i].loc, l) == 0 && !cur_has_sel(&p->cur[i])) return 1;
    }
    return 0;
}

static int str_width(const char *s, int len) {
    int i = 0, w = 0;
    while (i < len) {
        int cp;
        i += utf8_decode(s + i, len - i, &cp);
        w += char_width(cp);
    }
    return w;
}

/* ---------------- status line ---------------- */

static void status_subst(Pane *p, const char *fmt, Str *out) {
    Buffer *b = p->buf;
    Loc cl = buf_clamp(b, ACUR(p)->loc);
    const char *s = fmt;
    while (*s) {
        if (s[0] == '$' && s[1] == '(') {
            const char *e = strchr(s + 2, ')');
            char name[128];
            size_t n;
            if (!e) { str_puts(out, s); break; }
            n = (size_t)(e - (s + 2));
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, s + 2, n);
            name[n] = 0;
            s = e + 1;
            if (!strcmp(name, "filename")) {
                const char *nm = *b->name ? b->name : (*b->path ? b->path : "No name");
                str_puts(out, BOPT(b, O_BASENAME) && *nm ? path_base(nm) : nm);
            } else if (!strcmp(name, "modified")) {
                if (buf_modified(b)) str_puts(out, "+ ");
                else if (BOPT(b, O_READONLY)) str_puts(out, "[ro] ");
            } else if (!strcmp(name, "overwrite")) {
                if (p->overwrite && !BOPT(b, O_READONLY)) str_puts(out, "[ovwr] ");
            } else if (!strcmp(name, "line")) {
                str_printf(out, "%d", cl.y + 1);
            } else if (!strcmp(name, "col")) {
                str_printf(out, "%d", utf8_count(b->lines[cl.y].s, cl.x) + 1);
            } else if (!strcmp(name, "lines") || !strcmp(name, "status.lines")) {
                str_printf(out, "%d", b->nlines);
            } else if (!strcmp(name, "percentage")) {
                str_printf(out, "%d", (int)((cl.y + 1) * 100LL / (b->nlines > 0 ? b->nlines : 1)));
            } else if (!strcmp(name, "status.paste")) {
                if (GOPT(O_PASTE)) str_puts(out, "PASTE ");
            } else if (!strcmp(name, "status.vcol")) {
                str_printf(out, "%d", pane_visual_x(p, cl.y, cl.x));
            } else if (!strcmp(name, "status.bytes") || !strcmp(name, "status.size")) {
                long long total = 0;
                int y;
                for (y = 0; y < b->nlines; y++) total += b->lines[y].len + (y + 1 < b->nlines);
                if (!strcmp(name, "status.bytes")) str_printf(out, "%lld", total);
                else if (total < 1024) str_printf(out, "%lldB", total);
                else if (total < 1024 * 1024) str_printf(out, "%.1fKB", total / 1024.0);
                else str_printf(out, "%.1fMB", total / (1024.0 * 1024.0));
            } else if (starts_with(name, "opt:")) {
                int id = opt_find(name + 4);
                if (id >= 0) {
                    char *v = opt_to_string(id, id < O_FIRST_GLOBAL ? &b->opts[id] : &gopts[id]);
                    str_puts(out, v);
                    free(v);
                }
            } else if (starts_with(name, "bind:")) {
                const char *k = binding_key_for_action(name + 5);
                str_puts(out, k ? k : "null");
            }
        } else {
            /* copy literal text up to the next '$' in one go */
            const char *d = strchr(s + 1, '$');
            int n = d ? (int)(d - s) : (int)strlen(s);
            str_putn(out, s, n);
            s += n;
        }
    }
}

static void render_status(Pane *p, int active) {
    Str l, r;
    Style st = color_has("statusline") ? color_get("statusline") : style_reverse(def_style);
    int y = p->y + p->h - 1, lw, rw, x;
    if (!active && color_has("statusline.inactive")) st = color_get("statusline.inactive");
    str_init(&l);
    str_init(&r);
    str_reserve(&l, 0);
    str_reserve(&r, 0);
    status_subst(p, BOPTS(p->buf, O_STATUSFORMATL), &l);
    status_subst(p, BOPTS(p->buf, O_STATUSFORMATR), &r);
    scr_fill(p->x, y, p->w, 1, ' ', st);
    lw = scr_puts(p->x, y, l.s, st, p->w);
    rw = str_width(r.s, r.len);
    /* like micro: left text wins, right text fills whatever is left of its slot */
    x = p->x + p->w - rw;
    if (x < p->x + lw) x = p->x + lw;
    if (x < p->x + p->w) scr_puts(x, y, r.s, st, p->x + p->w - x);
    str_free(&l);
    str_free(&r);
}

/* ---------------- pane ---------------- */

/* colors looked up once per pane instead of once per cell */
typedef struct {
    int has_cl, has_cc, has_tws, has_taberr, has_sel, has_mb;
    Style cl, cc, hls, tws, taberr, sel, mb;
} PaneStyles;

static void load_styles(PaneStyles *ps) {
    ps->has_cl = color_has("cursor-line");
    ps->cl = color_get("cursor-line");
    ps->has_cc = color_has("color-column");
    ps->cc = color_get("color-column");
    ps->hls = color_get("hlsearch");
    ps->has_tws = color_has("trailingws");
    ps->tws = color_get("trailingws");
    ps->has_taberr = color_has("tab-error");
    ps->taberr = color_get("tab-error");
    ps->has_sel = color_has("selection");
    ps->sel = color_get("selection");
    ps->has_mb = color_has("match-brace");
    ps->mb = color_get("match-brace");
}

/* micro's "background from the group's foreground", group pre-looked-up */
static Style bg_of(Style s, int has, Style group) {
    if (has) s.bg = group.fg;
    return s;
}

static int loc_eq(Loc a, Loc b) { return a.x == b.x && a.y == b.y; }

void render_pane(Pane *p, int active) {
    Buffer *b = p->buf;
    int gw = pane_gutter_width(p), tw = pane_text_width(p), th = pane_text_height(p);
    int line = p->top, row = p->toprow, r;
    int softwrap = BOPT(b, O_SOFTWRAP);
    int ts = pane_tabsize(b);
    int syntax = BOPT(b, O_SYNTAX) && b->syn;
    Cursor *ac = ACUR(p);
    Loc acloc = buf_clamp(b, ac->loc);
    int curline = acloc.y;
    int cursorline = BOPT(b, O_CURSORLINE) && active && !cur_has_sel(ac);
    int colorcol = BOPT(b, O_COLORCOLUMN);
    int ndig = digits(b->nlines);
    int relruler = BOPT(b, O_RELATIVERULER);
    int hlsearch = BOPT(b, O_HLSEARCH) && b->hl_search && b->last_search && *b->last_search;
    int hltrail = BOPT(b, O_HLTRAILINGWS);
    int hltaberr = BOPT(b, O_HLTABERRORS) && BOPT(b, O_TABSTOSPACES);
    int anysel = 0, multi = p->ncur > 1, mb_highlight;
    Loc brace, match;
    int have_brace = 0;
    Style lnst = color_get("line-number"), clnst = color_get("current-line-number");
    Style ind_st = color_get("indent-char");
    PaneStyles ps;
    const char *ich = BOPTS(b, O_INDENTCHAR);
    int ichcp = ' ';
    unsigned char *smask = NULL;
    int smask_cap = 0;
    int *wbx = NULL, *wvx = NULL, wcap = 0, wn = 0, wline = -1; /* softwrap rows of `line` */
    int cursor_drawn = 0;
    char *showchars = BOPTS(b, O_SHOWCHARS);
    int show_tab = 0, show_space = 0, show_twsp = 0;

    load_styles(&ps);
    mb_highlight = !strcmp(BOPTS(b, O_MATCHBRACESTYLE), "highlight") && ps.has_mb;
    for (r = 0; r < p->ncur; r++)
        if (cur_has_sel(&p->cur[r])) anysel = 1;
    if (ich && *ich) utf8_decode(ich, (int)strlen(ich), &ichcp);
    if (showchars && *showchars) {
        /* micro's showchars: "tab=>,space=.,ispace=.." (subset) */
        const char *t;
        if ((t = strstr(showchars, "tab="))) utf8_decode(t + 4, (int)strlen(t + 4), &show_tab);
        if ((t = strstr(showchars, "space="))) utf8_decode(t + 6, (int)strlen(t + 6), &show_space);
        if ((t = strstr(showchars, "trailing="))) utf8_decode(t + 9, (int)strlen(t + 9), &show_twsp);
    }
    if (BOPT(b, O_MATCHBRACE) && active)
        have_brace = find_matching_brace(b, acloc, BOPT(b, O_MATCHBRACELEFT), &brace, &match);

    /* never draw outside the pane, however small it is */
    scr_clip(p->x, p->y, p->w, p->h);
    if (line < 0) line = 0;
    if (row < 0 || !softwrap) row = 0;

    for (r = 0; r < th; r++) {
        int sy = p->y + r;
        int nrows = 1;
        if (line >= b->nlines) {
            scr_fill(p->x, sy, p->w, 1, ' ', def_style);
            continue;
        }
        if (syntax) buf_ensure_hl(b, line);
        if (softwrap) {
            if (wline != line) {
                /* wrap each line once, not several times per row (a single
                   huge line used to cost O(length) per row, five times) */
                const int *bx, *vx;
                wn = pane_wrap_line(p, line, &bx, &vx);
                if (wn > wcap) {
                    wcap = wn;
                    wbx = xrealloc(wbx, sizeof *wbx * (size_t)wcap);
                    wvx = xrealloc(wvx, sizeof *wvx * (size_t)wcap);
                }
                memcpy(wbx, bx, sizeof *wbx * (size_t)wn);
                memcpy(wvx, vx, sizeof *wvx * (size_t)wn);
                wline = line;
            }
            nrows = wn;
            if (row >= nrows) row = nrows - 1;
        }
        /* gutter */
        if (gw > 0) {
            Style gs = line == curline && active ? clnst : lnst;
            char num[32];
            int i;
            if (row == 0) {
                int n = line + 1;
                if (relruler && line != curline) n = abs(line - curline);
                sprintf(num, "%*d ", ndig, n);
            } else {
                for (i = 0; i < ndig + 1 && i < 31; i++) num[i] = ' ';
                num[ndig + 1 < 31 ? ndig + 1 : 31] = 0;
            }
            scr_puts(p->x, sy, num, gs, gw);
        }
        /* text */
        {
            Line *ln = &b->lines[line];
            int bstart = 0, bend = ln->len, vstart = 0;
            int i, vx, x0 = p->x + gw;
            int trail = ln->len;
            int is_cl = cursorline && line == curline;
            int left = p->left;
            Style base_line = def_style;
            if (is_cl) base_line = bg_of(def_style, ps.has_cl, ps.cl);
            while (trail > 0 && (ln->s[trail - 1] == ' ' || ln->s[trail - 1] == '\t')) trail--;
            if (softwrap) {
                bstart = wbx[row];
                vstart = wvx[row];
                if (row + 1 < nrows) bend = wbx[row + 1];
                left = vstart;
            }
            if (hlsearch) {
                if (smask_cap < ln->len + 1) {
                    smask_cap = ln->len + 64;
                    smask = xrealloc(smask, (size_t)smask_cap);
                }
                memset(smask, 0, (size_t)ln->len + 1);
                search_highlight_ranges(p, line, smask, ln->len);
            }
            /* clear row */
            scr_fill(x0, sy, tw, 1, ' ', base_line);
            vx = vstart;
            i = bstart;
            while (i < bend) {
                int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
                int w = cp == '\t' ? ts - (vx % ts) : char_width(cp);
                int col = vx - left;
                Loc here = loc_make(i, line);
                if (col + w > 0 && col < tw) {
                    Style st = syntax && ln->hl ? group_style(ln->hl[i]) : def_style;
                    if (is_cl) st = bg_of(st, ps.has_cl, ps.cl);
                    if (colorcol > 0 && vx == colorcol) st = bg_of(st, ps.has_cc, ps.cc);
                    if (hlsearch && smask[i]) st = ps.hls;
                    if (hltrail && i >= trail && !(active && line == curline) && ps.has_tws) st = ps.tws;
                    if (hltaberr && cp == '\t' && ps.has_taberr) st = ps.taberr;
                    if (anysel && in_selection(p, here)) st = ps.has_sel ? ps.sel : style_reverse(st);
                    if (have_brace && (loc_eq(here, brace) || loc_eq(here, match))) {
                        if (mb_highlight) st = ps.mb;
                        else st.attr |= ATTR_UNDERLINE;
                    }
                    if (multi && other_cursor_at(p, here, 1)) st = style_reverse(st);
                    if (cp == '\t') {
                        /* only the visible part (tabs can be very wide) */
                        int k = col < 0 ? -col : 0, kend = w < tw - col ? w : tw - col;
                        for (; k < kend; k++) {
                            if (k == 0) {
                                int ch = show_tab ? show_tab : ichcp;
                                Style s2 = st;
                                if (ch != ' ' && !(anysel && in_selection(p, here))) {
                                    s2.fg = ind_st.fg;
                                    s2.attr = ind_st.attr;
                                }
                                scr_set(x0 + col + k, sy, ch, s2);
                            } else {
                                scr_set(x0 + col + k, sy, ' ', st);
                            }
                        }
                    } else if (col >= 0 && col + w <= tw) {
                        int ch = cp;
                        if (cp == ' ' && show_space && i < trail) ch = show_space;
                        if (cp == ' ' && show_twsp && i >= trail) ch = show_twsp;
                        scr_set(x0 + col, sy, ch, st);
                    } else {
                        int k;
                        for (k = 0; k < w; k++)
                            if (col + k >= 0 && col + k < tw) scr_set(x0 + col + k, sy, ' ', st);
                    }
                }
                if (active && loc_eq(here, acloc) && col >= 0 && col < tw) {
                    scr_show_cursor(x0 + col, sy);
                    cursor_drawn = 1;
                }
                vx = vx_add(vx, w);
                i += n;
                /* the rest of the row is off screen: nothing more to draw,
                   and a cursor further right couldn't be shown anyway */
                if (vx - left >= tw) break;
            }
            /* end of line cell */
            {
                int col = vx - left;
                Loc eol = loc_make(ln->len, line);
                if (i >= ln->len && bend == ln->len && col >= 0 && col < tw) {
                    Style st = base_line;
                    if (anysel && line + 1 < b->nlines && in_selection(p, eol))
                        st = ps.has_sel ? ps.sel : style_reverse(def_style);
                    if (multi && other_cursor_at(p, eol, 1)) st = style_reverse(st);
                    scr_set(x0 + col, sy, ' ', st);
                    if (active && loc_eq(acloc, eol)) {
                        scr_show_cursor(x0 + col, sy);
                        cursor_drawn = 1;
                    }
                }
                if (colorcol > 0) {
                    int cc = colorcol - left;
                    if (cc >= 0 && cc < tw && colorcol >= vx) {
                        Style st = scr_get_style(x0 + cc, sy);
                        st = bg_of(st, ps.has_cc, ps.cc);
                        scr_set_style(x0 + cc, sy, st);
                    }
                }
            }
        }
        /* advance */
        if (row + 1 < nrows) row++;
        else { line++; row = 0; }
    }
    free(smask);
    free(wbx);
    free(wvx);
    if (active && !cursor_drawn) scr_hide_cursor();
    if (BOPT(b, O_SCROLLBAR) && b->nlines > th) {
        int pos = (int)((long long)p->top * th / b->nlines);
        int sz = (int)((long long)th * th / b->nlines);
        int k;
        const char *sbc = GOPTS(O_SCROLLBARCHAR);
        int sbcp = '|';
        Style sbst = color_has("scrollbar") ? color_get("scrollbar") : def_style;
        if (sbc && *sbc) utf8_decode(sbc, (int)strlen(sbc), &sbcp);
        if (char_width(sbcp) != 1) sbcp = '|';
        if (sz < 1) sz = 1;
        for (k = pos; k < pos + sz && k < th; k++) scr_set(p->x + p->w - 1, p->y + k, sbcp, sbst);
    }
    if (BOPT(b, O_STATUSLINE)) render_status(p, active);
    scr_noclip();
}

/* ---------------- tab bar ---------------- */

static int tab_scroll; /* horizontal scroll of the tab bar (keeps the current tab visible) */

static const char *tab_name(int i) {
    Buffer *b = E.tabs[i]->active->buf;
    return *b->name ? b->name : (*b->path ? b->path : "No name");
}

/* width of tab i as drawn: "[name +]  " */
static int tab_width(int i) {
    const char *nm = tab_name(i);
    return 1 + str_width(nm, (int)strlen(nm)) + (buf_modified(E.tabs[i]->active->buf) ? 2 : 0) + 3;
}

static void tabbar_update_scroll(int w) {
    int i, x = 0, cs = 0, cw = 0;
    for (i = 0; i < E.ntabs; i++) {
        int tw = tab_width(i);
        if (i == E.curtab) { cs = x; cw = tw; }
        x += tw;
    }
    if (cs < tab_scroll) tab_scroll = cs;
    if (cs + cw > tab_scroll + w) tab_scroll = cs + cw - w;
    if (tab_scroll > x - w) tab_scroll = x - w;
    if (tab_scroll < 0) tab_scroll = 0;
}

int tabbar_tab_at(int mx) {
    int i, x = -tab_scroll;
    if (!layout_top()) return -1;
    for (i = 0; i < E.ntabs; i++) {
        int tw = tab_width(i);
        if (mx >= x && mx < x + tw) return i;
        x += tw;
    }
    return -1;
}

void render_tabbar(void) {
    int x, i, w = scr_w();
    int rev = GOPT(O_TABREVERSE), hl = GOPT(O_TABHIGHLIGHT);
    int charhl = (rev || hl) && !(rev && hl);
    Style base, active_st;
#define TABSTYLE(reverse, out_base, out_act)                               \
    do {                                                                   \
        out_base = color_has("tabbar") ? color_get("tabbar")               \
                                       : ((reverse) ? style_reverse(def_style) : def_style); \
        out_act = color_has("tabbar.active") ? color_get("tabbar.active") : out_base; \
    } while (0)
    if (!layout_top()) return;
    tabbar_update_scroll(w);
    TABSTYLE(rev, base, active_st);
    scr_fill(0, 0, w, 1, ' ', base);
    x = -tab_scroll;
    for (i = 0; i < E.ntabs && x < w; i++) {
        int act = i == E.curtab, tw = tab_width(i);
        if (x + tw <= 0) { x += tw; continue; } /* scrolled out on the left */
        TABSTYLE(charhl, base, active_st);
        scr_set(x++, 0, act ? '[' : ' ', act ? active_st : base);
        x += scr_puts(x, 0, tab_name(i), act ? active_st : base, -1);
        if (buf_modified(E.tabs[i]->active->buf)) x += scr_puts(x, 0, " +", act ? active_st : base, -1);
        scr_set(x++, 0, act ? ']' : ' ', act ? active_st : base);
        TABSTYLE(rev, base, active_st);
        scr_set(x++, 0, ' ', act ? active_st : base);
        scr_set(x++, 0, ' ', act ? active_st : base);
    }
#undef TABSTYLE
}

/* ---------------- key menu / info bar ---------------- */

void render_keymenu(void) {
    int i, h = scr_h();
    int infoh = (GOPT(O_INFOBAR) || prompt_active()) ? 1 : 0;
    if (!GOPT(O_KEYMENU)) return;
    for (i = 0; i < 2; i++) {
        int y = h - infoh - 2 + i;
        scr_fill(0, y, scr_w(), 1, ' ', def_style);
        scr_puts(0, y, keydisplay[i], def_style, scr_w());
    }
}

void render_infobar(void) {
    int y = scr_h() - 1, w = scr_w();
    Prompt *pr = &E.prompt;
    Style st = color_has("message") ? color_get("message") : def_style;
    if (!GOPT(O_INFOBAR) && !prompt_active()) return;
    scr_fill(0, y, w, 1, ' ', def_style);
    if (pr->kind == PROMPT_TEXT || pr->kind == PROMPT_YN || pr->kind == PROMPT_RAW) {
        int mx = scr_puts(0, y, pr->msg ? pr->msg : "", st, w);
        if (pr->kind == PROMPT_TEXT) {
            int avail = w - mx, i, vx = 0, cvx = 0;
            const char *s = pr->text.s ? pr->text.s : "";
            int len = pr->text.s ? pr->text.len : 0;
            int sa = -1, sb = -1;
            if (pr->anchor >= 0 && pr->anchor != pr->cx) {
                sa = pr->anchor < pr->cx ? pr->anchor : pr->cx;
                sb = pr->anchor < pr->cx ? pr->cx : pr->anchor;
            }
            for (i = 0; i < pr->cx && i < len;) {
                int cp, n = utf8_decode(s + i, len - i, &cp);
                cvx += char_width(cp);
                i += n;
            }
            if (avail < 1) avail = 1;
            if (cvx < pr->left) pr->left = cvx;
            if (cvx >= pr->left + avail) pr->left = cvx - avail + 1;
            for (i = 0; i < len;) {
                int cp, n = utf8_decode(s + i, len - i, &cp);
                int cw = char_width(cp);
                Style cs = def_style;
                if (vx - pr->left >= avail) break; /* the rest is off screen */
                if (sa >= 0 && i >= sa && i < sb)
                    cs = color_has("selection") ? color_get("selection") : style_reverse(def_style);
                if (vx >= pr->left && vx - pr->left + cw <= avail) scr_set(mx + vx - pr->left, y, cp, cs);
                vx += cw;
                i += n;
            }
            scr_show_cursor(mx + cvx - pr->left, y);
            /* completion suggestions */
            if (pr->ncomp > 1) {
                Style ss = color_has("statusline.suggestions") ? color_get("statusline.suggestions")
                         : color_has("statusline") ? color_get("statusline") : style_reverse(def_style);
                int sy = y - 1 - (GOPT(O_KEYMENU) ? 2 : 0), x = 0, k;
                scr_fill(0, sy, w, 1, ' ', ss);
                for (k = 0; k < pr->ncomp && x < w; k++) {
                    Style s2 = k == pr->comp_idx ? style_reverse(ss) : ss;
                    x += scr_puts(x, sy, pr->completions[k], s2, w - x);
                    if (x < w) scr_set(x++, sy, ' ', ss);
                }
            }
        } else {
            scr_show_cursor(mx < w ? mx : w - 1, y);
        }
        return;
    }
    if (E.msg) {
        Style es = color_has("error-message") ? color_get("error-message") : def_style;
        if (E.msg_error && !color_has("error-message")) {
            es.fg = 0;
            es.bg = 1;
        }
        scr_puts(0, y, E.msg, E.msg_error ? es : st, w);
    }
}
