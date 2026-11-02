/*
 * regex.c - RE2-syntax subset regular expressions (see regex.h).
 */
#include "regex.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* AST                                                                 */

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_ASSERT };
enum { A_BOL, A_EOL, A_WORDB, A_NWORDB, A_BOT, A_EOT };

typedef struct Node {
    int type;
    int cp;          /* N_CHAR: code point; N_CLASS: class idx; N_ASSERT: kind; N_ANY: dotnl */
    int icase;       /* N_CHAR */
    int min, max;    /* N_REP (max -1 = inf) */
    int greedy;      /* N_REP */
    int cap;         /* N_GROUP: capture index or -1 */
    int multiline;   /* N_ASSERT */
    struct Node **kids;
    int nkids, kcap;
} Node;

typedef struct {
    int lo, hi;
} Range;

typedef struct {
    Range *r;
    int n, cap;
    int neg;
    int icase;
} Class;

enum { I_CHAR, I_CHARI, I_ANY, I_ANYNL, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_ASSERT, I_MATCH };

typedef struct {
    unsigned char op;
    unsigned char flag;
    int x, y;
} Inst;

struct Regex {
    Inst *prog;
    int nprog, cprog;
    Class *classes;
    int nclass, cclass;
    int ncap;
    int anchored;  /* starts with \A or ^ (non-multiline) */
    int firstbyte; /* -1 or byte that every match must start with */
};

typedef struct {
    const char *p;
    const char *start;
    char *err;
    Regex *re;
    int ncap;
    int depth;
} Parser;

static Node *new_node(int type) {
    Node *n = xcalloc(1, sizeof *n);
    n->type = type;
    n->cap = -1;
    return n;
}

static void add_kid(Node *n, Node *k) {
    if (n->nkids == n->kcap) {
        n->kcap = n->kcap ? n->kcap * 2 : 4;
        n->kids = xrealloc(n->kids, sizeof *n->kids * (size_t)n->kcap);
    }
    n->kids[n->nkids++] = k;
}

static void free_node(Node *n) {
    int i;
    if (!n) return;
    for (i = 0; i < n->nkids; i++) free_node(n->kids[i]);
    free(n->kids);
    free(n);
}

static int new_class(Regex *re) {
    if (re->nclass == re->cclass) {
        re->cclass = re->cclass ? re->cclass * 2 : 4;
        re->classes = xrealloc(re->classes, sizeof *re->classes * (size_t)re->cclass);
    }
    memset(&re->classes[re->nclass], 0, sizeof(Class));
    return re->nclass++;
}

static void class_add(Class *c, int lo, int hi) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->r = xrealloc(c->r, sizeof *c->r * (size_t)c->cap);
    }
    c->r[c->n].lo = lo;
    c->r[c->n].hi = hi;
    c->n++;
}

/* add the complement of ranges [r..r+n) (sorted not required) */
static void class_add_negated(Class *c, const Range *r, int n) {
    /* simple approach: sort a copy and add the gaps */
    Range *t = xmalloc(sizeof *t * (size_t)(n ? n : 1));
    int i, j, prev = 0;
    memcpy(t, r, sizeof *t * (size_t)n);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && t[j - 1].lo > t[j].lo; j--) {
            Range tmp = t[j]; t[j] = t[j - 1]; t[j - 1] = tmp;
        }
    for (i = 0; i < n; i++) {
        if (t[i].lo > prev) class_add(c, prev, t[i].lo - 1);
        if (t[i].hi + 1 > prev) prev = t[i].hi + 1;
    }
    if (prev <= 0x10FFFF) class_add(c, prev, 0x10FFFF);
    free(t);
}

static const Range r_digit[] = {{'0', '9'}};
static const Range r_word[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
static const Range r_space[] = {{'\t', '\n'}, {'\f', '\r'}, {' ', ' '}};

static void class_add_set(Class *c, const Range *r, int n, int neg) {
    int i;
    if (neg) class_add_negated(c, r, n);
    else for (i = 0; i < n; i++) class_add(c, r[i].lo, r[i].hi);
}

/* POSIX [:name:] classes */
static int posix_class(const char *name, int len, Class *c, int neg) {
    static const struct { const char *name; Range r[4]; int n; } tbl[] = {
        {"alnum", {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}, 3},
        {"alpha", {{'A', 'Z'}, {'a', 'z'}}, 2},
        {"ascii", {{0, 0x7F}}, 1},
        {"blank", {{'\t', '\t'}, {' ', ' '}}, 2},
        {"cntrl", {{0, 0x1F}, {0x7F, 0x7F}}, 2},
        {"digit", {{'0', '9'}}, 1},
        {"graph", {{'!', '~'}}, 1},
        {"lower", {{'a', 'z'}}, 1},
        {"print", {{' ', '~'}}, 1},
        {"punct", {{'!', '/'}, {':', '@'}, {'[', '`'}, {'{', '~'}}, 4},
        {"space", {{'\t', '\r'}, {' ', ' '}}, 2},
        {"upper", {{'A', 'Z'}}, 1},
        {"word", {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}, 4},
        {"xdigit", {{'0', '9'}, {'A', 'F'}, {'a', 'f'}}, 3},
    };
    size_t i;
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if ((int)strlen(tbl[i].name) == len && !strncmp(tbl[i].name, name, (size_t)len)) {
            class_add_set(c, tbl[i].r, tbl[i].n, neg);
            return 1;
        }
    }
    return 0;
}

