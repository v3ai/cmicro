/*
 * render.c - drawing buffer panes, status lines, the tab bar, the info
 * bar (messages / prompts) and the key menu.
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

static Style bg_from_fg(Style s, const char *group) {
    if (color_has(group)) {
        Style c = color_get(group);
        s.bg = c.fg;
    }
    return s;
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

/* ---------------- status line ---------------- */

static void status_subst(Pane *p, const char *fmt, Str *out) {
    Buffer *b = p->buf;
    Cursor *c = ACUR(p);
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
                str_printf(out, "%d", c->loc.y + 1);
            } else if (!strcmp(name, "col")) {
                str_printf(out, "%d", utf8_count(b->lines[c->loc.y].s, c->loc.x) + 1);
            } else if (!strcmp(name, "lines") || !strcmp(name, "status.lines")) {
                str_printf(out, "%d", b->nlines);
            } else if (!strcmp(name, "percentage")) {
                str_printf(out, "%d", (c->loc.y + 1) * 100 / b->nlines);
            } else if (!strcmp(name, "status.paste")) {
                if (GOPT(O_PASTE)) str_puts(out, "PASTE ");
            } else if (!strcmp(name, "status.vcol")) {
                str_printf(out, "%d", pane_visual_x(p, c->loc.y, c->loc.x));
            } else if (!strcmp(name, "status.bytes") || !strcmp(name, "status.size")) {
                long total = 0;
                int y;
                for (y = 0; y < b->nlines; y++) total += b->lines[y].len + (y + 1 < b->nlines);
                if (!strcmp(name, "status.bytes")) str_printf(out, "%ld", total);
                else if (total < 1024) str_printf(out, "%ldB", total);
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
            str_putc(out, *s++);
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
    {
        int rwid = 0, i = 0;
        while (i < r.len) {
            int cp, n = utf8_decode(r.s + i, r.len - i, &cp);
            rwid += char_width(cp);
            i += n;
        }
        rw = rwid;
    }
    /* like micro: left text wins, right text fills whatever is left of its slot */
    x = p->x + p->w - rw;
    if (x < p->x + lw) x = p->x + lw;
    if (x < p->x + p->w) scr_puts(x, y, r.s, st, p->x + p->w - x);
    str_free(&l);
    str_free(&r);
}

/* ---------------- pane ---------------- */

