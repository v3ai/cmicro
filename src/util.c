#include "util.h"
#include "platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("micro: out of memory\n", stderr); exit(1); }
    return p;
}

void *xcalloc(size_t n, size_t m) {
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) { fputs("micro: out of memory\n", stderr); exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fputs("micro: out of memory\n", stderr); exit(1); }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *r = xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *xstrndup(const char *s, size_t n) {
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char *xsprintf(const char *fmt, ...) {
    va_list ap;
    int n;
    char *r;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    r = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(r, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return r;
}

/* ---------- Str ---------- */

void str_init(Str *s) { s->s = NULL; s->len = s->cap = 0; }
void str_free(Str *s) { free(s->s); str_init(s); }
void str_clear(Str *s) { s->len = 0; if (s->s) s->s[0] = 0; }

void str_reserve(Str *s, int n) {
    if (n + 1 > s->cap) {
        int c = s->cap ? s->cap : 16;
        while (c < n + 1) c *= 2;
        s->s = xrealloc(s->s, (size_t)c);
        s->cap = c;
        s->s[s->len] = 0; /* always NUL-terminated, even when empty */
    }
}

void str_putn(Str *s, const char *p, int n) {
    if (n <= 0) { str_reserve(s, s->len); s->s[s->len] = 0; return; }
    str_reserve(s, s->len + n);
    memcpy(s->s + s->len, p, (size_t)n);
    s->len += n;
    s->s[s->len] = 0;
}

void str_puts(Str *s, const char *p) { str_putn(s, p, (int)strlen(p)); }
void str_putc(Str *s, char c) { str_putn(s, &c, 1); }

void str_printf(Str *s, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    str_reserve(s, s->len + n);
    va_start(ap, fmt);
    vsnprintf(s->s + s->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->len += n;
}

void str_insert(Str *s, int at, const char *p, int n) {
    if (at < 0) at = 0;
    if (at > s->len) at = s->len;
    str_reserve(s, s->len + n);
    memmove(s->s + at + n, s->s + at, (size_t)(s->len - at));
    memcpy(s->s + at, p, (size_t)n);
    s->len += n;
    s->s[s->len] = 0;
}

void str_delete(Str *s, int at, int n) {
    if (at < 0 || at >= s->len || n <= 0) return;
    if (at + n > s->len) n = s->len - at;
    memmove(s->s + at, s->s + at + n, (size_t)(s->len - at - n));
    s->len -= n;
    s->s[s->len] = 0;
}

char *str_detach(Str *s) {
    char *r = s->s ? s->s : xstrdup("");
    str_init(s);
    return r;
}

/* ---------- UTF-8 ---------- */

int utf8_decode(const char *str, int len, int *cp) {
    const unsigned char *s = (const unsigned char *)str;
    if (len <= 0) { *cp = 0; return 0; }
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        if (*cp >= 0x80) return 2;
    } else if ((s[0] & 0xF0) == 0xE0 && len >= 3 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (*cp >= 0x800) return 3;
    } else if ((s[0] & 0xF8) == 0xF0 && len >= 4 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) |
              (s[3] & 0x3F);
        if (*cp >= 0x10000 && *cp <= 0x10FFFF) return 4;
    }
    *cp = 0xFFFD; /* invalid byte: treat as one replacement char */
    return 1;
}

int utf8_encode(int cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_next(const char *s, int len, int i) {
    int cp;
    if (i >= len) return len;
    return i + utf8_decode(s + i, len - i, &cp);
}

int utf8_prev(const char *s, int i) {
    int j, cp, n;
    if (i <= 0) return 0;
    /* walk back over continuation bytes, but verify the sequence decodes */
    for (j = i - 1; j > 0 && j > i - 4 && (((unsigned char)s[j]) & 0xC0) == 0x80; j--)
        ;
    n = utf8_decode(s + j, i - j, &cp);
    if (j + n == i) return j;
    return i - 1;
}

int utf8_count(const char *s, int len) {
    int i = 0, n = 0;
    while (i < len) { i = utf8_next(s, len, i); n++; }
    return n;
}

int utf8_offset(const char *s, int len, int nchars) {
    int i = 0;
    while (nchars-- > 0 && i < len) i = utf8_next(s, len, i);
    return i;
}

struct range { int lo, hi; };

static const struct range zero_width[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x0901, 0x0902}, {0x093C, 0x093C}, {0x0941, 0x0948},
    {0x094D, 0x094D}, {0x0951, 0x0954}, {0x0962, 0x0963}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

static const struct range wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251},
    {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static int in_table(int cp, const struct range *t, int n) {
    int lo = 0, hi = n - 1;
    if (cp < t[0].lo || cp > t[n - 1].hi) return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp > t[mid].hi) lo = mid + 1;
        else if (cp < t[mid].lo) hi = mid - 1;
        else return 1;
    }
    return 0;
}

int char_width(int cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 1; /* drawn as replacement */
    if (cp < 0x300) return 1;
    if (in_table(cp, zero_width, (int)(sizeof zero_width / sizeof zero_width[0]))) return 0;
    if (in_table(cp, wide, (int)(sizeof wide / sizeof wide[0]))) return 2;
    return 1;
}

