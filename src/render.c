/*
 * render.c - drawing buffer panes, status lines, the tab bar, the info
 * bar (messages / prompts) and the key menu.
 */
#include "micro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *keydisplay[] = {
    "^Q Quit, ^S Save, ^O Open, ^G Help, ^E Command Bar, ^K Cut Line",
    "^F Find, ^Z Undo, ^Y Redo, ^A Select All, ^D Duplicate Line, ^T New Tab",
};

int cm_layout_top(void) {
    return (cm_E.ntabs > 1 || GOPT(O_TABALWAYS)) ? 1 : 0;
}

int cm_layout_bottom_reserved(void) {
    int n = 0;
    if (GOPT(O_INFOBAR) || cm_prompt_active()) n++;
    if (GOPT(O_KEYMENU)) n += 2;
    return n;
}

static Style bg_from_fg(Style s, const char *group) {
    if (cm_color_has(group)) {
        Style c = cm_color_get(group);
        s.bg = c.fg;
    }
    return s;
}

static int in_selection(Pane *p, Loc l) {
    int i;
    for (i = 0; i < p->ncur; i++) {
        Cursor *c = &p->cur[i];
        if (cm_cur_has_sel(c) && cm_loc_cmp(l, c->sel[0]) >= 0 && cm_loc_cmp(l, c->sel[1]) < 0) return 1;
    }
    return 0;
}

static int other_cursor_at(Pane *p, Loc l, int active) {
    int i;
    for (i = 0; i < p->ncur; i++) {
        if (i == p->active && active) continue;
        if (cm_loc_cmp(p->cur[i].loc, l) == 0 && !cm_cur_has_sel(&p->cur[i])) return 1;
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
            if (!e) { cm_str_puts(out, s); break; }
            n = (size_t)(e - (s + 2));
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, s + 2, n);
            name[n] = 0;
            s = e + 1;
            if (!strcmp(name, "filename")) {
                const char *nm = *b->name ? b->name : (*b->path ? b->path : "No name");
                cm_str_puts(out, BOPT(b, O_BASENAME) && *nm ? cm_path_base(nm) : nm);
            } else if (!strcmp(name, "modified")) {
                if (cm_buf_modified(b)) cm_str_puts(out, "+ ");
                else if (BOPT(b, O_READONLY)) cm_str_puts(out, "[ro] ");
            } else if (!strcmp(name, "overwrite")) {
                if (p->overwrite && !BOPT(b, O_READONLY)) cm_str_puts(out, "[ovwr] ");
            } else if (!strcmp(name, "line")) {
                cm_str_printf(out, "%d", c->loc.y + 1);
            } else if (!strcmp(name, "col")) {
                cm_str_printf(out, "%d", cm_utf8_count(b->lines[c->loc.y].s, c->loc.x) + 1);
            } else if (!strcmp(name, "lines") || !strcmp(name, "status.lines")) {
                cm_str_printf(out, "%d", b->nlines);
            } else if (!strcmp(name, "percentage")) {
                cm_str_printf(out, "%d", (c->loc.y + 1) * 100 / b->nlines);
            } else if (!strcmp(name, "status.paste")) {
                if (GOPT(O_PASTE)) cm_str_puts(out, "PASTE ");
            } else if (!strcmp(name, "status.vcol")) {
                cm_str_printf(out, "%d", cm_pane_visual_x(p, c->loc.y, c->loc.x));
            } else if (!strcmp(name, "status.bytes") || !strcmp(name, "status.size")) {
                long sum = 0;
                int y;
                for (y = 0; y < b->nlines; y++) sum += b->lines[y].len + (y + 1 < b->nlines);
                if (!strcmp(name, "status.bytes")) cm_str_printf(out, "%ld", sum);
                else if (sum < 1024) cm_str_printf(out, "%ldB", sum);
                else {
                    /* one decimal in integer arithmetic (no float formatting),
                       rounding ties to even like printf("%.1f") in micro */
                    long unit = sum < 1024L * 1024L ? 1024L : 1024L * 1024L;
                    long rem = (sum % unit) * 10;
                    long tenths = (sum / unit) * 10 + rem / unit;
                    rem %= unit;
                    if (rem * 2 > unit || (rem * 2 == unit && tenths % 2 != 0)) tenths++;
                    cm_str_printf(out, "%ld.%ld%s", tenths / 10, tenths % 10, unit == 1024L ? "KB" : "MB");
                }
            } else if (cm_starts_with(name, "opt:")) {
                int id = cm_opt_find(name + 4);
                if (id >= 0) {
                    char *v = cm_opt_to_string(id, id < O_FIRST_GLOBAL ? &b->opts[id] : &cm_gopts[id]);
                    cm_str_puts(out, v);
                    free(v);
                }
            } else if (cm_starts_with(name, "bind:")) {
                const char *k = cm_binding_key_for_action(name + 5);
                cm_str_puts(out, k ? k : "null");
            }
        } else {
            cm_str_putc(out, *s++);
        }
    }
}

