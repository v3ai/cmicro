#include "conf.h"
#include "platform.h"
#include "util.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nesting limits.  Both parsers are recursive; bounding the depth bounds
   the stack they (and cnode_free / the JSON writer) can use, whatever the
   input looks like (e.g. 1e6 '[' characters, or "- - - - ..." lines). */
#define YAML_MAX_DEPTH 256
#define JSON_MAX_DEPTH 100

static CNode *cn_new(int type) {
    CNode *n = xcalloc(1, sizeof *n);
    n->type = type;
    return n;
}

static void cn_push(CNode *n, char *key, CNode *v) {
    if (n->n >= n->cap) {
        int cap = n->n < 8 ? 8 : n->n;
        if (cap > INT_MAX / 2) { fputs("micro: out of memory\n", stderr); exit(1); }
        cap *= 2;
        n->vals = xrealloc(n->vals, sizeof *n->vals * (size_t)cap);
        if (n->type == CN_MAP) n->keys = xrealloc(n->keys, sizeof *n->keys * (size_t)cap);
        n->cap = cap;
    }
    if (n->type == CN_MAP) n->keys[n->n] = key;
    else free(key);
    n->vals[n->n++] = v;
}

void cnode_free(CNode *n) {
    int i;
    if (!n) return;
    for (i = 0; i < n->n; i++) {
        if (n->type == CN_MAP && n->keys) free(n->keys[i]);
        cnode_free(n->vals[i]);
    }
    free(n->keys);
    free(n->vals);
    free(n->str);
    free(n);
}

CNode *cnode_get(const CNode *map, const char *key) {
    int i;
    if (!map || map->type != CN_MAP || !key || !map->keys) return NULL;
    for (i = map->n - 1; i >= 0; i--)
        if (map->keys[i] && !strcmp(map->keys[i], key)) return map->vals[i];
    return NULL;
}

const char *cnode_str(const CNode *map, const char *key) {
    CNode *v = cnode_get(map, key);
    return v && v->type == CN_STR ? v->str : NULL;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* read up to `max` hex digits after *s (s points at the escape letter);
   advances *s to the last digit consumed.  Values that don't fit a code
   point saturate (they're replaced by U+FFFD when encoded). */
static long read_hex(const char **s, int max) {
    long v = 0;
    int i, d;
    for (i = 0; i < max && (d = hexval((unsigned char)(*s)[1])) >= 0; i++) {
        (*s)++;
        if (v <= 0x10FFFF) v = v * 16 + d;
    }
    return v;
}

/* ================================================================== */
/* YAML subset                                                         */

typedef struct {
    int indent;
    char *text; /* content without indentation and comments, trailing ws stripped */
} YLine;

typedef struct {
    YLine *lines;
    int n, pos;
    char *err;
    int depth;            /* yaml_block nesting */
    int skip;             /* > 0: walk the structure without building nodes */
    const char *skip_key; /* root-level key whose value is skipped */
} YParser;

static int is_item(const char *t) { return t[0] == '-' && (t[1] == ' ' || t[1] == 0); }

/* strip a trailing comment that is outside quotes */
static void strip_comment(char *s) {
    int dq = 0, sq = 0;
    char *p = s;
    for (;;) {
        /* jump over the runs of bytes that can't change the state */
        p += strcspn(p, dq ? "\\\"" : sq ? "'" : "\"'#");
        if (!*p) break;
        if (dq) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') dq = 0;
        } else if (sq) {
            if (p[1] == '\'') p++;
            else sq = 0;
        } else if (*p == '"') dq = 1;
        else if (*p == '\'') sq = 1;
        else if (p == s || p[-1] == ' ' || p[-1] == '\t') { *p = 0; break; } /* '#' */
        p++;
    }
    while (p > s && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r')) *--p = 0;
}

/* Decode a scalar.  The result is never longer than the input (every
   escape sequence is at least as long as its UTF-8 encoding), so it is
   written straight into one allocation. */
static char *yaml_scalar(const char *s) {
    char *o, *out;
    while (*s == ' ') s++;
    out = o = xmalloc(strlen(s) + 1);
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1]) {
                s++;
                switch (*s) {
                case 'n': *o++ = '\n'; break;
                case 't': *o++ = '\t'; break;
                case 'r': *o++ = '\r'; break;
                case '0': *o++ = '\0'; break;
                case 'x': case 'u': case 'U': {
                    long v = read_hex(&s, *s == 'x' ? 2 : *s == 'u' ? 4 : 8);
                    o += utf8_encode(v > 0x10FFFF ? 0xFFFD : (int)v, o);
                    break;
                }
                default: *o++ = *s; break;
                }
                s++;
            } else *o++ = *s++;
        }
    } else if (*s == '\'') {
        s++;
        while (*s) {
            if (*s == '\'') {
                if (s[1] == '\'') { *o++ = '\''; s += 2; continue; }
                break;
            }
            *o++ = *s++;
        }
    } else {
        size_t n = strlen(s);
        memcpy(o, s, n);
        o += n;
    }
    *o = 0;
    return out;
}