/* Approximate Unicode classes (\pL, \p{Greek}, ...) */
static int unicode_class(const char *name, int len, Class *c, int neg) {
    Range r[8];
    int n = 0;
#define IS(s) ((int)strlen(s) == len && !strncmp(name, s, (size_t)len))
    if (IS("L") || IS("Letter")) {
        r[n].lo = 'A'; r[n++].hi = 'Z'; r[n].lo = 'a'; r[n++].hi = 'z';
        r[n].lo = 0xAA; r[n++].hi = 0xAA; r[n].lo = 0xB5; r[n++].hi = 0xB5;
        r[n].lo = 0xC0; r[n++].hi = 0xD6; r[n].lo = 0xD8; r[n++].hi = 0xF6;
        r[n].lo = 0xF8; r[n++].hi = 0x2FF; r[n].lo = 0x370; r[n++].hi = 0x10FFFF;
    } else if (IS("Lu")) {
        r[n].lo = 'A'; r[n++].hi = 'Z'; r[n].lo = 0xC0; r[n++].hi = 0xD6;
        r[n].lo = 0xD8; r[n++].hi = 0xDE; r[n].lo = 0x391; r[n++].hi = 0x3A9;
        r[n].lo = 0x410; r[n++].hi = 0x42F;
    } else if (IS("Ll")) {
        r[n].lo = 'a'; r[n++].hi = 'z'; r[n].lo = 0xDF; r[n++].hi = 0xF6;
        r[n].lo = 0xF8; r[n++].hi = 0xFF; r[n].lo = 0x3B1; r[n++].hi = 0x3C9;
        r[n].lo = 0x430; r[n++].hi = 0x44F;
    } else if (IS("N") || IS("Nd")) {
        r[n].lo = '0'; r[n++].hi = '9'; r[n].lo = 0x660; r[n++].hi = 0x669;
        r[n].lo = 0xFF10; r[n++].hi = 0xFF19;
    } else if (IS("Greek")) {
        r[n].lo = 0x370; r[n++].hi = 0x3FF; r[n].lo = 0x1F00; r[n++].hi = 0x1FFF;
    } else if (IS("Cyrillic")) {
        r[n].lo = 0x400; r[n++].hi = 0x52F;
    } else if (IS("Han")) {
        r[n].lo = 0x2E80; r[n++].hi = 0x2FDF; r[n].lo = 0x3400; r[n++].hi = 0x4DBF;
        r[n].lo = 0x4E00; r[n++].hi = 0x9FFF; r[n].lo = 0xF900; r[n++].hi = 0xFAFF;
    } else if (IS("Hiragana")) {
        r[n].lo = 0x3040; r[n++].hi = 0x309F;
    } else if (IS("Katakana")) {
        r[n].lo = 0x30A0; r[n++].hi = 0x30FF;
    } else if (IS("Latin")) {
        r[n].lo = 'A'; r[n++].hi = 'Z'; r[n].lo = 'a'; r[n++].hi = 'z';
        r[n].lo = 0xC0; r[n++].hi = 0x24F;
    } else if (IS("P") || IS("Punct")) {
        r[n].lo = '!'; r[n++].hi = '/'; r[n].lo = ':'; r[n++].hi = '@';
        r[n].lo = '['; r[n++].hi = '`'; r[n].lo = '{'; r[n++].hi = '~';
        r[n].lo = 0x2010; r[n++].hi = 0x2027;
    } else if (IS("Z") || IS("Zs")) {
        r[n].lo = ' '; r[n++].hi = ' '; r[n].lo = 0xA0; r[n++].hi = 0xA0;
        r[n].lo = 0x2000; r[n++].hi = 0x200A; r[n].lo = 0x3000; r[n++].hi = 0x3000;
    } else if (IS("S") || IS("Sm") || IS("So")) {
        r[n].lo = '$'; r[n++].hi = '$'; r[n].lo = '+'; r[n++].hi = '+';
        r[n].lo = '<'; r[n++].hi = '>'; r[n].lo = '^'; r[n++].hi = '^';
        r[n].lo = '|'; r[n++].hi = '|'; r[n].lo = '~'; r[n++].hi = '~';
        r[n].lo = 0x2190; r[n++].hi = 0x2BFF;
    } else {
        /* unknown script/category: approximate as "any non-ASCII" */
        r[n].lo = 0x80; r[n++].hi = 0x10FFFF;
    }
#undef IS
    class_add_set(c, r, n, neg);
    return 1;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void set_err(Parser *ps, const char *msg) {
    if (!ps->err) ps->err = xsprintf("%s at offset %d", msg, (int)(ps->p - ps->start));
}

/* Parse an escape after '\'.  Returns code point >= 0 for a literal,
 * or -2 if it added a class into *cls (when cls != NULL), or -3 for an
 * assertion (stored in *assert_kind), -1 on error. */
static int parse_escape(Parser *ps, Class *cls, int *assert_kind) {
    int c = (unsigned char)*ps->p;
    if (!c) { set_err(ps, "trailing backslash"); return -1; }
    ps->p++;
    switch (c) {
    case 'd': case 'D':
        if (!cls) return -1;
        class_add_set(cls, r_digit, 1, c == 'D');
        return -2;
    case 'w': case 'W':
        if (!cls) return -1;
        class_add_set(cls, r_word, 4, c == 'W');
        return -2;
    case 's': case 'S':
        if (!cls) return -1;
        class_add_set(cls, r_space, 3, c == 'S');
        return -2;
    case 'p': case 'P': {
        const char *name;
        int len;
        if (!cls) return -1;
        if (*ps->p == '{') {
            const char *e = strchr(ps->p, '}');
            if (!e) { set_err(ps, "bad \\p"); return -1; }
            name = ps->p + 1;
            len = (int)(e - name);
            ps->p = e + 1;
        } else {
            name = ps->p;
            len = 1;
            if (*ps->p) ps->p++;
        }
        {
            int neg = (c == 'P');
            if (len > 0 && name[0] == '^') { neg = !neg; name++; len--; }
            unicode_class(name, len, cls, neg);
        }
        return -2;
    }
    case 'b': if (assert_kind) { *assert_kind = A_WORDB; return -3; } return 8;
    case 'B': if (assert_kind) { *assert_kind = A_NWORDB; return -3; } return -1;
    case 'A': if (assert_kind) { *assert_kind = A_BOT; return -3; } return -1;
    case 'z': if (assert_kind) { *assert_kind = A_EOT; return -3; } return -1;
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case 'e': return 27;
    case 'x': {
        int v = 0, h;
        if (*ps->p == '{') {
            ps->p++;
            while ((h = hexval((unsigned char)*ps->p)) >= 0) { v = v * 16 + h; ps->p++; }
            if (*ps->p != '}') { set_err(ps, "bad \\x{}"); return -1; }
            ps->p++;
            return v;
        }
        h = hexval((unsigned char)*ps->p);
        if (h < 0) { set_err(ps, "bad \\x"); return -1; }
        v = h; ps->p++;
        h = hexval((unsigned char)*ps->p);
        if (h < 0) { set_err(ps, "bad \\x"); return -1; }
        v = v * 16 + h; ps->p++;
        return v;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = c - '0', k;
        for (k = 0; k < 2 && *ps->p >= '0' && *ps->p <= '7'; k++) v = v * 8 + (*ps->p++ - '0');
        return v;
    }
    default:
        if (isalnum(c)) { set_err(ps, "invalid escape"); return -1; }
        /* punctuation or utf-8 byte: literal */
        if (c >= 0x80) {
            int cp, n;
            ps->p--;
            n = utf8_decode(ps->p, (int)strlen(ps->p), &cp);
            ps->p += n;
            return cp;
        }
        return c;
    }
}

static Node *parse_alt(Parser *ps, int *flags);

static Node *parse_class(Parser *ps, int flags) {
    Node *n;
    int idx = new_class(ps->re);
    Class *c;
    int first = 1;
    /* ps->p points after '[' */
    c = &ps->re->classes[idx];
    if (*ps->p == '^') { c->neg = 1; ps->p++; }
    c->icase = (flags & RE_ICASE) != 0;
    for (;;) {
        int lo, hi;
        c = &ps->re->classes[idx];
        if (!*ps->p) { set_err(ps, "missing ]"); return NULL; }
        if (*ps->p == ']' && !first) { ps->p++; break; }
        first = 0;
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            const char *e = strstr(ps->p + 2, ":]");
            if (e) {
                const char *name = ps->p + 2;
                int neg = 0;
                if (*name == '^') { neg = 1; name++; }
                if (posix_class(name, (int)(e - name), c, neg)) {
                    ps->p = e + 2;
                    continue;
                }
            }
        }
        if (*ps->p == '\\') {
            ps->p++;
            lo = parse_escape(ps, c, NULL);
            if (lo == -2) continue;
            if (lo < 0) { if (!ps->err) set_err(ps, "bad escape in class"); return NULL; }
        } else {
            int n2 = utf8_decode(ps->p, (int)strlen(ps->p), &lo);
            ps->p += n2;
        }
        hi = lo;
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            ps->p++;
            if (*ps->p == '\\') {
                ps->p++;
                hi = parse_escape(ps, NULL, NULL);
                if (hi < 0) { if (!ps->err) set_err(ps, "bad range"); return NULL; }
            } else {
                int n2 = utf8_decode(ps->p, (int)strlen(ps->p), &hi);
                ps->p += n2;
            }
            if (hi < lo) { set_err(ps, "invalid class range"); return NULL; }
        }
        class_add(c, lo, hi);
    }
    n = new_node(N_CLASS);
    n->cp = idx;
    return n;
}

