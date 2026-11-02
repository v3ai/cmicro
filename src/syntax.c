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
static int *gtab;     /* open-addressing name -> id hash (0 = empty) */
static size_t gtcap;  /* power of two, > 2 * ngroups */

/* ids are stored per byte as unsigned short (Line.hl) */
#define MAX_GROUPS 65535

static size_t gname_hash(const char *s) {
    size_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void gtab_put(int id) {
    size_t i = gname_hash(groups[id]) & (gtcap - 1);
    while (gtab[i]) i = (i + 1) & (gtcap - 1);
    gtab[i] = id;
}

int group_id(const char *name) {
    size_t h;
    int id;
    if (!name || !*name) return 0;
    if (ngroups == 0) {
        cgroups = 64;
        groups = xmalloc(sizeof *groups * (size_t)cgroups);
        gstyles = xmalloc(sizeof *gstyles * (size_t)cgroups);
        groups[0] = xstrdup("");
        gstyles[0] = def_style;
        ngroups = 1;
        gtcap = 256;
        gtab = xcalloc(gtcap, sizeof *gtab);
    }
    for (h = gname_hash(name) & (gtcap - 1); (id = gtab[h]) != 0; h = (h + 1) & (gtcap - 1))
        if (!strcmp(groups[id], name)) return id;
    if (ngroups >= MAX_GROUPS) return 0;
    if (ngroups == cgroups) {
        cgroups *= 2;
        groups = xrealloc(groups, sizeof *groups * (size_t)cgroups);
        gstyles = xrealloc(gstyles, sizeof *gstyles * (size_t)cgroups);
    }
    groups[ngroups] = xstrdup(name);
    gstyles[ngroups] = color_get(name);
    id = ngroups++;
    if (2 * (size_t)ngroups >= gtcap) {
        int k;
        gtcap *= 2;
        free(gtab);
        gtab = xcalloc(gtcap, sizeof *gtab);
        for (k = 1; k < ngroups; k++) gtab_put(k);
    } else {
        gtab[h] = id;
    }
    return id;
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
        /* only filetype and detect are needed here; the rules (most of
           the file) are walked but not built, and parsed by syntax_load */
        root = yaml_parse_skip(text, "rules", NULL);
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
            for (k = 0; k < limit && lines; k++)
                if (lines[k] && re_match_str(m[i]->signature, lines[k])) { res = m[i]; break; }
        }
        if (!res) res = m[0];
    }
    free(fm);
    free(hm);
    return res ? res : default_syntax;
}

/* ---------------- search prefilter ----------------
 *
 * Nearly all highlighting time is spent in re_search() trying every start
 * position of a line for every pattern.  For most rules a match can only
 * begin with a few bytes ("\b(if|else)\b" with 'i' or 'e', an operator
 * class with punctuation), and often only at a word start.  The regex
 * source is analysed once, conservatively, into
 *   ok[b]: a non-empty match may begin with byte b (all bytes >= 0x80
 *          are always allowed: multi-byte characters aren't analysed),
 *   wb:    every match is preceded by a \b at its start position,
 * and searches first skip positions that cannot start a match.  Patterns
 * that can match the empty string, use flags, \Q, \p or anything not
 * understood get no filter.  Skipped positions are only ASCII bytes, which
 * are always character boundaries, so the leftmost match is unchanged. */

struct ReFilter {
    int active;      /* the fields below are valid and worth checking */
    int cf;          /* cached re_context_free() */
    int req;         /* an ASCII byte every match contains, or -1 */
    unsigned char wb;
    unsigned char ok[256];
};

typedef struct {
    unsigned char set[128]; /* ASCII first bytes */
    unsigned char req[128]; /* ASCII bytes every match contains */
    int nullable;           /* can match empty */
    int wb;                 /* every consuming match starts at a \b */
} FInfo;

typedef struct {
    const char *p;
    int bad; /* unsupported / unparsable: no filter */
    int depth;
} FParser;

