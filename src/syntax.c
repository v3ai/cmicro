/*
 * syntax.c - micro-compatible syntax definitions and highlighter.
 *
 * Syntax files use micro's YAML format (patterns, regions with
 * start/end/skip, nested rules, include, limit-group).  The highlighting
 * algorithm follows micro's pkg/highlight: a line is processed left to
 * right; outside of a region we look for the earliest region start and
 * apply patterns before it; inside a region we look for its end (with skip
 * sequences masked out) or a nested region, whichever comes first.  The
 * region still open at the end of a line is carried to the next line.
 */
#include "syntax.h"
#include "conf.h"
#include "runtime.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- groups ---------------- */

static char **groups;
static Style *gstyles;
static int ngroups, cgroups;

int group_id(const char *name) {
    int i;
    if (!name || !*name) return 0;
    if (ngroups == 0) {
        cgroups = 64;
        groups = xmalloc(sizeof *groups * (size_t)cgroups);
        gstyles = xmalloc(sizeof *gstyles * (size_t)cgroups);
        groups[0] = xstrdup("");
        gstyles[0] = def_style;
        ngroups = 1;
    }
    for (i = 1; i < ngroups; i++)
        if (!strcmp(groups[i], name)) return i;
    if (ngroups == cgroups) {
        cgroups *= 2;
        groups = xrealloc(groups, sizeof *groups * (size_t)cgroups);
        gstyles = xrealloc(gstyles, sizeof *gstyles * (size_t)cgroups);
    }
    groups[ngroups] = xstrdup(name);
    gstyles[ngroups] = color_get(name);
    return ngroups++;
}

const char *group_name(int id) { return id > 0 && id < ngroups ? groups[id] : ""; }
int group_count(void) { return ngroups; }

void group_styles_refresh(void) {
    int i;
    if (ngroups == 0) group_id("default");
    gstyles[0] = def_style;
    for (i = 1; i < ngroups; i++) gstyles[i] = color_get(groups[i]);
}

Style group_style(int id) {
    if (id <= 0 || id >= ngroups) return def_style;
    return gstyles[id];
}

/* ---------------- syntax index ---------------- */

static Syntax **syntaxes;
static int nsyntax;
static Syntax *default_syntax;

static Regex *compile_or_null(const char *pat) {
    char *err = NULL;
    Regex *re;
    if (!pat) return NULL;
    re = re_compile(pat, 0, &err);
    free(err);
    return re;
}

void syntax_init(void) {
    char **names = runtime_list(RT_SYNTAX);
    int i;
    group_id("default");
    for (i = 0; names[i]; i++) {
        char *text = runtime_read(RT_SYNTAX, names[i]);
        CNode *root, *det;
        const char *ft;
        Syntax *s;
        if (!text) continue;
        root = yaml_parse(text, NULL);
        free(text);
        ft = cnode_str(root, "filetype");
        if (!ft) { cnode_free(root); continue; }
        s = xcalloc(1, sizeof *s);
        s->filetype = xstrdup(ft);
        s->file = xstrdup(names[i]);
        det = cnode_get(root, "detect");
        if (det) {
            const char *f = cnode_str(det, "filename");
            if (f && *f) s->fname = compile_or_null(f);
            s->header = compile_or_null(cnode_str(det, "header"));
            s->signature = compile_or_null(cnode_str(det, "signature"));
        }
        cnode_free(root);
        if (!strcmp(names[i], "default")) { default_syntax = s; continue; }
        syntaxes = xrealloc(syntaxes, sizeof *syntaxes * (size_t)(nsyntax + 1));
        syntaxes[nsyntax++] = s;
    }
    free_strv(names);
}

char **syntax_filetypes(void) {
    char **v = xmalloc(sizeof *v * (size_t)(nsyntax + 2));
    int i, n = 0, j;
    for (i = 0; i < nsyntax; i++) {
        int dup = 0;
        for (j = 0; j < n; j++) if (!strcmp(v[j], syntaxes[i]->filetype)) dup = 1;
        if (!dup) v[n++] = xstrdup(syntaxes[i]->filetype);
    }
    v[n++] = xstrdup("off");
    v[n] = NULL;
    return v;
}