static int parse_int(Parser *ps, int *out) {
    int v = 0, any = 0;
    while (*ps->p >= '0' && *ps->p <= '9') { v = v * 10 + (*ps->p++ - '0'); any = 1; if (v > 100000) v = 100000; }
    *out = v;
    return any;
}

static Node *parse_atom(Parser *ps, int *flags) {
    int c = (unsigned char)*ps->p;
    Node *n;
    if (c == '(') {
        ps->p++;
        if (++ps->depth > 500) { set_err(ps, "nesting too deep"); return NULL; }
        if (*ps->p == '?') {
            /* (?:...), (?flags), (?flags:...), (?P<name>...) */
            int newflags = *flags, on = 1;
            ps->p++;
            if (*ps->p == 'P' || *ps->p == '<') {
                const char *e;
                int cap;
                if (*ps->p == 'P') ps->p++;
                if (*ps->p != '<') { set_err(ps, "bad named group"); return NULL; }
                e = strchr(ps->p, '>');
                if (!e) { set_err(ps, "bad named group"); return NULL; }
                ps->p = e + 1;
                cap = ps->ncap++;
                n = new_node(N_GROUP);
                n->cap = cap;
                {
                    int f2 = *flags;
                    Node *k = parse_alt(ps, &f2);
                    if (!k) { free_node(n); return NULL; }
                    add_kid(n, k);
                }
                if (*ps->p != ')') { set_err(ps, "missing )"); free_node(n); return NULL; }
                ps->p++;
                ps->depth--;
                return n;
            }
            for (;;) {
                c = (unsigned char)*ps->p;
                if (c == '-') { on = 0; ps->p++; continue; }
                if (c == 'i') { newflags = on ? (newflags | RE_ICASE) : (newflags & ~RE_ICASE); ps->p++; continue; }
                if (c == 'm') { newflags = on ? (newflags | 0x100) : (newflags & ~0x100); ps->p++; continue; }
                if (c == 's') { newflags = on ? (newflags | 0x200) : (newflags & ~0x200); ps->p++; continue; }
                if (c == 'U') { newflags = on ? (newflags | 0x400) : (newflags & ~0x400); ps->p++; continue; }
                break;
            }
            if (*ps->p == ')') {
                ps->p++;
                *flags = newflags; /* applies to rest of current group */
                ps->depth--;
                return new_node(N_EMPTY);
            }
            if (*ps->p == ':') {
                Node *k;
                ps->p++;
                k = parse_alt(ps, &newflags);
                if (!k) return NULL;
                if (*ps->p != ')') { set_err(ps, "missing )"); free_node(k); return NULL; }
                ps->p++;
                ps->depth--;
                n = new_node(N_GROUP);
                n->cap = -1;
                add_kid(n, k);
                return n;
            }
            set_err(ps, "invalid or unsupported group syntax");
            return NULL;
        } else {
            int cap = ps->ncap++;
            int f2 = *flags;
            Node *k = parse_alt(ps, &f2);
            if (!k) return NULL;
            if (*ps->p != ')') { set_err(ps, "missing )"); free_node(k); return NULL; }
            ps->p++;
            ps->depth--;
            n = new_node(N_GROUP);
            n->cap = cap;
            add_kid(n, k);
            return n;
        }
    }
    if (c == '[') {
        ps->p++;
        return parse_class(ps, *flags);
    }
    if (c == '.') {
        ps->p++;
        n = new_node(N_ANY);
        n->cp = (*flags & 0x200) != 0;
        return n;
    }
    if (c == '^' || c == '$') {
        ps->p++;
        n = new_node(N_ASSERT);
        n->cp = c == '^' ? A_BOL : A_EOL;
        n->multiline = (*flags & 0x100) != 0;
        return n;
    }
    if (c == '\\') {
        int ak = -1, v;
        Class tmp;
        memset(&tmp, 0, sizeof tmp);
        ps->p++;
        if (*ps->p == 'Q') {
            /* \Q...\E literal */
            const char *e;
            Node *cat = new_node(N_CAT);
            ps->p++;
            e = strstr(ps->p, "\\E");
            if (!e) e = ps->p + strlen(ps->p);
            while (ps->p < e) {
                int cp, k = utf8_decode(ps->p, (int)(e - ps->p), &cp);
                Node *ch = new_node(N_CHAR);
                ch->cp = cp;
                ch->icase = (*flags & RE_ICASE) != 0;
                add_kid(cat, ch);
                ps->p += k;
            }
            if (*ps->p) ps->p += 2;
            return cat;
        }
        v = parse_escape(ps, &tmp, &ak);
        if (v == -3) {
            free(tmp.r);
            n = new_node(N_ASSERT);
            n->cp = ak;
            return n;
        }
        if (v == -2) {
            int idx = new_class(ps->re);
            ps->re->classes[idx] = tmp;
            ps->re->classes[idx].icase = (*flags & RE_ICASE) != 0;
            n = new_node(N_CLASS);
            n->cp = idx;
            return n;
        }
        free(tmp.r);
        if (v < 0) { if (!ps->err) set_err(ps, "bad escape"); return NULL; }
        n = new_node(N_CHAR);
        n->cp = v;
        n->icase = (*flags & RE_ICASE) != 0;
        return n;
    }
    {
        int cp, k = utf8_decode(ps->p, (int)strlen(ps->p), &cp);
        ps->p += k;
        n = new_node(N_CHAR);
        n->cp = cp;
        n->icase = (*flags & RE_ICASE) != 0;
        return n;
    }
}