static int fp_isword(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

static void set_range(unsigned char *set, int lo, int hi) {
    int c;
    if (lo < 0) lo = 0;
    for (c = lo; c <= hi && c < 128; c++) set[c] = 1;
}

static void set_all(unsigned char *set) { memset(set, 1, 128); }

static int set_empty(const unsigned char *set) {
    int c;
    for (c = 0; c < 128; c++)
        if (set[c]) return 0;
    return 1;
}

static int fp_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* An escape after '\\' (fp->p points after the backslash).  Returns the
   character, -2 if it added a set to `set`, -3 for an assertion (*assert
   set to the letter), or -1 (fp->bad set) for anything unsupported. */
static int fp_escape(FParser *fp, unsigned char *set, int in_class, int *assert) {
    int c = (unsigned char)*fp->p, h, v;
    if (!c) { fp->bad = 1; return -1; }
    fp->p++;
    switch (c) {
    case 'd': set_range(set, '0', '9'); return -2;
    case 'w': set_range(set, '0', '9'); set_range(set, 'A', 'Z'); set_range(set, 'a', 'z'); set['_'] = 1; return -2;
    case 's': set_range(set, '\t', '\r'); set[' '] = 1; return -2;
    case 'D': case 'W': case 'S': set_all(set); return -2;
    case 'b':
        if (in_class) return 8;
        *assert = 'b';
        return -3;
    case 'B': case 'A': case 'z':
        if (in_class) { fp->bad = 1; return -1; }
        *assert = c;
        return -3;
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case 'e': return 27;
    case 'x':
        v = 0;
        if (*fp->p == '{') {
            fp->p++;
            while ((h = fp_hex((unsigned char)*fp->p)) >= 0) {
                if (v < 0x110000) v = v * 16 + h;
                fp->p++;
            }
            if (*fp->p != '}') { fp->bad = 1; return -1; }
            fp->p++;
            return v;
        }
        if ((h = fp_hex((unsigned char)*fp->p)) < 0 || fp_hex((unsigned char)fp->p[1]) < 0) { fp->bad = 1; return -1; }
        v = h * 16 + fp_hex((unsigned char)fp->p[1]);
        fp->p += 2;
        return v;
    default:
        if (c >= '0' && c <= '7') {
            int k;
            v = c - '0';
            for (k = 0; k < 2 && *fp->p >= '0' && *fp->p <= '7'; k++) v = v * 8 + (*fp->p++ - '0');
            return v;
        }
        if (c >= 0x80) { fp->p--; return -4; } /* literal multi-byte char: caller skips it */
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            fp->bad = 1; /* \p, \Q, invalid escapes... */
            return -1;
        }
        return c; /* escaped punctuation */
    }
}

/* skip one (possibly multi-byte) literal character, return its first byte */
static int fp_char(FParser *fp) {
    int c = (unsigned char)*fp->p, cp;
    /* stops at the NUL: it is never a continuation byte */
    fp->p += utf8_decode(fp->p, 4, &cp);
    return c;
}

/* a [...] class (fp->p after the bracket); *single: its only member if
   it has exactly one, which is ASCII, else -1 */
