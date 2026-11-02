#include "conf.h"
#include "platform.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static CNode *cn_new(int type) {
    CNode *n = xcalloc(1, sizeof *n);
    n->type = type;
    return n;
}

static void cn_push(CNode *n, char *key, CNode *v) {
    if (n->n == n->cap) {
        n->cap = n->cap ? n->cap * 2 : 8;
        n->vals = xrealloc(n->vals, sizeof *n->vals * (size_t)n->cap);
        if (n->type == CN_MAP) n->keys = xrealloc(n->keys, sizeof *n->keys * (size_t)n->cap);
    }
    if (n->type == CN_MAP) n->keys[n->n] = key;
    n->vals[n->n++] = v;
}

void cnode_free(CNode *n) {
    int i;
    if (!n) return;
    for (i = 0; i < n->n; i++) {
        if (n->type == CN_MAP) free(n->keys[i]);
        cnode_free(n->vals[i]);
    }
    free(n->keys);
    free(n->vals);
    free(n->str);
    free(n);
}

CNode *cnode_get(const CNode *map, const char *key) {
    int i;
    if (!map || map->type != CN_MAP) return NULL;
    for (i = map->n - 1; i >= 0; i--)
        if (!strcmp(map->keys[i], key)) return map->vals[i];
    return NULL;
}

const char *cnode_str(const CNode *map, const char *key) {
    CNode *v = cnode_get(map, key);
    return v && v->type == CN_STR ? v->str : NULL;
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
} YParser;

/* strip a trailing comment that is outside quotes */
static void strip_comment(char *s) {
    int dq = 0, sq = 0;
    char *p;
    for (p = s; *p; p++) {
        if (dq) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') dq = 0;
        } else if (sq) {
            if (*p == '\'') {
                if (p[1] == '\'') p++;
                else sq = 0;
            }
        } else if (*p == '"') dq = 1;
        else if (*p == '\'') sq = 1;
        else if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
    }
    p = s + strlen(s);
    while (p > s && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r')) *--p = 0;
}

static char *yaml_scalar(const char *s) {
    Str o;
    str_init(&o);
    str_reserve(&o, 0);
    while (*s == ' ') s++;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1]) {
                s++;
                switch (*s) {
                case 'n': str_putc(&o, '\n'); break;
                case 't': str_putc(&o, '\t'); break;
                case 'r': str_putc(&o, '\r'); break;
                case '0': str_putc(&o, '\0'); break;
                case 'x': case 'u': case 'U': {
                    int len = *s == 'x' ? 2 : *s == 'u' ? 4 : 8, v = 0, i;
                    char buf[4];
                    for (i = 0; i < len && isxdigit((unsigned char)s[1]); i++) {
                        s++;
                        v = v * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10));
                    }
                    str_putn(&o, buf, utf8_encode(v, buf));
                    break;
                }
                default: str_putc(&o, *s); break;
                }
                s++;
            } else str_putc(&o, *s++);
        }
    } else if (*s == '\'') {
        s++;
        while (*s) {
            if (*s == '\'') {
                if (s[1] == '\'') { str_putc(&o, '\''); s += 2; continue; }
                break;
            }
            str_putc(&o, *s++);
        }
    } else {
        str_puts(&o, s);
    }
    return str_detach(&o);
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
    for (; s[i]; i++)
        if (s[i] == ':' && (s[i + 1] == 0 || s[i + 1] == ' ')) return i;
    return -1;
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
    CNode *m = cn_new(CN_MAP);
    while (p->pos < p->n && p->lines[p->pos].indent == indent) {
        YLine *l = &p->lines[p->pos];
        int c;
        char *key, *rest;
        if (l->text[0] == '-' && (l->text[1] == ' ' || l->text[1] == 0)) break;
        c = find_colon(l->text);
        if (c < 0) {
            if (!p->err) p->err = xsprintf("expected 'key: value' in '%s'", l->text);
            p->pos++;
            continue;
        }
        l->text[c] = 0;
        key = yaml_scalar(l->text);
        rest = l->text + c + 1;
        while (*rest == ' ') rest++;
        p->pos++;
        if (*rest) {
            cn_push(m, key, yaml_value_inline(rest));
        } else if (p->pos < p->n && (p->lines[p->pos].indent > indent ||
                                     (p->lines[p->pos].indent == indent && p->lines[p->pos].text[0] == '-' &&
                                      (p->lines[p->pos].text[1] == ' ' || p->lines[p->pos].text[1] == 0)))) {
            cn_push(m, key, yaml_block(p, p->lines[p->pos].indent));
        } else {
            cn_push(m, key, cn_new(CN_NULL));
        }
    }
    return m;
}