static Node *parse_cat(Parser *ps, int *flags) {
    Node *cat = new_node(N_CAT);
    int nrep;
    while (*ps->p && *ps->p != '|' && *ps->p != ')') {
        Node *a;
        int c = (unsigned char)*ps->p;
        if (c == '*' || c == '+' || c == '?') { set_err(ps, "missing argument to repetition operator"); free_node(cat); return NULL; }
        a = parse_atom(ps, flags);
        if (!a) { free_node(cat); return NULL; }
        /* repetitions (a second, nested one is an error, as in RE2) */
        for (nrep = 0;; nrep++) {
            int min, max;
            const char *save = ps->p;
            c = (unsigned char)*ps->p;
            if (c == '*') { min = 0; max = -1; ps->p++; }
            else if (c == '+') { min = 1; max = -1; ps->p++; }
            else if (c == '?') { min = 0; max = 1; ps->p++; }
            else if (c == '{') {
                ps->p++;
                if (!parse_int(ps, &min)) { ps->p = save; break; }
                if (*ps->p == ',') {
                    ps->p++;
                    if (!parse_int(ps, &max)) max = -1;
                } else max = min;
                if (*ps->p != '}') { ps->p = save; break; }
                ps->p++;
                if (min > 1000 || max > 1000 || (max >= 0 && max < min)) {
                    set_err(ps, "invalid repeat count");
                    free_node(a); free_node(cat);
                    return NULL;
                }
            } else break;
            if (nrep > 0) {
                set_err(ps, "invalid nested repetition operator");
                free_node(a); free_node(cat);
                return NULL;
            }
            {
                Node *r = new_node(N_REP);
                r->min = min;
                r->max = max;
                r->greedy = 1;
                if (*ps->p == '?') { r->greedy = 0; ps->p++; }
                if (*flags & 0x400) r->greedy = !r->greedy;
                add_kid(r, a);
                a = r;
            }
        }
        /* a '{' that did not form a repeat is parsed as a literal next loop */
        add_kid(cat, a);
    }
    return cat;
}