static void render_status(Pane *p, int active) {
    Str l, r;
    Style st = cm_color_has("statusline") ? cm_color_get("statusline") : cm_style_reverse(cm_def_style);
    int y = p->y + p->h - 1, lw, rw, x;
    if (!active && cm_color_has("statusline.inactive")) st = cm_color_get("statusline.inactive");
    cm_str_init(&l);
    cm_str_init(&r);
    cm_str_reserve(&l, 0);
    cm_str_reserve(&r, 0);
    status_subst(p, BOPTS(p->buf, O_STATUSFORMATL), &l);
    status_subst(p, BOPTS(p->buf, O_STATUSFORMATR), &r);
    cm_scr_fill(p->x, y, p->w, 1, ' ', st);
    lw = cm_scr_puts(p->x, y, l.s, st, p->w);
    {
        int rwid = 0, i = 0;
        while (i < r.len) {
            int cp, n = cm_utf8_decode(r.s + i, r.len - i, &cp);
            rwid += cm_char_width(cp);
            i += n;
        }
        rw = rwid;
    }
    /* like micro: left text wins, right text fills whatever is left of its slot */
    x = p->x + p->w - rw;
    if (x < p->x + lw) x = p->x + lw;
    if (x < p->x + p->w) cm_scr_puts(x, y, r.s, st, p->x + p->w - x);
    cm_str_free(&l);
    cm_str_free(&r);
}

/* ---------------- pane ---------------- */