Syntax *syntax_by_filetype(const char *ft) {
    int i;
    if (!ft) return NULL;
    for (i = 0; i < nsyntax; i++)
        if (!strcmp(syntaxes[i]->filetype, ft)) return syntaxes[i];
    if (default_syntax && !strcmp(ft, "unknown")) return default_syntax;
    return NULL;
}

Syntax *syntax_detect(const char *path, const char *firstline, const char **lines, int nlines,
                      int detectlimit) {
    Syntax **fm = xmalloc(sizeof *fm * (size_t)(nsyntax + 1));
    Syntax **hm = xmalloc(sizeof *hm * (size_t)(nsyntax + 1));
    int nf = 0, nh = 0, i, k;
    Syntax *res = NULL, **m;
    int nm;
    const char *base = path ? path_base(path) : "";
    for (i = 0; i < nsyntax; i++) {
        Syntax *s = syntaxes[i];
        if (path && *path && s->fname &&
            (re_match_str(s->fname, path) || re_match_str(s->fname, base))) {
            fm[nf++] = s;
        } else if (nf == 0 && s->header && firstline && re_match_str(s->header, firstline)) {
            hm[nh++] = s;
        }
    }
    m = nf ? fm : hm;
    nm = nf ? nf : nh;
    if (nm == 1) res = m[0];
    else if (nm > 1) {
        int limit = nlines;
        if (detectlimit > 0 && nlines > detectlimit) limit = detectlimit;
        for (i = 0; i < nm && !res; i++) {
            if (!m[i]->signature) continue;
            for (k = 0; k < limit; k++)
                if (re_match_str(m[i]->signature, lines[k])) { res = m[i]; break; }
        }
        if (!res) res = m[0];
    }
    free(fm);
    free(hm);
    return res ? res : default_syntax;
}

/* ---------------- rule parsing ---------------- */

static void rules_add_pattern(Rules *r, int group, Regex *re) {
    r->pats = xrealloc(r->pats, sizeof *r->pats * (size_t)(r->npats + 1));
    r->pats[r->npats].group = group;
    r->pats[r->npats].re = re;
    r->npats++;
}

static void rules_add_region(Rules *r, Region *reg) {
    r->regions = xrealloc(r->regions, sizeof *r->regions * (size_t)(r->nregions + 1));
    r->regions[r->nregions++] = reg;
}

static void copy_rules(const Rules *src, Rules *dst, Region *parent) {
    int i;
    for (i = 0; i < src->npats; i++) rules_add_pattern(dst, src->pats[i].group, src->pats[i].re);
    for (i = 0; i < src->nregions; i++) {
        Region *o = src->regions[i], *n = xcalloc(1, sizeof *n);
        n->group = o->group;
        n->limit_group = o->limit_group;
        n->start = o->start;
        n->end = o->end;
        n->skip = o->skip;
        n->parent = parent;
        copy_rules(&o->rules, &n->rules, n);
        rules_add_region(dst, n);
    }
}

static int load_depth;