/* find "key: value" separator outside quotes; returns index of ':' or -1 */
static int find_colon(const char *s) {
    int i = 0;
    if (s[0] == '"' || s[0] == '\'') {
        char q = s[0];
        for (i = 1; s[i]; i++) {
            if (q == '"' && s[i] == '\\' && s[i + 1]) { i++; continue; }
            if (s[i] == q) {
                if (q == '\'' && s[i + 1] == '\'') { i++; continue; }
                i++;
                break;
            }
        }
    }
    for (;;) {
        const char *c = strchr(s + i, ':');
        if (!c) return -1;
        i = (int)(c - s);
        if (s[i + 1] == 0 || s[i + 1] == ' ') return i;
        i++;
    }
}

static CNode *yaml_block(YParser *p, int indent);

static CNode *yaml_value_inline(const char *v) {
    while (*v == ' ') v++;
    if (*v == '[') {
        /* simple flow list of scalars */
        CNode *l = cn_new(CN_LIST);
        const char *q = v + 1;
        while (*q && *q != ']') {
            const char *e;
            char *item;
            while (*q == ' ' || *q == ',') q++;
            if (*q == ']' || !*q) break;
            e = q;
            if (*e == '"' || *e == '\'') {
                char c = *e++;
                while (*e && *e != c) { if (c == '"' && *e == '\\' && e[1]) e++; e++; }
                if (*e) e++;
            } else {
                while (*e && *e != ',' && *e != ']') e++;
            }
            item = xstrndup(q, (size_t)(e - q));
            {
                CNode *sn = cn_new(CN_STR);
                sn->str = yaml_scalar(item);
                cn_push(l, NULL, sn);
            }
            free(item);
            q = e;
        }
        return l;
    }
    {
        CNode *sn = cn_new(CN_STR);
        sn->str = yaml_scalar(v);
        return sn;
    }
}

static CNode *yaml_map(YParser *p, int indent) {
    CNode *m = p->skip ? NULL : cn_new(CN_MAP);
    int root = p->depth == 1;
    while (p->pos < p->n && p->lines[p->pos].indent == indent) {
        YLine *l = &p->lines[p->pos];
        int c, skipping;
        char *key, *rest;
        CNode *v;
        if (is_item(l->text)) break;
        c = find_colon(l->text);
        if (c < 0) {
            if (!p->err) p->err = xsprintf("expected 'key: value' in '%s'", l->text);
            p->pos++;
            continue;
        }
        l->text[c] = 0;
        key = p->skip ? NULL : yaml_scalar(l->text);
        skipping = root && key && p->skip_key && !strcmp(key, p->skip_key);
        if (skipping) p->skip++;
        rest = l->text + c + 1;
        while (*rest == ' ') rest++;
        p->pos++;
        if (*rest) {
            v = p->skip ? NULL : yaml_value_inline(rest);
        } else if (p->pos < p->n && (p->lines[p->pos].indent > indent ||
                                     (p->lines[p->pos].indent == indent && is_item(p->lines[p->pos].text)))) {
            v = yaml_block(p, p->lines[p->pos].indent);
        } else {
            v = p->skip ? NULL : cn_new(CN_NULL);
        }
        if (skipping) {
            p->skip--;
            v = cn_new(CN_NULL);
        }
        if (m) cn_push(m, key, v);
    }
    return m;
}