static void fp_class(FParser *fp, unsigned char *set, int *single) {
    static const struct { const char *name; int lo[4], hi[4]; } posix[] = {
        {"alnum", {'0', 'A', 'a', -1}, {'9', 'Z', 'z', -1}},
        {"alpha", {'A', 'a', -1, -1}, {'Z', 'z', -1, -1}},
        {"ascii", {0, -1, -1, -1}, {127, -1, -1, -1}},
        {"blank", {'\t', ' ', -1, -1}, {'\t', ' ', -1, -1}},
        {"cntrl", {0, 127, -1, -1}, {31, 127, -1, -1}},
        {"digit", {'0', -1, -1, -1}, {'9', -1, -1, -1}},
        {"graph", {'!', -1, -1, -1}, {'~', -1, -1, -1}},
        {"lower", {'a', -1, -1, -1}, {'z', -1, -1, -1}},
        {"print", {' ', -1, -1, -1}, {'~', -1, -1, -1}},
        {"punct", {'!', ':', '[', '{'}, {'/', '@', '`', '~'}},
        {"space", {'\t', ' ', -1, -1}, {'\r', ' ', -1, -1}},
        {"upper", {'A', -1, -1, -1}, {'Z', -1, -1, -1}},
        {"word", {'0', 'A', 'a', '_'}, {'9', 'Z', 'z', '_'}},
        {"xdigit", {'0', 'A', 'a', -1}, {'9', 'F', 'f', -1}},
    };
    unsigned char cls[128];
    int first = 1, neg = 0, high = 0;
    *single = -1;
    memset(cls, 0, sizeof cls);
    if (*fp->p == '^') { neg = 1; fp->p++; }
    for (;;) {
        int lo, hi, dummy;
        if (!*fp->p) { fp->bad = 1; return; }
        if (*fp->p == ']' && !first) { fp->p++; break; }
        first = 0;
        if (fp->p[0] == '[' && fp->p[1] == ':') {
            const char *e = strstr(fp->p + 2, ":]"), *name = fp->p + 2;
            if (e) {
                int pneg = 0, k, found = 0;
                size_t i;
                if (*name == '^') { pneg = 1; name++; }
                for (i = 0; i < sizeof posix / sizeof posix[0]; i++) {
                    if (strlen(posix[i].name) == (size_t)(e - name) && !strncmp(posix[i].name, name, (size_t)(e - name))) {
                        if (pneg) set_all(cls);
                        for (k = 0; k < 4 && posix[i].lo[k] >= 0; k++) set_range(cls, posix[i].lo[k], posix[i].hi[k]);
                        found = 1;
                        break;
                    }
                }
                if (found) { fp->p = e + 2; continue; }
            }
        }
        if (*fp->p == '\\') {
            fp->p++;
            lo = fp_escape(fp, cls, 1, &dummy);
            if (fp->bad) return;
            if (lo == -2) continue;
            if (lo == -4) lo = fp_char(fp);
        } else {
            lo = fp_char(fp);
        }
        hi = lo;
        if (fp->p[0] == '-' && fp->p[1] && fp->p[1] != ']') {
            fp->p++;
            if (*fp->p == '\\') {
                unsigned char tmp[128];
                fp->p++;
                hi = fp_escape(fp, tmp, 1, &dummy);
                if (hi == -4) {
                    hi = 0x10FFFF;
                    fp_char(fp);
                } else if (fp->bad || hi < 0) {
                    fp->bad = 1;
                    return;
                }
            } else {
                hi = fp_char(fp);
                if (hi >= 0x80) hi = 0x10FFFF;
            }
        }
        if (lo < 0) { fp->bad = 1; return; }
        if (hi >= 128) high = 1;
        set_range(cls, lo, hi);
    }
    *single = -1;
    if (neg) set_all(set); /* complements aren't worth analysing */
    else {
        int c, n = 0;
        for (c = 0; c < 128; c++) {
            set[c] |= cls[c];
            if (cls[c]) { n++; *single = c; }
        }
        if (n != 1 || high) *single = -1;
    }
}

static void fp_alt(FParser *fp, FInfo *out);
static void fp_repeat(FParser *fp, FInfo *a, int *is_wb);

/* one atom plus an optional repetition; *is_wb: it's a bare \b */
static void fp_atom(FParser *fp, FInfo *a, int *is_wb) {
    int c = (unsigned char)*fp->p, assert = 0, single = -1;
    memset(a, 0, sizeof *a);
    *is_wb = 0;
    if (c == '(') {
        fp->p++;
        if (*fp->p == '?') {
            if (fp->p[1] == ':') fp->p += 2;
            else if (fp->p[1] == 'P' || fp->p[1] == '<') {
                const char *e = strchr(fp->p, '>');
                if (!e) { fp->bad = 1; return; }
                fp->p = e + 1;
            } else { fp->bad = 1; return; } /* flags */
        }
        if (++fp->depth > 32) { fp->bad = 1; return; }
        fp_alt(fp, a);
        fp->depth--;
        if (fp->bad) return;
        if (*fp->p != ')') { fp->bad = 1; return; }
        fp->p++;
    } else if (c == '[') {
        fp->p++;
        fp_class(fp, a->set, &single);
        if (single >= 0) a->req[single] = 1;
    } else if (c == '.') {
        fp->p++;
        set_all(a->set);
    } else if (c == '^' || c == '$') {
        fp->p++;
        a->nullable = 1;
    } else if (c == '\\') {
        int v;
        fp->p++;
        v = fp_escape(fp, a->set, 0, &assert);
        if (fp->bad) return;
        if (v == -3) {
            a->nullable = 1;
            *is_wb = assert == 'b';
        } else if (v == -4) {
            fp_char(fp); /* non-ASCII literal: only bytes >= 0x80 */
        } else if (v >= 0 && v < 128) {
            a->set[v] = 1;
            a->req[v] = 1;
        }
    } else if (c == '*' || c == '+' || c == '?' || c == ')' || c == '|' || !c) {
        fp->bad = 1;
        return;
    } else {
        c = fp_char(fp);
        if (c < 128) a->set[c] = a->req[c] = 1;
    }
    if (fp->bad) return;
    fp_repeat(fp, a, is_wb);
    if (a->nullable) memset(a->req, 0, sizeof a->req);
}