static int parse_rules(Syntax *syn, CNode *list, Rules *out, Region *parent, char **err) {
    int i, k;
    if (!list || list->type != CN_LIST) return 0;
    for (i = 0; i < list->n; i++) {
        CNode *item = list->vals[i];
        if (item->type != CN_MAP) continue;
        for (k = 0; k < item->n; k++) {
            const char *key = item->keys[k];
            CNode *v = item->vals[k];
            if (!strcmp(key, "include")) {
                Syntax *inc;
                if (v->type != CN_STR) continue;
                inc = syntax_by_filetype(v->str);
                if (inc && inc != syn && load_depth < 8) {
                    load_depth++;
                    if (syntax_load(inc, NULL) == 0) copy_rules(&inc->rules, out, parent);
                    load_depth--;
                }
            } else if (v->type == CN_STR) {
                char *e = NULL;
                Regex *re = re_compile(v->str, 0, &e);
                if (!re) {
                    if (err && !*err) *err = xsprintf("%s: %s: %s", syn->file, key, e);
                    free(e);
                    continue;
                }
                rules_add_pattern(out, group_id(key), re);
            } else if (v->type == CN_MAP) {
                Region *r = xcalloc(1, sizeof *r);
                const char *s;
                char *e = NULL;
                r->group = group_id(key);
                s = cnode_str(v, "limit-group");
                r->limit_group = s ? group_id(s) : r->group;
                r->parent = parent;
                s = cnode_str(v, "start");
                if (s && *s) r->start = re_compile(s, 0, &e);
                free(e); e = NULL;
                s = cnode_str(v, "end");
                if (s && *s) r->end = re_compile(s, 0, &e);
                free(e); e = NULL;
                s = cnode_str(v, "skip");
                if (s && *s) r->skip = re_compile(s, 0, &e);
                free(e);
                if (!r->start || !r->end) {
                    if (err && !*err) *err = xsprintf("%s: bad region %s", syn->file, key);
                    re_free(r->start);
                    re_free(r->end);
                    re_free(r->skip);
                    free(r);
                    continue;
                }
                parse_rules(syn, cnode_get(v, "rules"), &r->rules, r, err);
                rules_add_region(out, r);
            }
        }
    }
    return 0;
}

int syntax_load(Syntax *s, char **err) {
    char *text;
    CNode *root;
    if (err) *err = NULL;
    if (!s) return -1;
    if (s->loaded) return s->failed ? -1 : 0;
    s->loaded = 1; /* also guards include cycles */
    text = runtime_read(RT_SYNTAX, s->file);
    if (!text) { s->failed = 1; return -1; }
    root = yaml_parse(text, NULL);
    free(text);
    parse_rules(s, cnode_get(root, "rules"), &s->rules, NULL, err);
    cnode_free(root);
    return 0;
}

/* ---------------- highlighting ---------------- */

/* Skip masks.  micro hides matches of a region's `skip` regex before
 * looking for its start/end.  Masks are computed once per line and reused
 * for later offsets as long as the offset doesn't fall inside a skip match
 * (for context-free skip regexes the masking from there on is identical). */
typedef struct {
    Regex *skip;
    int from, gen, cap;
    char *m;             /* masked copy of the line (valid from `from`) */
    unsigned char *in;   /* in[i]: i is strictly inside a skip match */
} SkipMask;

static SkipMask skm[16];
static int nskm, skm_gen;

static const char *masked_from(Regex *skip, const char *line, int off, int len, int *gen) {
    int i, slot = -1, pos = 0, n = len - off, caps[2];
    const char *sub = line + off;
    SkipMask *k;
    for (i = 0; i < nskm; i++) {
        if (skm[i].skip != skip) continue;
        slot = i;
        if (re_context_free(skip) && skm[i].from <= off && !skm[i].in[off]) {
            *gen = skm[i].gen;
            return skm[i].m + off;
        }
        break;
    }
    if (slot < 0) slot = nskm < 16 ? nskm++ : 0;
    k = &skm[slot];
    if (k->cap < len + 1) {
        k->cap = len + 64;
        k->m = xrealloc(k->m, (size_t)k->cap);
        k->in = xrealloc(k->in, (size_t)k->cap);
    }
    k->skip = skip;
    k->from = off;
    k->gen = ++skm_gen;
    memcpy(k->m + off, sub, (size_t)n);
    memset(k->in + off, 0, (size_t)n + 1);
    while (pos <= n && re_search(skip, sub, n, pos, caps, 1)) {
        if (caps[1] > caps[0]) {
            memset(k->m + off + caps[0], 1, (size_t)(caps[1] - caps[0]));
            if (caps[1] - caps[0] > 1) memset(k->in + off + caps[0] + 1, 1, (size_t)(caps[1] - caps[0] - 1));
            pos = caps[1];
        } else {
            pos = caps[1] < n ? utf8_next(sub, n, caps[1]) : n + 1;
        }
    }
    *gen = k->gen;
    return k->m + off;
}