static CNode *yaml_list(YParser *p, int indent) {
    CNode *l = p->skip ? NULL : cn_new(CN_LIST);
    while (p->pos < p->n && p->lines[p->pos].indent == indent) {
        YLine *ln = &p->lines[p->pos];
        char *rest;
        int off;
        CNode *v;
        if (!is_item(ln->text)) break;
        rest = ln->text + 1;
        off = 1;
        while (*rest == ' ') { rest++; off++; }
        if (!*rest) {
            p->pos++;
            if (p->pos < p->n && p->lines[p->pos].indent > indent)
                v = yaml_block(p, p->lines[p->pos].indent);
            else
                v = p->skip ? NULL : cn_new(CN_NULL);
        } else if (find_colon(rest) >= 0 || (rest[0] == '-' && rest[1] == ' ')) {
            /* rewrite this line as a nested block starting at the item's column */
            memmove(ln->text, rest, strlen(rest) + 1);
            ln->indent = indent + off;
            v = yaml_block(p, ln->indent);
        } else {
            v = p->skip ? NULL : yaml_value_inline(rest);
            p->pos++;
        }
        if (l) cn_push(l, NULL, v);
    }
    return l;
}

static CNode *yaml_block(YParser *p, int indent) {
    YLine *l = &p->lines[p->pos];
    CNode *r;
    if (p->depth >= YAML_MAX_DEPTH) {
        if (!p->err) p->err = xstrdup("nesting too deep");
        p->pos = p->n; /* give up on the rest: guarantees progress */
        return p->skip ? NULL : cn_new(CN_NULL);
    }
    p->depth++;
    if (is_item(l->text)) {
        r = yaml_list(p, indent);
    } else if (find_colon(l->text) < 0) {
        /* multi-line plain/quoted scalar: join lines at this indentation */
        Str s;
        str_init(&s);
        str_reserve(&s, 0);
        while (p->pos < p->n && p->lines[p->pos].indent >= indent &&
               find_colon(p->lines[p->pos].text) < 0) {
            if (!p->skip) {
                if (s.len) str_putc(&s, ' ');
                str_puts(&s, p->lines[p->pos].text);
            }
            p->pos++;
        }
        r = NULL;
        if (!p->skip) {
            r = cn_new(CN_STR);
            r->str = yaml_scalar(s.s);
        }
        str_free(&s);
    } else {
        r = yaml_map(p, indent);
    }
    p->depth--;
    return r;
}

static CNode *yaml_parse_impl(const char *text, const char *skip_key, char **err) {
    YParser p;
    CNode *root;
    char *buf, *s;
    int cap = 0;
    memset(&p, 0, sizeof p);
    p.skip_key = skip_key;
    /* one copy of the text; lines are NUL-terminated in place */
    buf = s = xstrdup(text ? text : "");
    while (*s) {
        char *e = strchr(s, '\n');
        char *line = s;
        int ind = 0;
        if (e) *e = 0;
        while (line[ind] == ' ') ind++;
        strip_comment(line + ind);
        if (line[ind] && strcmp(line + ind, "---") != 0) {
            if (p.n == cap) {
                if (cap > INT_MAX / 2) { fputs("micro: out of memory\n", stderr); exit(1); }
                cap = cap ? cap * 2 : 64;
                p.lines = xrealloc(p.lines, sizeof *p.lines * (size_t)cap);
            }
            p.lines[p.n].indent = ind;
            p.lines[p.n].text = line + ind;
            p.n++;
        }
        if (!e) break;
        s = e + 1;
    }
    if (p.n == 0) root = cn_new(CN_MAP);
    else root = yaml_block(&p, p.lines[0].indent);
    if (p.pos < p.n && !p.err) p.err = xsprintf("unexpected indentation near '%s'", p.lines[p.pos].text);
    free(p.lines);
    free(buf);
    if (err) *err = p.err;
    else free(p.err);
    return root;
}

CNode *yaml_parse(const char *text, char **err) { return yaml_parse_impl(text, NULL, err); }

CNode *yaml_parse_skip(const char *text, const char *skip_key, char **err) {
    return yaml_parse_impl(text, skip_key, err);
}

/* ================================================================== */
/* JSON (lenient)                                                      */

typedef struct {
    const char *p;
    char *err;
} JParser;

static void j_ws(JParser *j) {
    for (;;) {
        while (*j->p && isspace((unsigned char)*j->p)) j->p++;
        if (j->p[0] == '/' && j->p[1] == '/') {
            while (*j->p && *j->p != '\n') j->p++;
        } else if (j->p[0] == '/' && j->p[1] == '*') {
            const char *e = strstr(j->p + 2, "*/");
            j->p = e ? e + 2 : j->p + strlen(j->p);
        } else break;
    }
}