void cm_render_pane(Pane *p, int active) {
    Buffer *b = p->buf;
    int gw = cm_pane_gutter_width(p), tw = cm_pane_text_width(p), th = cm_pane_text_height(p);
    int line = p->first_line, row = p->first_row, r;
    int softwrap = BOPT(b, O_SOFTWRAP);
    int ts = BOPT(b, O_TABSIZE);
    int syntax = BOPT(b, O_SYNTAX) && b->syn;
    Cursor *ac = ACUR(p);
    int curline = ac->loc.y;
    int cursorline = BOPT(b, O_CURSORLINE) && active && !cm_cur_has_sel(ac);
    int colorcol = BOPT(b, O_COLORCOLUMN);
    int ndig = cm_digits(b->nlines);
    int relruler = BOPT(b, O_RELATIVERULER);
    int hlsearch = BOPT(b, O_HLSEARCH) && b->hl_search && b->last_search && *b->last_search;
    Loc brace, match;
    int have_brace = 0;
    Style lnst = cm_color_get("line-number"), clnst = cm_color_get("current-line-number");
    Style ind_st = cm_color_get("indent-char");
    const char *ich = BOPTS(b, O_INDENTCHAR);
    int ichcp = ' ';
    unsigned char *smask = NULL;
    int smask_cap = 0;
    int cursor_drawn = 0;
    char *showchars = BOPTS(b, O_SHOWCHARS);
    int show_tab = 0, show_space = 0, show_twsp = 0;

    if (ich && *ich) cm_utf8_decode(ich, (int)strlen(ich), &ichcp);
    if (showchars && *showchars) {
        /* micro's showchars: "tab=>,space=.,ispace=.." (subset) */
        const char *t;
        if ((t = strstr(showchars, "tab="))) cm_utf8_decode(t + 4, (int)strlen(t + 4), &show_tab);
        if ((t = strstr(showchars, "space="))) cm_utf8_decode(t + 6, (int)strlen(t + 6), &show_space);
        if ((t = strstr(showchars, "trailing="))) cm_utf8_decode(t + 9, (int)strlen(t + 9), &show_twsp);
    }
    if (BOPT(b, O_MATCHBRACE) && active)
        have_brace = cm_find_matching_brace(b, ac->loc, BOPT(b, O_MATCHBRACELEFT), &brace, &match);

    for (r = 0; r < th; r++) {
        int sy = p->y + r;
        int nrows;
        if (line >= b->nlines) {
            cm_scr_fill(p->x, sy, p->w, 1, ' ', cm_def_style);
            continue;
        }
        if (syntax) cm_buf_ensure_hl(b, line);
        nrows = softwrap ? cm_pane_line_rows(p, line) : 1;
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
            cm_scr_puts(p->x, sy, num, gs, gw);
        }
        /* text */
        {
            Line *ln = &b->lines[line];
            int bstart = 0, bend = ln->len, vstart = 0;
            int i, vx, x0 = p->x + gw;
            int trail = ln->len;
            Style base_line = cm_def_style;
            if (cursorline && line == curline) base_line = bg_from_fg(cm_def_style, "cursor-line");
            while (trail > 0 && (ln->s[trail - 1] == ' ' || ln->s[trail - 1] == '\t')) trail--;
            if (softwrap) {
                Loc a = cm_pane_row_to_loc(p, line, row, 0);
                int rr, rvx;
                (void)rr;
                bstart = a.x;
                /* compute row start column and end byte */
                cm_pane_loc_to_row(p, a, &rr, &rvx);
                vstart = cm_pane_visual_x(p, line, a.x) - rvx;
                if (row + 1 < nrows) {
                    Loc nb = cm_pane_row_to_loc(p, line, row + 1, 0);
                    bend = nb.x;
                    /* cm_pane_row_to_loc returns the first char of the next row */
                }
            }
            if (hlsearch) {
                if (smask_cap < ln->len + 1) {
                    smask_cap = ln->len + 64;
                    smask = cm_xrealloc(smask, (size_t)smask_cap);
                }
                memset(smask, 0, (size_t)ln->len + 1);
                cm_search_highlight_ranges(p, line, smask, ln->len);
            }
            /* clear row */
            cm_scr_fill(x0, sy, tw, 1, ' ', base_line);
            vx = softwrap ? vstart : 0;
            i = softwrap ? bstart : 0;
            if (!softwrap) {
                /* skip to horizontally visible part quickly (still need vx for tabs) */
            }
            while (i < bend) {
                int cp, n = cm_utf8_decode(ln->s + i, ln->len - i, &cp);
                int w = cp == '\t' ? ts - (vx % ts) : cm_char_width(cp);
                int col = softwrap ? vx - vstart : vx - p->left;
                Style st = syntax && ln->hl ? cm_group_style(ln->hl[i]) : cm_def_style;
                Loc here = cm_loc_make(i, line);
                if (cursorline && line == curline) st = bg_from_fg(st, "cursor-line");
                if (colorcol > 0 && vx == colorcol) st = bg_from_fg(st, "color-column");
                if (hlsearch && smask[i]) st = cm_color_get("hlsearch");
                if (BOPT(b, O_HLTRAILINGWS) && i >= trail && !(active && line == curline))
                    st = cm_color_has("trailingws") ? cm_color_get("trailingws") : st;
                if (BOPT(b, O_HLTABERRORS) && cp == '\t' && BOPT(b, O_TABSTOSPACES))
                    st = cm_color_has("tab-error") ? cm_color_get("tab-error") : st;
                if (in_selection(p, here))
                    st = cm_color_has("selection") ? cm_color_get("selection") : cm_style_reverse(st);
                if (have_brace && (cm_loc_cmp(here, brace) == 0 || cm_loc_cmp(here, match) == 0)) {
                    if (!strcmp(BOPTS(b, O_MATCHBRACESTYLE), "highlight") && cm_color_has("match-brace"))
                        st = cm_color_get("match-brace");
                    else
                        st.attr |= ATTR_UNDERLINE;
                }
                if (other_cursor_at(p, here, 1) && p->ncur > 1) st = cm_style_reverse(st);
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
                                cm_scr_set(x0 + col + k, sy, ch, s2);
                            } else {
                                cm_scr_set(x0 + col + k, sy, ' ', st);
                            }
                        }
                    } else if (col >= 0 && col + w <= tw) {
                        int ch = cp;
                        if (cp == ' ' && show_space && i < trail) ch = show_space;
                        if (cp == ' ' && show_twsp && i >= trail) ch = show_twsp;
                        cm_scr_set(x0 + col, sy, ch, st);
                    } else {
                        int k;
                        for (k = 0; k < w; k++)
                            if (col + k >= 0 && col + k < tw) cm_scr_set(x0 + col + k, sy, ' ', st);
                    }
                }
                if (active && cm_loc_cmp(here, ac->loc) == 0 && col >= 0 && col < tw) {
                    cm_scr_show_cursor(x0 + col, sy);
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
                Loc eol = cm_loc_make(ln->len, line);
                if (bend == ln->len && col >= 0 && col < tw) {
                    Style st = base_line;
                    if (line + 1 < b->nlines && in_selection(p, eol))
                        st = cm_color_has("selection") ? cm_color_get("selection") : cm_style_reverse(cm_def_style);
                    if (other_cursor_at(p, eol, 1) && p->ncur > 1) st = cm_style_reverse(st);
                    cm_scr_set(x0 + col, sy, ' ', st);
                    if (active && cm_loc_cmp(ac->loc, eol) == 0) {
                        cm_scr_show_cursor(x0 + col, sy);
                        cursor_drawn = 1;
                    }
                }
                if (colorcol > 0) {
                    int cc = colorcol - (softwrap ? vstart : p->left);
                    if (cc >= 0 && cc < tw && colorcol >= vx) {
                        Style st = cm_scr_get_style(x0 + cc, sy);
                        st = bg_from_fg(st, "color-column");
                        cm_scr_set_style(x0 + cc, sy, st);
                    }
                }
            }
        }
        /* advance */
        if (row + 1 < nrows) row++;
        else { line++; row = 0; }
    }
    free(smask);
    if (active && !cursor_drawn) cm_scr_hide_cursor();
    if (BOPT(b, O_SCROLLBAR) && b->nlines > th) {
        int pos = (int)((long)p->first_line * th / b->nlines);
        int sz = th * th / b->nlines;
        int k;
        const char *sbc = GOPTS(O_SCROLLBARCHAR);
        int sbcp = '|';
        if (sbc && *sbc) cm_utf8_decode(sbc, (int)strlen(sbc), &sbcp);
        if (sz < 1) sz = 1;
        for (k = pos; k < pos + sz && k < th; k++)
            cm_scr_set(p->x + p->w - 1, p->y + k, sbcp, cm_color_has("scrollbar") ? cm_color_get("scrollbar") : cm_def_style);
    }
    if (BOPT(b, O_STATUSLINE)) render_status(p, active);
}