static Node *parse_alt(Parser *ps, int *flags) {
    Node *first = parse_cat(ps, flags), *alt;
    if (!first) return NULL;
    if (*ps->p != '|') return first;
    alt = new_node(N_ALT);
    add_kid(alt, first);
    while (*ps->p == '|') {
        Node *k;
        ps->p++;
        k = parse_cat(ps, flags);
        if (!k) { free_node(alt); return NULL; }
        add_kid(alt, k);
    }
    return alt;
}

/* ------------------------------------------------------------------ */
/* Code generation                                                     */

static int emit(Regex *re, int op, int x, int y, int flag) {
    if (re->nprog == re->cprog) {
        re->cprog = re->cprog ? re->cprog * 2 : 64;
        re->prog = xrealloc(re->prog, sizeof *re->prog * (size_t)re->cprog);
    }
    re->prog[re->nprog].op = (unsigned char)op;
    re->prog[re->nprog].x = x;
    re->prog[re->nprog].y = y;
    re->prog[re->nprog].flag = (unsigned char)flag;
    return re->nprog++;
}

/* can the node match the empty string? */
static int nullable(const Node *n) {
    int i;
    switch (n->type) {
    case N_EMPTY: case N_ASSERT: return 1;
    case N_CHAR: case N_ANY: case N_CLASS: return 0;
    case N_CAT:
        for (i = 0; i < n->nkids; i++) if (!nullable(n->kids[i])) return 0;
        return 1;
    case N_ALT:
        for (i = 0; i < n->nkids; i++) if (nullable(n->kids[i])) return 1;
        return 0;
    case N_REP: return n->min == 0 || nullable(n->kids[0]);
    case N_GROUP: return nullable(n->kids[0]);
    }
    return 0;
}