/* find first match of re in s[off..len) treating it as its own string,
 * with matches of skip masked out.  Returns 1 and sets *ms,*me relative. */
static int find_index_gen(Regex *re, Regex *skip, const char *s, int off, int len, int *ms, int *me, int *gen) {
    const char *sub = s + off;
    int n = len - off, caps[2];
    *gen = 0;
    if (skip) sub = masked_from(skip, s, off, len, gen);
    if (!re_search(re, sub, n, 0, caps, 1)) return 0;
    *ms = caps[0];
    *me = caps[1];
    return 1;
}

static int find_index(Regex *re, Regex *skip, const char *s, int off, int len, int *ms, int *me) {
    int gen;
    return find_index_gen(re, skip, s, off, len, ms, me, &gen);
}

/* Per-line cache of region-start searches.  Without it, every region
   transition rescans the rest of the line for every region start, which is
   quadratic on long minified lines.  A context-free start regex has the
   same leftmost match from a later offset (or still none), provided the
   skip mask it was searched in is still valid there. */
typedef struct {
    Regex *re, *skip;
    int from, found, s, e, gen; /* absolute positions */
} StartCache;

static StartCache scache[64];
static int nscache;

static int find_start(Region *r, const char *line, int off, int len, int *ms, int *me) {
    int i, s = 0, e = 0, found, gen = 0;
    if (!re_context_free(r->start)) return find_index(r->start, r->skip, line, off, len, ms, me);
    for (i = 0; i < nscache; i++) {
        StartCache *c = &scache[i];
        if (c->re == r->start && c->skip == r->skip && c->from <= off) {
            int ok = 1;
            if (r->skip) {
                int g;
                masked_from(r->skip, line, off, len, &g);
                ok = g == c->gen;
            }
            if (ok && !c->found) return 0;
            if (ok && c->s >= off) {
                *ms = c->s - off;
                *me = c->e - off;
                return 1;
            }
            break;
        }
    }
    found = find_index_gen(r->start, r->skip, line, off, len, &s, &e, &gen);
    if (i == nscache && nscache < 64) nscache++;
    if (i < 64) {
        scache[i].re = r->start;
        scache[i].skip = r->skip;
        scache[i].from = off;
        scache[i].found = found;
        scache[i].s = s + off;
        scache[i].e = e + off;
        scache[i].gen = gen;
    }
    if (found) { *ms = s; *me = e; }
    return found;
}

static void fill(unsigned short *hl, int a, int b, int g) {
    for (; a < b; a++) hl[a] = (unsigned short)g;
}

/* apply all patterns over s[off..end) (as a standalone string) */
/* Per-line cache of pattern searches (same idea as StartCache): a
   context-free regex searched over line[..end) gives the same leftmost
   match at or after an absolute position, whatever the substring start. */
typedef struct {
    Regex *re;
    int end, from, found, s, e;
} PatCache;

static PatCache pcache[128];
static int npcache;

static int pat_search(Regex *re, const char *line, int off, int end, int pos, int *ms, int *me) {
    int i, caps[2], found, apos = off + pos;
    int cf = re_context_free(re);
    if (cf) {
        for (i = 0; i < npcache; i++) {
            PatCache *c = &pcache[i];
            if (c->re == re && c->end == end && c->from <= apos) {
                if (!c->found) return 0;
                if (c->s >= apos) {
                    *ms = c->s - off;
                    *me = c->e - off;
                    return 1;
                }
                break;
            }
        }
    } else {
        i = 128;
    }
    found = re_search(re, line + off, end - off, pos, caps, 1);
    if (cf) {
        if (i == npcache && npcache < 128) npcache++;
        if (i < 128) {
            pcache[i].re = re;
            pcache[i].end = end;
            pcache[i].from = apos;
            pcache[i].found = found;
            pcache[i].s = found ? caps[0] + off : 0;
            pcache[i].e = found ? caps[1] + off : 0;
        }
    }
    if (found) { *ms = caps[0]; *me = caps[1]; }
    return found;
}