/* an optional repetition after an atom */
static void fp_repeat(FParser *fp, FInfo *a, int *is_wb) {
    int c;
    /* repetition */
    c = (unsigned char)*fp->p;
    if (c == '*' || c == '?') { a->nullable = 1; *is_wb = 0; fp->p++; }
    else if (c == '+') { *is_wb = 0; fp->p++; }
    else if (c == '{') {
        const char *q = fp->p + 1;
        int min = 0, any = 0;
        while (*q >= '0' && *q <= '9') { if (min < 100000) min = min * 10 + (*q - '0'); q++; any = 1; }
        if (any) {
            if (*q == ',') { q++; while (*q >= '0' && *q <= '9') q++; }
            if (*q == '}') {
                fp->p = q + 1;
                *is_wb = 0;
                if (min == 0) a->nullable = 1;
                c = '}';
            }
        }
        /* otherwise '{' is a literal: the next atom */
    } else return;
    if (c == '*' || c == '?' || c == '+' || c == '}') {
        if (*fp->p == '?') fp->p++; /* non-greedy */
        c = (unsigned char)*fp->p;
        if (c == '*' || c == '+' || c == '?') fp->bad = 1; /* nested repetition */
        if (c == '{') {
            const char *q = fp->p + 1;
            if (*q >= '0' && *q <= '9') fp->bad = 1; /* maybe nested repetition: don't guess */
        }
    }
}

static void fp_cat(FParser *fp, FInfo *out) {
    int wb_seen = 0;
    memset(out, 0, sizeof *out);
    out->nullable = 1;
    out->wb = 1;
    while (*fp->p && *fp->p != '|' && *fp->p != ')') {
        FInfo a;
        int is_wb, c;
        fp_atom(fp, &a, &is_wb);
        if (fp->bad) return;
        for (c = 0; c < 128; c++) out->req[c] |= a.req[c];
        if (out->nullable) {
            /* matches can start in this atom */
            if (!set_empty(a.set)) {
                for (c = 0; c < 128; c++) out->set[c] |= a.set[c];
                if (!(wb_seen || a.wb)) out->wb = 0;
            }
            if (is_wb) wb_seen = 1;
            out->nullable = a.nullable;
        }
    }
}

static void fp_alt(FParser *fp, FInfo *out) {
    int c;
    fp_cat(fp, out);
    while (!fp->bad && *fp->p == '|') {
        FInfo b;
        fp->p++;
        fp_cat(fp, &b);
        if (fp->bad) return;
        for (c = 0; c < 128; c++) {
            out->set[c] |= b.set[c];
            out->req[c] &= b.req[c];
        }
        if (b.nullable) out->nullable = 1;
        if (!set_empty(b.set) && !b.wb) out->wb = 0;
    }
}

/* Per-regex search info for a compiled rule: the cached
   re_context_free() and, if the source was understood and the filter is
   worth checking, the prefilter. */