static CNode *yaml_list(YParser *p, int indent) {
    CNode *l = cn_new(CN_LIST);
    while (p->pos < p->n && p->lines[p->pos].indent == indent) {
        YLine *ln = &p->lines[p->pos];
        char *rest;
        int off;
        if (!(ln->text[0] == '-' && (ln->text[1] == ' ' || ln->text[1] == 0))) break;
        rest = ln->text + 1;
        off = 1;
        while (*rest == ' ') { rest++; off++; }
        if (!*rest) {
            p->pos++;
            if (p->pos < p->n && p->lines[p->pos].indent > indent)
                cn_push(l, NULL, yaml_block(p, p->lines[p->pos].indent));
            else
                cn_push(l, NULL, cn_new(CN_NULL));
            continue;
        }
        if (find_colon(rest) >= 0 || (rest[0] == '-' && rest[1] == ' ')) {
            /* rewrite this line as a nested block starting at the item's column */
            memmove(ln->text, rest, strlen(rest) + 1);
            ln->indent = indent + off;
            cn_push(l, NULL, yaml_block(p, ln->indent));
        } else {
            cn_push(l, NULL, yaml_value_inline(rest));
            p->pos++;
        }
    }
    return l;
}

static CNode *yaml_block(YParser *p, int indent) {
    YLine *l = &p->lines[p->pos];
    if (l->text[0] == '-' && (l->text[1] == ' ' || l->text[1] == 0)) return yaml_list(p, indent);
    if (find_colon(l->text) < 0) {
        /* multi-line plain/quoted scalar: join lines at this indentation */
        Str s;
        CNode *n;
        str_init(&s);
        str_reserve(&s, 0);
        while (p->pos < p->n && p->lines[p->pos].indent >= indent &&
               find_colon(p->lines[p->pos].text) < 0) {
            if (s.len) str_putc(&s, ' ');
            str_puts(&s, p->lines[p->pos].text);
            p->pos++;
        }
        n = cn_new(CN_STR);
        n->str = yaml_scalar(s.s);
        str_free(&s);
        return n;
    }
    return yaml_map(p, indent);
}

CNode *yaml_parse(const char *text, char **err) {
    YParser p;
    CNode *root;
    const char *s = text;
    int i;
    memset(&p, 0, sizeof p);
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char *line = xstrndup(s, len);
        int ind = 0;
        while (line[ind] == ' ') ind++;
        strip_comment(line + ind);
        if (line[ind] && strcmp(line + ind, "---") != 0) {
            if (p.n % 64 == 0) p.lines = xrealloc(p.lines, sizeof *p.lines * (size_t)(p.n + 64));
            p.lines[p.n].indent = ind;
            p.lines[p.n].text = xstrdup(line + ind);
            p.n++;
        }
        free(line);
        s = e ? e + 1 : s + len;
    }
    if (p.n == 0) root = cn_new(CN_MAP);
    else root = yaml_block(&p, p.lines[0].indent);
    if (p.pos < p.n && !p.err) p.err = xsprintf("unexpected indentation near '%s'", p.lines[p.pos].text);
    for (i = 0; i < p.n; i++) free(p.lines[i].text);
    free(p.lines);
    if (err) *err = p.err;
    else free(p.err);
    return root;
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