/* \uXXXX (s points at the 'u'); exactly 4 digits or -1 */
static long j_hex4(const char *s) {
    long v = 0;
    int i, d;
    for (i = 1; i <= 4; i++) {
        if ((d = hexval((unsigned char)s[i])) < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

static char *j_string(JParser *j) {
    Str o;
    char q = *j->p++;
    str_init(&o);
    str_reserve(&o, 0);
    while (*j->p && *j->p != q) {
        const char *run = j->p;
        while (*j->p && *j->p != q && *j->p != '\\') j->p++;
        if (j->p > run) { str_putn(&o, run, (int)(j->p - run)); continue; }
        if (*j->p == '\\' && j->p[1]) {
            j->p++;
            switch (*j->p) {
            case 'n': str_putc(&o, '\n'); break;
            case 't': str_putc(&o, '\t'); break;
            case 'r': str_putc(&o, '\r'); break;
            case 'b': str_putc(&o, '\b'); break;
            case 'f': str_putc(&o, '\f'); break;
            case 'u': {
                char buf[4];
                long v = j_hex4(j->p), lo;
                if (v < 0) {
                    /* short escape: take what hex digits there are */
                    v = read_hex(&j->p, 4);
                } else {
                    j->p += 4;
                    if (v >= 0xD800 && v <= 0xDBFF && j->p[1] == '\\' && j->p[2] == 'u' &&
                        (lo = j_hex4(j->p + 2)) >= 0xDC00 && lo <= 0xDFFF) {
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                }
                if (v >= 0xD800 && v <= 0xDFFF) v = 0xFFFD; /* unpaired surrogate */
                str_putn(&o, buf, utf8_encode((int)v, buf));
                break;
            }
            default: str_putc(&o, *j->p); break;
            }
            j->p++;
        } else str_putc(&o, *j->p++);
    }
    if (*j->p == q) j->p++;
    else if (!j->err) j->err = xstrdup("unterminated string");
    return str_detach(&o);
}

static CNode *j_value(JParser *j, int depth) {
    CNode *n;
    j_ws(j);
    if (depth > JSON_MAX_DEPTH) { if (!j->err) j->err = xstrdup("nesting too deep"); return cn_new(CN_NULL); }
    if (*j->p == '{') {
        n = cn_new(CN_MAP);
        j->p++;
        for (;;) {
            char *k;
            j_ws(j);
            if (*j->p == '}') { j->p++; break; }
            if (*j->p == '"' || *j->p == '\'') k = j_string(j);
            else {
                const char *s = j->p;
                while (*j->p && (isalnum((unsigned char)*j->p) || *j->p == '_' || *j->p == '$')) j->p++;
                if (s == j->p) { if (!j->err) j->err = xstrdup("expected key"); break; }
                k = xstrndup(s, (size_t)(j->p - s));
            }
            j_ws(j);
            if (*j->p != ':') { free(k); if (!j->err) j->err = xstrdup("expected ':'"); break; }
            j->p++;
            cn_push(n, k, j_value(j, depth + 1));
            if (j->err) break;
            j_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; break; }
            if (!j->err) j->err = xstrdup("expected ',' or '}'");
            break;
        }
        return n;
    }
    if (*j->p == '[') {
        n = cn_new(CN_LIST);
        j->p++;
        for (;;) {
            j_ws(j);
            if (*j->p == ']') { j->p++; break; }
            cn_push(n, NULL, j_value(j, depth + 1));
            if (j->err) break;
            j_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; break; }
            if (!j->err) j->err = xstrdup("expected ',' or ']'");
            break;
        }
        return n;
    }
    if (*j->p == '"' || *j->p == '\'') {
        n = cn_new(CN_STR);
        n->str = j_string(j);
        return n;
    }
    if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "false", 5)) {
        n = cn_new(CN_BOOL);
        n->b = j->p[0] == 't';
        n->str = xstrdup(n->b ? "true" : "false");
        j->p += n->b ? 4 : 5;
        return n;
    }
    if (!strncmp(j->p, "null", 4)) {
        j->p += 4;
        return cn_new(CN_NULL);
    }
    if (*j->p == '-' || *j->p == '+' || *j->p == '.' || isdigit((unsigned char)*j->p)) {
        char *end;
        n = cn_new(CN_NUM);
        n->num = strtod(j->p, &end);
        n->str = xstrndup(j->p, (size_t)(end - j->p));
        if (end == j->p) { if (!j->err) j->err = xstrdup("bad number"); }
        j->p = end;
        return n;
    }
    if (!j->err) j->err = xsprintf("unexpected character '%c'", *j->p ? *j->p : '?');
    return cn_new(CN_NULL);
}