static ReFilter *make_filter(const char *src, const Regex *re) {
    FParser fp;
    FInfo fi;
    ReFilter *f = xcalloc(1, sizeof *f);
    int c, n = 0, best = -1, best_rank = 9;
    f->cf = re_context_free(re);
    f->req = -1;
    memset(&fp, 0, sizeof fp);
    fp.p = src;
    fp_alt(&fp, &fi);
    if (fp.bad || *fp.p || fi.nullable) return f;
    for (c = 0; c < 128; c++) {
        n += fi.set[c];
        /* the required byte most likely to be absent from a line:
           punctuation, then upper case, digits, lower case; never space */
        if (fi.req[c] && c != ' ') {
            int rank = fp_isword(c) ? (c >= 'A' && c <= 'Z' ? 1 : c >= '0' && c <= '9' ? 2 : 3) : 0;
            if (best < 0 || rank < best_rank) { best = c; best_rank = rank; }
        }
    }
    if (n == 128 && !fi.wb && best < 0) return f;
    f->active = 1;
    for (c = 0; c < 128; c++) f->ok[c] = fi.set[c];
    memset(f->ok + 128, 1, 128);
    f->wb = (unsigned char)fi.wb;
    f->req = best;
    return f;
}

static int context_free(const Regex *re, const ReFilter *f) { return f ? f->cf : re_context_free(re); }

/* first position >= start in s[0..len) where `f` allows a match to begin
   (len if none: then there is no match, since filtered patterns never
   match the empty string) */
static int filter_skip(const ReFilter *f, const char *s, int len, int start) {
    const unsigned char *u = (const unsigned char *)s;
    int i = start;
    for (;;) {
        while (i < len && !f->ok[u[i]]) i++;
        if (i >= len || !f->wb || i == 0 || !fp_isword(u[i]) || !fp_isword(u[i - 1])) return i;
        /* inside a word: \b can't hold here; go to the word's end */
        while (i < len && fp_isword(u[i])) i++;
    }
}

/* re_search with the prefilter */
static int search(const Regex *re, const ReFilter *f, const char *s, int len, int start, int *caps) {
    if (f && f->active) {
        if (start >= len) return 0;
        if (f->req >= 0 && !memchr(s + start, f->req, (size_t)(len - start))) return 0;
        start = filter_skip(f, s, len, start);
        if (start >= len) return 0;
    }
    return re_search(re, s, len, start, caps, 1);
}

static Regex *compile_rule(const char *src, ReFilter **flt, char **err) {
    Regex *re = re_compile(src, 0, err);
    *flt = re ? make_filter(src, re) : NULL;
    return re;
}

/* ---------------- rule parsing ---------------- */

/* Arrays grow to the next power of two (the capacity is implied by the
   count, so Rules needs no extra field). */
static int needs_grow(int n) { return n == 0 || (n >= 4 && (n & (n - 1)) == 0); }
static size_t grown(int n) { return n == 0 ? 4 : (size_t)n * 2; }

/* Rules added while loading one syntax (includes copy whole rule trees,
   so a chain of syntaxes including each other several times would
   otherwise grow exponentially). */
#define RULE_BUDGET 200000
static long rule_budget;

static void rules_add_pattern(Rules *r, int group, Regex *re, ReFilter *flt) {
    if (needs_grow(r->npats)) r->pats = xrealloc(r->pats, sizeof *r->pats * grown(r->npats));
    r->pats[r->npats].group = group;
    r->pats[r->npats].re = re;
    r->pats[r->npats].flt = flt;
    r->npats++;
}

static void rules_add_region(Rules *r, Region *reg) {
    if (needs_grow(r->nregions)) r->regions = xrealloc(r->regions, sizeof *r->regions * grown(r->nregions));
    r->regions[r->nregions++] = reg;
}