/* ---------------- tab bar ---------------- */

void cm_render_tabbar(void) {
    int x = 0, i, w = cm_scr_w();
    int rev = GOPT(O_TABREVERSE), hl = GOPT(O_TABHIGHLIGHT);
    int charhl = (rev || hl) && !(rev && hl);
    Style base, active_st;
#define TABSTYLE(reverse, out_base, out_act)                               \
    do {                                                                   \
        out_base = cm_color_has("tabbar") ? cm_color_get("tabbar")               \
                                       : ((reverse) ? cm_style_reverse(cm_def_style) : cm_def_style); \
        out_act = cm_color_has("tabbar.active") ? cm_color_get("tabbar.active") : out_base; \
    } while (0)
    if (!cm_layout_top()) return;
    for (i = 0; i < cm_E.ntabs && x < w; i++) {
        Pane *p = cm_E.tabs[i]->active;
        const char *nm = *p->buf->name ? p->buf->name : (*p->buf->path ? p->buf->path : "No name");
        int act = i == cm_E.curtab;
        TABSTYLE(charhl, base, active_st);
        cm_scr_set(x++, 0, act ? '[' : ' ', act ? active_st : base);
        x += cm_scr_puts(x, 0, nm, act ? active_st : base, w - x > 0 ? w - x : 0);
        if (cm_buf_modified(p->buf)) x += cm_scr_puts(x, 0, " +", act ? active_st : base, w - x > 0 ? w - x : 0);
        cm_scr_set(x++, 0, act ? ']' : ' ', act ? active_st : base);
        TABSTYLE(rev, base, active_st);
        cm_scr_set(x++, 0, ' ', act ? active_st : base);
        cm_scr_set(x++, 0, ' ', act ? active_st : base);
    }
    TABSTYLE(rev, base, active_st);
    (void)active_st;
    while (x < w) cm_scr_set(x++, 0, ' ', base);
#undef TABSTYLE
}