/* apply all patterns over s[off..end) (as a standalone string); matches
   starting at or after limit_end (relative, -1 = none) are ignored */
static void apply_patterns(const Rules *r, const char *s, int off, int end, unsigned short *hl,
                           int limit_end, int only_group, int filter) {
    int i, n = end - off, ms, me;
    const char *sub = s + off;
    for (i = 0; i < r->npats; i++) {
        const Pattern *p = &r->pats[i];
        int pos = 0, prev_end = -1;
        if (filter && p->group != only_group) continue;
        while (pos <= n && pat_search(p->re, s, off, end, pos, &ms, &me)) {
            int accept = 1;
            if (limit_end >= 0 && ms >= limit_end) break; /* later matches are discarded anyway */
            if (me == ms) {
                if (ms == prev_end) accept = 0;
                pos = me < n ? utf8_next(sub, n, me) : n + 1;
            } else {
                pos = me;
            }
            prev_end = me;
            if (accept) fill(hl, off + ms, off + me, p->group);
        }
    }
}

Region *syntax_highlight_line(Syntax *syn, Region *cur, const char *line, int len,
                              unsigned short *hl) {
    int off = 0, iter = 0;
    nscache = 0;
    nskm = 0;
    npcache = 0;
    if (!syn) {
        if (hl) fill(hl, 0, len, 0);
        return NULL;
    }
    for (;;) {
        if (++iter > 2 * len + 64) {
            /* pathological definition (empty start/end matches): stop */
            return cur;
        }
        if (!cur) {
            int best = -1, bs = 0, be = 0, i, sublen = len - off;
            if (off >= len) return NULL;
            for (i = 0; i < syn->rules.nregions; i++) {
                int s, e;
                Region *r = syn->rules.regions[i];
                if (find_start(r, line, off, len, &s, &e) &&
                    (best < 0 || s < bs)) {
                    best = i; bs = s; be = e;
                }
            }
            if (best >= 0 && bs != sublen) {
                Region *r = syn->rules.regions[best];
                if (hl) {
                    fill(hl, off, off + bs, 0);
                    apply_patterns(&syn->rules, line, off, off + bs, hl, -1, 0, 0);
                    fill(hl, off + bs, off + be, r->limit_group);
                }
                off += be;
                cur = r;
                continue;
            }
            if (hl) {
                fill(hl, off, len, 0);
                apply_patterns(&syn->rules, line, off, len, hl, -1, 0, 0);
            }
            return NULL;
        } else {
            int es = 0, ee = 0, have_end, search_nesting = 1, sublen = len - off;
            int first_s = sublen, first_e = 0, i;
            Region *first = NULL;
            have_end = off <= len && find_index(cur->end, cur->skip, line, off, len, &es, &ee);
            if (have_end) {
                if (es == 0) search_nesting = 0;
                else { first_s = es; first_e = ee; }
            }
            if (search_nesting) {
                for (i = 0; i < cur->rules.nregions; i++) {
                    int s, e;
                    Region *r = cur->rules.regions[i];
                    if (find_start(r, line, off, len, &s, &e) && s < first_s) {
                        first = r; first_s = s; first_e = e;
                    }
                }
            }
            if (first && first_s != sublen) {
                if (hl) {
                    fill(hl, off, off + first_s, cur->group);
                    fill(hl, off + first_s, off + first_e, first->limit_group);
                }
                off += first_e;
                cur = first;
                continue;
            }
            if (hl) {
                fill(hl, off, have_end ? off + ee : len, cur->group);
                if (search_nesting)
                    apply_patterns(&cur->rules, line, off, len, hl, have_end ? es : -1,
                                   cur->limit_group, cur->group != cur->limit_group);
            }
            if (have_end) {
                if (hl) fill(hl, off + es, off + ee, cur->limit_group);
                off += ee;
                cur = cur->parent;
                continue;
            }
            return cur;
        }
    }
}