static int gen(Regex *re, Node *n) {
    int i;
    if (re->nprog > 200000) return -1;
    switch (n->type) {
    case N_EMPTY: break;
    case N_CHAR: {
        char buf[4];
        int k = utf8_encode(n->cp, buf);
        if (k == 1 && n->icase && isalpha((unsigned char)buf[0]))
            emit(re, I_CHARI, tolower((unsigned char)buf[0]), 0, 0);
        else
            for (i = 0; i < k; i++) emit(re, I_CHAR, (unsigned char)buf[i], 0, 0);
        break;
    }
    case N_ANY: emit(re, n->cp ? I_ANYNL : I_ANY, 0, 0, 0); break;
    case N_CLASS: emit(re, I_CLASS, n->cp, 0, 0); break;
    case N_ASSERT: emit(re, I_ASSERT, n->cp, 0, n->multiline); break;
    case N_CAT:
        for (i = 0; i < n->nkids; i++)
            if (gen(re, n->kids[i]) < 0) return -1;
        break;
    case N_ALT: {
        int *jmps = xmalloc(sizeof(int) * (size_t)n->nkids);
        for (i = 0; i < n->nkids; i++) {
            if (i < n->nkids - 1) {
                int sp = emit(re, I_SPLIT, 0, 0, 0);
                re->prog[sp].x = re->nprog;
                if (gen(re, n->kids[i]) < 0) { free(jmps); return -1; }
                jmps[i] = emit(re, I_JMP, 0, 0, 0);
                re->prog[sp].y = re->nprog;
            } else {
                if (gen(re, n->kids[i]) < 0) { free(jmps); return -1; }
            }
        }
        for (i = 0; i < n->nkids - 1; i++) re->prog[jmps[i]].x = re->nprog;
        free(jmps);
        break;
    }
    case N_GROUP:
        if (n->cap >= 0) emit(re, I_SAVE, 2 * n->cap, 0, 0);
        if (gen(re, n->kids[0]) < 0) return -1;
        if (n->cap >= 0) emit(re, I_SAVE, 2 * n->cap + 1, 0, 0);
        break;
    case N_REP: {
        Node *k = n->kids[0];
        for (i = 0; i < n->min; i++)
            if (gen(re, k) < 0) return -1;
        if (n->max == -1 && nullable(k)) {
            /* like Go: x* with a nullable body compiles as (x+)? so that an
               empty iteration cannot hide the exit path from the memo table */
            int sp = emit(re, I_SPLIT, 0, 0, 0), sp2;
            int body = re->nprog;
            if (gen(re, k) < 0) return -1;
            sp2 = emit(re, I_SPLIT, 0, 0, 0);
            if (n->greedy) {
                re->prog[sp].x = body; re->prog[sp].y = re->nprog;
                re->prog[sp2].x = body; re->prog[sp2].y = re->nprog;
            } else {
                re->prog[sp].x = re->nprog; re->prog[sp].y = body;
                re->prog[sp2].x = re->nprog; re->prog[sp2].y = body;
            }
        } else if (n->max == -1) {
            int sp = emit(re, I_SPLIT, 0, 0, 0), j;
            int body = re->nprog;
            if (gen(re, k) < 0) return -1;
            j = emit(re, I_JMP, sp, 0, 0);
            (void)j;
            if (n->greedy) { re->prog[sp].x = body; re->prog[sp].y = re->nprog; }
            else { re->prog[sp].x = re->nprog; re->prog[sp].y = body; }
        } else if (n->max > n->min) {
            int cnt = n->max - n->min;
            int *sps = xmalloc(sizeof(int) * (size_t)cnt);
            for (i = 0; i < cnt; i++) {
                sps[i] = emit(re, I_SPLIT, 0, 0, 0);
                if (n->greedy) re->prog[sps[i]].x = re->nprog;
                else re->prog[sps[i]].y = re->nprog;
                if (gen(re, k) < 0) { free(sps); return -1; }
            }
            for (i = 0; i < cnt; i++) {
                if (n->greedy) re->prog[sps[i]].y = re->nprog;
                else re->prog[sps[i]].x = re->nprog;
            }
            free(sps);
        }
        break;
    }
    }
    return 0;
}