/* ---------------- key menu / info bar ---------------- */

void cm_render_keymenu(void) {
    int i, h = cm_scr_h();
    int infoh = (GOPT(O_INFOBAR) || cm_prompt_active()) ? 1 : 0;
    if (!GOPT(O_KEYMENU)) return;
    for (i = 0; i < 2; i++) {
        int y = h - infoh - 2 + i;
        cm_scr_fill(0, y, cm_scr_w(), 1, ' ', cm_def_style);
        cm_scr_puts(0, y, keydisplay[i], cm_def_style, cm_scr_w());
    }
}

void cm_render_infobar(void) {
    int y = cm_scr_h() - 1, w = cm_scr_w();
    Prompt *pr = &cm_E.prompt;
    Style st = cm_color_has("message") ? cm_color_get("message") : cm_def_style;
    if (!GOPT(O_INFOBAR) && !cm_prompt_active()) return;
    cm_scr_fill(0, y, w, 1, ' ', cm_def_style);
    if (pr->kind == PROMPT_TEXT || pr->kind == PROMPT_YN || pr->kind == PROMPT_RAW) {
        int mx = cm_scr_puts(0, y, pr->msg ? pr->msg : "", st, w);
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
                int cp, n = cm_utf8_decode(s + i, len - i, &cp);
                cvx += cm_char_width(cp);
                i += n;
            }
            if (avail < 1) avail = 1;
            if (cvx < pr->left) pr->left = cvx;
            if (cvx >= pr->left + avail) pr->left = cvx - avail + 1;
            for (i = 0; i < len;) {
                int cp, n = cm_utf8_decode(s + i, len - i, &cp);
                int cw = cm_char_width(cp);
                Style cs = cm_def_style;
                if (sa >= 0 && i >= sa && i < sb)
                    cs = cm_color_has("selection") ? cm_color_get("selection") : cm_style_reverse(cm_def_style);
                if (vx >= pr->left && vx - pr->left + cw <= avail) cm_scr_set(mx + vx - pr->left, y, cp, cs);
                vx += cw;
                i += n;
            }
            cm_scr_show_cursor(mx + cvx - pr->left, y);
            /* completion suggestions */
            if (pr->ncomp > 1) {
                Style ss = cm_color_has("statusline.suggestions") ? cm_color_get("statusline.suggestions")
                         : cm_color_has("statusline") ? cm_color_get("statusline") : cm_style_reverse(cm_def_style);
                int sy = y - 1 - (GOPT(O_KEYMENU) ? 2 : 0), x = 0, k;
                cm_scr_fill(0, sy, w, 1, ' ', ss);
                for (k = 0; k < pr->ncomp && x < w; k++) {
                    Style s2 = k == pr->comp_idx ? cm_style_reverse(ss) : ss;
                    x += cm_scr_puts(x, sy, pr->completions[k], s2, w - x);
                    if (x < w) cm_scr_set(x++, sy, ' ', ss);
                }
            }
        } else {
            cm_scr_show_cursor(mx < w ? mx : w - 1, y);
        }
        return;
    }
    if (cm_E.msg) {
        Style es = cm_color_has("error-message") ? cm_color_get("error-message") : cm_def_style;
        if (cm_E.msg_error && !cm_color_has("error-message")) {
            es.fg = 0;
            es.bg = 1;
        }
        cm_scr_puts(0, y, cm_E.msg, cm_E.msg_error ? es : st, w);
    }
}