static void copy_rules(const Rules *src, Rules *dst, Region *parent) {
    int i;
    for (i = 0; i < src->npats && rule_budget > 0; i++, rule_budget--)
        rules_add_pattern(dst, src->pats[i].group, src->pats[i].re, src->pats[i].flt);
    for (i = 0; i < src->nregions && rule_budget > 0; i++, rule_budget--) {
        Region *o = src->regions[i], *n = xcalloc(1, sizeof *n);
        n->group = o->group;
        n->limit_group = o->limit_group;
        n->start = o->start;
        n->end = o->end;
        n->skip = o->skip;
        n->start_flt = o->start_flt;
        n->end_flt = o->end_flt;
        n->skip_flt = o->skip_flt;
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
                ReFilter *flt;
                Regex *re = compile_rule(v->str, &flt, &e);
                if (!re) {
                    if (err && !*err) *err = xsprintf("%s: %s: %s", syn->file, key, e ? e : "invalid regex");
                    free(e);
                    continue;
                }
                rules_add_pattern(out, group_id(key), re, flt);
            } else if (v->type == CN_MAP) {
                Region *r = xcalloc(1, sizeof *r);
                const char *s;
                char *e = NULL;
                r->group = group_id(key);
                s = cnode_str(v, "limit-group");
                r->limit_group = s ? group_id(s) : r->group;
                r->parent = parent;
                s = cnode_str(v, "start");
                if (s && *s) r->start = compile_rule(s, &r->start_flt, &e);
                free(e); e = NULL;
                s = cnode_str(v, "end");
                if (s && *s) r->end = compile_rule(s, &r->end_flt, &e);
                free(e); e = NULL;
                s = cnode_str(v, "skip");
                if (s && *s) r->skip = compile_rule(s, &r->skip_flt, &e);
                free(e);
                if (!r->start || !r->end) {
                    if (err && !*err) *err = xsprintf("%s: bad region %s", syn->file, key);
                    re_free(r->start);
                    re_free(r->end);
                    re_free(r->skip);
                    free(r->start_flt);
                    free(r->end_flt);
                    free(r->skip_flt);
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
    if (load_depth == 0) rule_budget = RULE_BUDGET;
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

static const char *masked_from(Regex *skip, const ReFilter *sf, const char *line, int off, int len, int *gen) {
    int i, slot = -1, pos = 0, n = len - off, caps[2];
    const char *sub = line + off;
    SkipMask *k;
    for (i = 0; i < nskm; i++) {
        if (skm[i].skip != skip) continue;
        slot = i;
        if (context_free(skip, sf) && skm[i].from <= off && !skm[i].in[off]) {
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
    while (pos <= n && search(skip, sf, sub, n, pos, caps)) {
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
static int find_index_gen(Regex *re, const ReFilter *rf, Regex *skip, const ReFilter *sf, const char *s, int off,
                          int len, int *ms, int *me, int *gen) {
    const char *sub = s + off;
    int n = len - off, caps[2];
    *gen = 0;
    if (skip) sub = masked_from(skip, sf, s, off, len, gen);
    if (!search(re, rf, sub, n, 0, caps)) return 0;
    *ms = caps[0];
    *me = caps[1];
    return 1;
}

static int find_index(Regex *re, const ReFilter *rf, Regex *skip, const ReFilter *sf, const char *s, int off, int len,
                      int *ms, int *me) {
    int gen;
    return find_index_gen(re, rf, skip, sf, s, off, len, ms, me, &gen);
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
    if (!context_free(r->start, r->start_flt))
        return find_index(r->start, r->start_flt, r->skip, r->skip_flt, line, off, len, ms, me);
    for (i = 0; i < nscache; i++) {
        StartCache *c = &scache[i];
        if (c->re == r->start && c->skip == r->skip && c->from <= off) {
            int ok = 1;
            if (r->skip) {
                int g;
                masked_from(r->skip, r->skip_flt, line, off, len, &g);
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
    found = find_index_gen(r->start, r->start_flt, r->skip, r->skip_flt, line, off, len, &s, &e, &gen);
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

static int pat_search(Regex *re, const ReFilter *flt, const char *line, int off, int end, int pos, int *ms, int *me) {
    int i, caps[2], found, apos = off + pos;
    int cf = context_free(re, flt);
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
    found = search(re, flt, line + off, end - off, pos, caps);
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
        while (pos <= n && pat_search(p->re, p->flt, s, off, end, pos, &ms, &me)) {
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
    int off = 0;
    long iter = 0;
    nscache = 0;
    nskm = 0;
    npcache = 0;
    if (!syn) {
        if (hl) fill(hl, 0, len, 0);
        return NULL;
    }
    for (;;) {
        if (++iter > 2 * (long)len + 64) {
            /* pathological definition (empty start/end matches): stop,
               but leave no stale ids in the rest of hl */
            if (hl && off < len) fill(hl, off, len, cur ? cur->group : 0);
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
            have_end = off <= len && find_index(cur->end, cur->end_flt, cur->skip, cur->skip_flt, line, off, len, &es, &ee);
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