CNode *json_parse(const char *text, char **err) {
    JParser j;
    CNode *n;
    j.p = text ? text : "";
    j.err = NULL;
    n = j_value(&j, 0);
    j_ws(&j);
    if (!j.err && *j.p) j.err = xstrdup("trailing characters");
    if (err) *err = j.err;
    else free(j.err);
    return n;
}

/* ================================================================== */
/* Writing / editing                                                   */

CNode *cnode_new(int type) { return cn_new(type); }

static void cn_remove_at(CNode *map, int i) {
    free(map->keys[i]);
    cnode_free(map->vals[i]);
    memmove(map->keys + i, map->keys + i + 1, sizeof(char *) * (size_t)(map->n - i - 1));
    memmove(map->vals + i, map->vals + i + 1, sizeof(CNode *) * (size_t)(map->n - i - 1));
    map->n--;
}

/* A map read from a file can hold a key more than once; cnode_get (and
   everything applying a map in order) lets the last one win. So set
   replaces the last one and drops the earlier ones, and del removes them
   all: otherwise the value written back would be shadowed on reload. */
void cnode_set(CNode *map, const char *key, CNode *val) {
    int i, last = -1;
    if (!map || map->type != CN_MAP || !key) { cnode_free(val); return; }
    for (i = map->n - 1; i >= 0; i--) {
        if (map->keys[i] && !strcmp(map->keys[i], key)) { last = i; break; }
    }
    if (last < 0) { cn_push(map, xstrdup(key), val); return; }
    cnode_free(map->vals[last]);
    map->vals[last] = val;
    for (i = last - 1; i >= 0; i--)
        if (map->keys[i] && !strcmp(map->keys[i], key)) cn_remove_at(map, i);
}

void cnode_del(CNode *map, const char *key) {
    int i;
    if (!map || map->type != CN_MAP || !key) return;
    for (i = map->n - 1; i >= 0; i--)
        if (map->keys[i] && !strcmp(map->keys[i], key)) cn_remove_at(map, i);
}

static void jw_str(Str *o, const char *s) {
    str_putc(o, '"');
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') { str_putc(o, '\\'); str_putc(o, *s); }
        else if (*s == '\n') str_putn(o, "\\n", 2);
        else if (*s == '\t') str_putn(o, "\\t", 2);
        else if ((unsigned char)*s < 32) str_printf(o, "\\u%04x", (unsigned)(unsigned char)*s);
        else str_putc(o, *s);
    }
    str_putc(o, '"');
}

static void jw_indent(Str *o, int n) {
    while (n-- > 0) str_putc(o, ' ');
}

static void jw(Str *o, const CNode *n, int ind) {
    int i;
    if (!n) { str_putn(o, "null", 4); return; }
    switch (n->type) {
    case CN_MAP:
        str_putn(o, "{\n", 2);
        for (i = 0; i < n->n; i++) {
            jw_indent(o, ind + 4);
            jw_str(o, n->keys ? n->keys[i] : "");
            str_putn(o, ": ", 2);
            jw(o, n->vals[i], ind + 4);
            str_putn(o, i + 1 < n->n ? ",\n" : "\n", i + 1 < n->n ? 2 : 1);
        }
        jw_indent(o, ind);
        str_putc(o, '}');
        break;
    case CN_LIST:
        str_putc(o, '[');
        for (i = 0; i < n->n; i++) {
            if (i) str_putn(o, ", ", 2);
            jw(o, n->vals[i], ind);
        }
        str_putc(o, ']');
        break;
    case CN_STR: jw_str(o, n->str); break;
    case CN_NUM: {
        const char *v = n->str && *n->str ? n->str : "0";
        str_putn(o, v, (int)strlen(v));
        break;
    }
    case CN_BOOL: str_putn(o, n->b ? "true" : "false", n->b ? 4 : 5); break;
    default: str_putn(o, "null", 4);
    }
}

/* Written atomically (temp file + rename, see plat_save_open), so a
   crash or full disk never leaves a truncated bindings.json behind. */
int json_write_file(const char *path, const CNode *n) {
    Str o;
    PlatSave *f;
    int denied = 0, rc;
    str_init(&o);
    jw(&o, n, 0);
    str_putc(&o, '\n');
    f = plat_save_open(path, (long long)o.len, 0, &denied);
    if (!f) { str_free(&o); return -1; }
    rc = plat_save_write(f, o.s, (size_t)o.len);
    rc = plat_save_close(f, rc == 0) == 0 && rc == 0 ? 0 : -1;
    str_free(&o);
    return rc;
}