Regex *re_compile(const char *pattern, int flags, char **err) {
    Parser ps;
    Regex *re = xcalloc(1, sizeof *re);
    Node *root;
    int f = flags & RE_ICASE;
    memset(&ps, 0, sizeof ps);
    ps.p = ps.start = pattern;
    ps.re = re;
    ps.ncap = 1;
    if (err) *err = NULL;
    if (flags & RE_LITERAL) {
        const char *p = pattern;
        root = new_node(N_CAT);
        while (*p) {
            int cp, k = utf8_decode(p, (int)strlen(p), &cp);
            Node *ch = new_node(N_CHAR);
            ch->cp = cp;
            ch->icase = (flags & RE_ICASE) != 0;
            add_kid(root, ch);
            p += k;
        }
    } else {
        root = parse_alt(&ps, &f);
        if (root && *ps.p == ')') { set_err(&ps, "unexpected )"); free_node(root); root = NULL; }
    }
    if (!root) {
        if (err) *err = ps.err ? ps.err : xstrdup("invalid regex");
        else free(ps.err);
        re_free(re);
        return NULL;
    }
    re->ncap = ps.ncap;
    emit(re, I_SAVE, 0, 0, 0);
    if (gen(re, root) < 0) {
        free_node(root);
        if (err) *err = xstrdup("regex too large");
        re_free(re);
        return NULL;
    }
    emit(re, I_SAVE, 1, 0, 0);
    emit(re, I_MATCH, 0, 0, 0);
    free_node(root);
    /* anchoring / first byte analysis */
    re->firstbyte = -1;
    if (re->nprog > 1) {
        Inst *i1 = &re->prog[1];
        if (i1->op == I_ASSERT && (i1->x == A_BOT || (i1->x == A_BOL && !i1->flag))) re->anchored = 1;
        if (i1->op == I_CHAR) re->firstbyte = i1->x;
    }
    return re;
}

void re_free(Regex *re) {
    int i;
    if (!re) return;
    for (i = 0; i < re->nclass; i++) free(re->classes[i].r);
    free(re->classes);
    free(re->prog);
    free(re);
}

int re_ncaps(const Regex *re) { return re->ncap; }