int is_word_char(int cp) {
    if (cp < 128) return isalnum(cp) || cp == '_';
    /* treat all non-ASCII letters/symbols (except common punctuation/space) as word chars */
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x206F) || cp == 0x3000) return 0;
    return 1;
}

int is_space_char(int cp) { return cp == ' ' || cp == '\t'; }

/* ---------- misc ---------- */

int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

int strieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

const char *path_last_sep(const char *p) {
    const char *r = NULL;
    for (; *p; p++)
        if (plat_is_sep((unsigned char)*p)) r = p;
    return r;
}

char *path_join(const char *a, const char *b) {
    size_t n = strlen(a);
    if (plat_is_abs(b)) return xstrdup(b);
    if (n == 0) return xstrdup(b);
    if (plat_is_sep((unsigned char)a[n - 1])) return xsprintf("%s%s", a, b);
    return xsprintf("%s/%s", a, b);
}

const char *path_base(const char *p) {
    const char *s = path_last_sep(p);
    return s ? s + 1 : p;
}

char *path_dir(const char *p) {
    const char *s = path_last_sep(p);
    if (!s) return xstrdup(".");
    if (s == p) return xstrndup(p, 1);              /* "/" */
    if (s == p + 2 && p[1] == ':') return xstrndup(p, 3); /* "C:\" */
    return xstrndup(p, (size_t)(s - p));
}

char *expand_home(const char *p) {
    char *home, *r;
    if (p[0] != '~' || (p[1] != 0 && !plat_is_sep((unsigned char)p[1]))) return xstrdup(p);
    home = plat_home_dir();
    if (!home) return xstrdup(p);
    r = xsprintf("%s%s", home, p + 1);
    free(home);
    return r;
}

char *read_file(const char *path, long *len) {
    FILE *f = plat_fopen(path, "rb");
    Str s;
    char buf[65536];
    size_t n;
    if (!f) return NULL;
    str_init(&s);
    str_reserve(&s, 0);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) str_putn(&s, buf, (int)n);
    if (ferror(f)) { fclose(f); str_free(&s); return NULL; }
    fclose(f);
    if (len) *len = s.len;
    return str_detach(&s);
}

/* glob with *, ?, ** and [...] classes; '*' does not cross '/', '**' does */
int glob_match(const char *p, const char *s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (plat_is_sep((unsigned char)*p)) p++;
            for (;; s++) {
                if (glob_match(p, s)) return 1;
                if (!*s) return 0;
            }
        } else if (*p == '*') {
            p++;
            for (;; s++) {
                if (glob_match(p, s)) return 1;
                if (!*s || plat_is_sep((unsigned char)*s)) return 0;
            }
        } else if (*p == '?') {
            if (!*s || plat_is_sep((unsigned char)*s)) return 0;
            p++; s++;
        } else if (*p == '[') {
            int neg = 0, ok = 0;
            const char *q = p + 1;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2]) ok = 1;
                    q += 3;
                } else {
                    if (*s == *q) ok = 1;
                    q++;
                }
            }
            if (!*s || ok == neg) return 0;
            p = *q ? q + 1 : q;
            s++;
        } else {
            if (*p != *s && !(plat_is_sep((unsigned char)*p) && plat_is_sep((unsigned char)*s))) return 0;
            p++; s++;
        }
    }
    return *s == 0;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int mini(int a, int b) { return a < b ? a : b; }
int maxi(int a, int b) { return a > b ? a : b; }
int digits(int n) { int d = 1; while (n >= 10) { n /= 10; d++; } return d; }

char **shell_split(const char *s, int *argc) {
    char **v = NULL;
    int n = 0, cap = 0;
    int win = plat_is_sep('\\'); /* Windows: backslashes are literal */
    Str cur;
    str_init(&cur);
    while (*s) {
        int have = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        str_clear(&cur);
        str_reserve(&cur, 0);
        while (*s && *s != ' ' && *s != '\t') {
            have = 1;
            if (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '\\' && !win && (s[1] == '"' || s[1] == '\\')) s++;
                    else if (*s == '\\' && win && s[1] == '"') s++;
                    str_putc(&cur, *s++);
                }
                if (*s) s++;
            } else if (*s == '\'') {
                s++;
                while (*s && *s != '\'') str_putc(&cur, *s++);
                if (*s) s++;
            } else if (*s == '\\' && s[1] && !win) {
                s++;
                str_putc(&cur, *s++);
            } else {
                str_putc(&cur, *s++);
            }
        }
        if (have) {
            if (n + 2 > cap) { cap = cap ? cap * 2 : 8; v = xrealloc(v, sizeof *v * (size_t)cap); }
            v[n++] = xstrdup(cur.s ? cur.s : "");
        }
    }
    str_free(&cur);
    if (!v) v = xmalloc(sizeof *v);
    v[n] = NULL;
    if (argc) *argc = n;
    return v;
}

void free_strv(char **v) {
    char **p;
    if (!v) return;
    for (p = v; *p; p++) free(*p);
    free(v);
}