void render_pane(Pane *p, int active) {
    Buffer *b = p->buf;
    int gw = pane_gutter_width(p), tw = pane_text_width(p), th = pane_text_height(p);
    int line = p->top, row = p->toprow, r;
    int softwrap = BOPT(b, O_SOFTWRAP);
    int ts = BOPT(b, O_TABSIZE);
    int syntax = BOPT(b, O_SYNTAX) && b->syn;
    Cursor *ac = ACUR(p);
    int curline = ac->loc.y;
    int cursorline = BOPT(b, O_CURSORLINE) && active && !cur_has_sel(ac);
    int colorcol = BOPT(b, O_COLORCOLUMN);
    int ndig = digits(b->nlines);
    int relruler = BOPT(b, O_RELATIVERULER);
    int hlsearch = BOPT(b, O_HLSEARCH) && b->hl_search && b->last_search && *b->last_search;
    Loc brace, match;
    int have_brace = 0;
    Style lnst = color_get("line-number"), clnst = color_get("current-line-number");
    Style ind_st = color_get("indent-char");
    const char *ich = BOPTS(b, O_INDENTCHAR);
    int ichcp = ' ';
    unsigned char *smask = NULL;
    int smask_cap = 0;
    int cursor_drawn = 0;
    char *showchars = BOPTS(b, O_SHOWCHARS);
    int show_tab = 0, show_space = 0, show_twsp = 0;

    if (ich && *ich) utf8_decode(ich, (int)strlen(ich), &ichcp);
    if (showchars && *showchars) {
        /* micro's showchars: "tab=>,space=.,ispace=.." (subset) */
        const char *t;
        if ((t = strstr(showchars, "tab="))) utf8_decode(t + 4, (int)strlen(t + 4), &show_tab);
        if ((t = strstr(showchars, "space="))) utf8_decode(t + 6, (int)strlen(t + 6), &show_space);
        if ((t = strstr(showchars, "trailing="))) utf8_decode(t + 9, (int)strlen(t + 9), &show_twsp);
    }
    if (BOPT(b, O_MATCHBRACE) && active)
        have_brace = find_matching_brace(b, ac->loc, BOPT(b, O_MATCHBRACELEFT), &brace, &match);

    for (r = 0; r < th; r++) {
        int sy = p->y + r;
        int nrows;
        if (line >= b->nlines) {
            scr_fill(p->x, sy, p->w, 1, ' ', def_style);
            continue;
        }
        if (syntax) buf_ensure_hl(b, line);
        nrows = softwrap ? pane_line_rows(p, line) : 1;
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
            Style base_line = def_style;
            if (cursorline && line == curline) base_line = bg_from_fg(def_style, "cursor-line");
            while (trail > 0 && (ln->s[trail - 1] == ' ' || ln->s[trail - 1] == '\t')) trail--;
            if (softwrap) {
                Loc a = pane_row_to_loc(p, line, row, 0);
                int rr, rvx;
                (void)rr;
                bstart = a.x;
                /* compute row start column and end byte */
                pane_loc_to_row(p, a, &rr, &rvx);
                vstart = pane_visual_x(p, line, a.x) - rvx;
                if (row + 1 < nrows) {
                    Loc nb = pane_row_to_loc(p, line, row + 1, 0);
                    bend = nb.x;
                    /* pane_row_to_loc returns the first char of the next row */
                }
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
            vx = softwrap ? vstart : 0;
            i = softwrap ? bstart : 0;
            if (!softwrap) {
                /* skip to horizontally visible part quickly (still need vx for tabs) */
            }
            while (i < bend) {
                int cp, n = utf8_decode(ln->s + i, ln->len - i, &cp);
                int w = cp == '\t' ? ts - (vx % ts) : char_width(cp);
                int col = softwrap ? vx - vstart : vx - p->left;
                Style st = syntax && ln->hl ? group_style(ln->hl[i]) : def_style;
                Loc here = loc_make(i, line);
                if (cursorline && line == curline) st = bg_from_fg(st, "cursor-line");
                if (colorcol > 0 && vx == colorcol) st = bg_from_fg(st, "color-column");
                if (hlsearch && smask[i]) st = color_get("hlsearch");
                if (BOPT(b, O_HLTRAILINGWS) && i >= trail && !(active && line == curline))
                    st = color_has("trailingws") ? color_get("trailingws") : st;
                if (BOPT(b, O_HLTABERRORS) && cp == '\t' && BOPT(b, O_TABSTOSPACES))
                    st = color_has("tab-error") ? color_get("tab-error") : st;
                if (in_selection(p, here))
                    st = color_has("selection") ? color_get("selection") : style_reverse(st);
                if (have_brace && (loc_cmp(here, brace) == 0 || loc_cmp(here, match) == 0)) {
                    if (!strcmp(BOPTS(b, O_MATCHBRACESTYLE), "highlight") && color_has("match-brace"))
                        st = color_get("match-brace");
                    else
                        st.attr |= ATTR_UNDERLINE;
                }
                if (other_cursor_at(p, here, 1) && p->ncur > 1) st = style_reverse(st);
                if (col + w > 0 && col < tw) {
                    if (cp == '\t') {
                        int k;
                        for (k = 0; k < w; k++) {
                            if (col + k < 0 || col + k >= tw) continue;
                            if (k == 0) {
                                int ch = show_tab ? show_tab : ichcp;
                                Style s2 = st;
                                if (ch != ' ' && !in_selection(p, here)) {
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
                if (active && loc_cmp(here, ac->loc) == 0 && col >= 0 && col < tw) {
                    scr_show_cursor(x0 + col, sy);
                    cursor_drawn = 1;
                }
                vx += w;
                i += n;
                if (!softwrap && vx - p->left >= tw) {
                    /* rest of line is off screen, but the cursor may be at EOL beyond */
                    if (!(active && line == curline)) break;
                }
            }
            /* end of line cell */
            {
                int col = softwrap ? vx - vstart : vx - p->left;
                Loc eol = loc_make(ln->len, line);
                if (bend == ln->len && col >= 0 && col < tw) {
                    Style st = base_line;
                    if (line + 1 < b->nlines && in_selection(p, eol))
                        st = color_has("selection") ? color_get("selection") : style_reverse(def_style);
                    if (other_cursor_at(p, eol, 1) && p->ncur > 1) st = style_reverse(st);
                    scr_set(x0 + col, sy, ' ', st);
                    if (active && loc_cmp(ac->loc, eol) == 0) {
                        scr_show_cursor(x0 + col, sy);
                        cursor_drawn = 1;
                    }
                }
                if (colorcol > 0) {
                    int cc = colorcol - (softwrap ? vstart : p->left);
                    if (cc >= 0 && cc < tw && colorcol >= vx) {
                        Style st = scr_get_style(x0 + cc, sy);
                        st = bg_from_fg(st, "color-column");
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
    if (active && !cursor_drawn) scr_hide_cursor();
    if (BOPT(b, O_SCROLLBAR) && b->nlines > th) {
        int pos = (int)((long)p->top * th / b->nlines);
        int sz = th * th / b->nlines;
        int k;
        const char *sbc = GOPTS(O_SCROLLBARCHAR);
        int sbcp = '|';
        if (sbc && *sbc) utf8_decode(sbc, (int)strlen(sbc), &sbcp);
        if (sz < 1) sz = 1;
        for (k = pos; k < pos + sz && k < th; k++)
            scr_set(p->x + p->w - 1, p->y + k, sbcp, color_has("scrollbar") ? color_get("scrollbar") : def_style);
    }
    if (BOPT(b, O_STATUSLINE)) render_status(p, active);
}

/* ---------------- tab bar ---------------- */

void render_tabbar(void) {
    int x = 0, i, w = scr_w();
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
    for (i = 0; i < E.ntabs && x < w; i++) {
        Pane *p = E.tabs[i]->active;
        const char *nm = *p->buf->name ? p->buf->name : (*p->buf->path ? p->buf->path : "No name");
        int act = i == E.curtab;
        TABSTYLE(charhl, base, active_st);
        scr_set(x++, 0, act ? '[' : ' ', act ? active_st : base);
        x += scr_puts(x, 0, nm, act ? active_st : base, w - x > 0 ? w - x : 0);
        if (buf_modified(p->buf)) x += scr_puts(x, 0, " +", act ? active_st : base, w - x > 0 ? w - x : 0);
        scr_set(x++, 0, act ? ']' : ' ', act ? active_st : base);
        TABSTYLE(rev, base, active_st);
        scr_set(x++, 0, ' ', act ? active_st : base);
        scr_set(x++, 0, ' ', act ? active_st : base);
    }
    TABSTYLE(rev, base, active_st);
    (void)active_st;
    while (x < w) scr_set(x++, 0, ' ', base);
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
            int len = pr->text.len;
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