int re_context_free(const Regex *re) {
    int i;
    for (i = 0; i < re->nprog; i++) {
        const Inst *in = &re->prog[i];
        if (in->op == I_ASSERT && in->x != A_EOL && in->x != A_EOT) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */

static int ascii_word(int c) { return isalnum(c) || c == '_'; }

static int class_match(const Class *c, int cp) {
    int i, hit = 0;
    for (i = 0; i < c->n; i++)
        if (cp >= c->r[i].lo && cp <= c->r[i].hi) { hit = 1; break; }
    if (!hit && c->icase && cp < 128 && isalpha(cp)) {
        int alt = islower(cp) ? toupper(cp) : tolower(cp);
        for (i = 0; i < c->n; i++)
            if (alt >= c->r[i].lo && alt <= c->r[i].hi) { hit = 1; break; }
    }
    return hit != c->neg;
}

static int check_assert(int kind, int ml, const char *s, int len, int pos) {
    switch (kind) {
    case A_BOL: return pos == 0 || (ml && s[pos - 1] == '\n');
    case A_EOL: return pos == len || (ml && s[pos] == '\n');
    case A_BOT: return pos == 0;
    case A_EOT: return pos == len;
    case A_WORDB:
    case A_NWORDB: {
        int a = pos > 0 && ascii_word((unsigned char)s[pos - 1]);
        int b = pos < len && ascii_word((unsigned char)s[pos]);
        return (kind == A_WORDB) == (a != b);
    }
    }
    return 0;
}

typedef struct {
    int pc, pos;
    int capi, capv; /* capi >= 0: restore job */
} Job;

static Job *jobs;
static int njobs_cap;
/* visited (pc, pos) bitset, position-major relative to the search start.
   Only the rows touched by a search are cleared afterwards, so the cost of
   clearing is proportional to the work done. */
static unsigned char *visit;
static size_t visit_cap;   /* bytes */
static int vis_base, vis_maxpos;
static size_t vis_rows; /* positions covered by the table from vis_base */
#define VISIT_MAX_BITS ((size_t)256 * 1024 * 1024)

static int run(const Regex *re, const char *s, int len, int start, int *caps, int use_visit, long *budget) {
    int sp = 0;
    const Inst *prog = re->prog;
#define PUSH(a, b, c, d)                                                       \
    do {                                                                       \
        if (sp == njobs_cap) {                                                 \
            njobs_cap = njobs_cap ? njobs_cap * 2 : 256;                       \
            jobs = xrealloc(jobs, sizeof *jobs * (size_t)njobs_cap);           \
        }                                                                      \
        jobs[sp].pc = (a); jobs[sp].pos = (b); jobs[sp].capi = (c); jobs[sp].capv = (d); sp++; \
    } while (0)
    PUSH(0, start, -1, 0);
    while (sp > 0) {
        Job j = jobs[--sp];
        int pc, pos;
        if (j.capi >= 0) { caps[j.capi] = j.capv; continue; }
        pc = j.pc;
        pos = j.pos;
        for (;;) {
            const Inst *in = &prog[pc];
            if (use_visit && (size_t)(pos - vis_base) < vis_rows) {
                size_t k = (size_t)(pos - vis_base) * (size_t)re->nprog + (size_t)pc;
                unsigned char bit = (unsigned char)(1u << (k & 7));
                if (visit[k >> 3] & bit) break;
                visit[k >> 3] |= bit;
                if (pos > vis_maxpos) vis_maxpos = pos;
            } else if (--*budget < 0) {
                return 0;
            }
            switch (in->op) {
            case I_CHAR:
                if (pos < len && (unsigned char)s[pos] == in->x) { pos++; pc++; continue; }
                goto fail;
            case I_CHARI:
                if (pos < len && tolower((unsigned char)s[pos]) == in->x) { pos++; pc++; continue; }
                goto fail;
            case I_ANY:
            case I_ANYNL: {
                int cp, k;
                if (pos >= len) goto fail;
                if (in->op == I_ANY && s[pos] == '\n') goto fail;
                k = utf8_decode(s + pos, len - pos, &cp);
                pos += k;
                pc++;
                continue;
            }
            case I_CLASS: {
                int cp, k;
                if (pos >= len) goto fail;
                k = utf8_decode(s + pos, len - pos, &cp);
                if (!class_match(&re->classes[in->x], cp)) goto fail;
                pos += k;
                pc++;
                continue;
            }
            case I_SPLIT:
                PUSH(in->y, pos, -1, 0);
                pc = in->x;
                continue;
            case I_JMP:
                pc = in->x;
                continue;
            case I_SAVE:
                if (in->x < 2 * re->ncap) {
                    PUSH(0, 0, in->x, caps[in->x]);
                    caps[in->x] = pos;
                }
                pc++;
                continue;
            case I_ASSERT:
                if (check_assert(in->x, in->flag, s, len, pos)) { pc++; continue; }
                goto fail;
            case I_MATCH:
                return 1;
            }
        fail:
            break;
        }
    }
    return 0;
#undef PUSH
}

int re_search(const Regex *re, const char *s, int len, int start, int *caps, int ncap) {
    int localcaps[20];
    int *cp = caps;
    int n = 2 * re->ncap, i, use_visit, found = 0;
    size_t cells = (size_t)re->nprog * ((size_t)(len - start) + 1);
    long budget = 50000000;
    if (start > len) return 0;
    if (!caps || ncap < re->ncap) {
        if (n <= 20) cp = localcaps;
        else cp = xmalloc(sizeof(int) * (size_t)n);
    }
    for (i = 0; i < n; i++) cp[i] = -1;
    /* Normally one table covers the whole search.  For huge inputs the table
       is a window rebased at every start position (no sharing between start
       positions, but still linear per attempt). */
    use_visit = start >= 0;
    {
        int windowed = cells > VISIT_MAX_BITS;
        size_t bytes = (windowed ? VISIT_MAX_BITS : cells) / 8 + 1;
        vis_rows = windowed ? VISIT_MAX_BITS / (size_t)re->nprog : (size_t)(len - start) + 1;
        if (vis_rows == 0) vis_rows = 1;
        if (bytes > visit_cap) {
            visit = xrealloc(visit, bytes);
            memset(visit + visit_cap, 0, bytes - visit_cap);
            visit_cap = bytes;
        }
        vis_base = start;
        vis_maxpos = start - 1;
        (void)windowed;
    }
    for (i = start; i <= len;) {
        if (re->firstbyte >= 0) {
            const char *f = memchr(s + i, re->firstbyte, (size_t)(len - i));
            if (!f) break;
            i = (int)(f - s);
        }
        if (cells > VISIT_MAX_BITS) {
            /* windowed: rebase on this start position */
            if (vis_maxpos >= vis_base) {
                size_t endbit = (size_t)(vis_maxpos - vis_base + 1) * (size_t)re->nprog;
                memset(visit, 0, endbit / 8 + 1 < visit_cap ? endbit / 8 + 1 : visit_cap);
            }
            vis_base = i;
            vis_maxpos = i - 1;
        }
        if (run(re, s, len, i, cp, use_visit, &budget)) {
            found = 1;
            break;
        }
        if (re->anchored || budget < 0) break;
        if (i >= len) break;
        i = utf8_next(s, len, i);
    }
    if (use_visit && vis_maxpos >= vis_base) {
        size_t endbit = (size_t)(vis_maxpos - vis_base + 1) * (size_t)re->nprog;
        memset(visit, 0, endbit / 8 + 1 < visit_cap ? endbit / 8 + 1 : visit_cap);
    }
    if (found && cp != caps && caps)
        for (n = 0; n < 2 * ncap && n < 2 * re->ncap; n++) caps[n] = cp[n];
    if (cp != caps && cp != localcaps) free(cp);
    return found;
}

int re_match_str(const Regex *re, const char *s) {
    return re_search(re, s, (int)strlen(s), 0, NULL, 0);
}

char *re_quote(const char *s) {
    Str o;
    str_init(&o);
    str_reserve(&o, 0);
    for (; *s; s++) {
        if (strchr("\\.+*?()|[]{}^$", *s)) str_putc(&o, '\\');
        str_putc(&o, *s);
    }
    return str_detach(&o);
}