static char *j_string(JParser *j) {
    Str o;
    char q = *j->p++;
    str_init(&o);
    str_reserve(&o, 0);
    while (*j->p && *j->p != q) {
        if (*j->p == '\\' && j->p[1]) {
            j->p++;
            switch (*j->p) {
            case 'n': str_putc(&o, '\n'); break;
            case 't': str_putc(&o, '\t'); break;
            case 'r': str_putc(&o, '\r'); break;
            case 'b': str_putc(&o, '\b'); break;
            case 'f': str_putc(&o, '\f'); break;
            case 'u': {
                int v = 0, i;
                char buf[4];
                for (i = 0; i < 4 && isxdigit((unsigned char)j->p[1]); i++) {
                    j->p++;
                    v = v * 16 + (isdigit((unsigned char)*j->p) ? *j->p - '0' : tolower((unsigned char)*j->p) - 'a' + 10);
                }
                str_putn(&o, buf, utf8_encode(v, buf));
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
    if (depth > 100) { if (!j->err) j->err = xstrdup("nesting too deep"); return cn_new(CN_NULL); }
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
        if (end == j->p) { if (!j->err) j->err = xstrdup("bad number"); j->p++; }
        j->p = end;
        return n;
    }
    if (!j->err) j->err = xsprintf("unexpected character '%c'", *j->p ? *j->p : '?');
    return cn_new(CN_NULL);
}

CNode *json_parse(const char *text, char **err) {
    JParser j;
    CNode *n;
    j.p = text;
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

#include <stdio.h>

CNode *cnode_new(int type) { return cn_new(type); }

void cnode_set(CNode *map, const char *key, CNode *val) {
    int i;
    for (i = 0; i < map->n; i++) {
        if (!strcmp(map->keys[i], key)) {
            cnode_free(map->vals[i]);
            map->vals[i] = val;
            return;
        }
    }
    cn_push(map, xstrdup(key), val);
}

void cnode_del(CNode *map, const char *key) {
    int i;
    for (i = 0; i < map->n; i++) {
        if (!strcmp(map->keys[i], key)) {
            free(map->keys[i]);
            cnode_free(map->vals[i]);
            memmove(map->keys + i, map->keys + i + 1, sizeof(char *) * (size_t)(map->n - i - 1));
            memmove(map->vals + i, map->vals + i + 1, sizeof(CNode *) * (size_t)(map->n - i - 1));
            map->n--;
            return;
        }
    }
}

static void jw_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\t') fputs("\\t", f);
        else if ((unsigned char)*s < 32) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void jw(FILE *f, const CNode *n, int ind) {
    int i;
    switch (n->type) {
    case CN_MAP:
        fputs("{\n", f);
        for (i = 0; i < n->n; i++) {
            fprintf(f, "%*s", ind + 4, "");
            jw_str(f, n->keys[i]);
            fputs(": ", f);
            jw(f, n->vals[i], ind + 4);
            fputs(i + 1 < n->n ? ",\n" : "\n", f);
        }
        fprintf(f, "%*s}", ind, "");
        break;
    case CN_LIST:
        fputc('[', f);
        for (i = 0; i < n->n; i++) {
            if (i) fputs(", ", f);
            jw(f, n->vals[i], ind);
        }
        fputc(']', f);
        break;
    case CN_STR: jw_str(f, n->str); break;
    case CN_NUM: fputs(n->str ? n->str : "0", f); break;
    case CN_BOOL: fputs(n->b ? "true" : "false", f); break;
    default: fputs("null", f);
    }
}

int json_write_file(const char *path, const CNode *n) {
    FILE *f = plat_fopen(path, "w");
    if (!f) return -1;
    jw(f, n, 0);
    fputc('\n', f);
    return fclose(f) == 0 ? 0 : -1;
}
