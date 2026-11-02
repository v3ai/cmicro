/* libFuzzer harness for util.c: UTF-8 helpers, glob_match, shell_split
 * and the Str buffer.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o util_fuzz \
 *       tests/util_fuzz.c src/util.c src/platform.c src/platform_win32.c
 *   ./util_fuzz -max_len=4096 corpus/
 *
 * Every input is copied into an exact-size heap buffer so ASan reports
 * any read past the end. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/util.h"

/* Go's utf8.DecodeRune, written independently as the reference */
static int ref_decode(const unsigned char *s, int len, int *cp) {
    int need, c, i, min;
    if (len <= 0) { *cp = 0; return 0; }
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) { need = 1; c = s[0] & 0x1F; min = 0x80; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { need = 2; c = s[0] & 0x0F; min = 0x800; }
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) { need = 3; c = s[0] & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if (len < need + 1) { *cp = 0xFFFD; return 1; }
    for (i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = c;
    return need + 1;
}

/* the original recursive glob matcher (semantics reference; exponential,
   so only used on short inputs) */
static int sep(int c) { return c == '/'; }
static int ref_glob(const char *p, const char *s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (sep((unsigned char)*p)) p++;
            for (;; s++) {
                if (ref_glob(p, s)) return 1;
                if (!*s) return 0;
            }
        } else if (*p == '*') {
            p++;
            for (;; s++) {
                if (ref_glob(p, s)) return 1;
                if (!*s || sep((unsigned char)*s)) return 0;
            }
        } else if (*p == '?') {
            if (!*s || sep((unsigned char)*s)) return 0;
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
            if (*p != *s && !(sep((unsigned char)*p) && sep((unsigned char)*s))) return 0;
            p++; s++;
        }
    }
    return *s == 0;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static void test_utf8(const char *s, int len) {
    int i, n, cp, rcp, count = 0;
    for (i = 0; i < len; i++) {
        n = utf8_decode(s + i, len - i, &cp);
        CHECK(n == ref_decode((const unsigned char *)s + i, len - i, &rcp));
        CHECK(cp == rcp);
        CHECK(char_width(cp) >= 0 && char_width(cp) <= 2);
        (void)is_word_char(cp);
        if (cp != 0xFFFD || n == 3) {
            char buf[4];
            int m = utf8_encode(cp, buf);
            CHECK(m == n && !memcmp(buf, s + i, (size_t)n));
        }
        /* utf8_prev never moves more than 4 bytes back, always moves */
        n = utf8_prev(s, i + 1);
        CHECK(n <= i && n >= i - 3 && n >= 0);
    }
    for (i = 0; i < len; i = utf8_next(s, len, i)) count++;
    CHECK(i == len);
    CHECK(count == utf8_count(s, len));
    CHECK(utf8_offset(s, len, count) == len);
    CHECK(utf8_offset(s, len, count + 5) == len);
    /* walking back from the end visits the same boundaries */
    {
        int j = len, back = 0;
        while (j > 0) { j = utf8_prev(s, j); back++; }
        CHECK(back >= count);
    }
}

static void test_encode(const uint8_t *d, size_t n) {
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        int cp = (int)((uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3]);
        char *buf = xmalloc(4);
        int m = utf8_encode(cp, buf), dc;
        CHECK(m >= 1 && m <= 4);
        CHECK(utf8_decode(buf, m, &dc) == m || (cp >= 0xD800 && cp <= 0xDFFF));
        if (cp >= 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) CHECK(dc == cp);
        free(buf);
    }
}

static void test_str(const uint8_t *d, size_t n) {
    Str s;
    char *model = xmalloc(1);
    int mlen = 0;
    size_t i = 0;
    str_init(&s);
    str_reserve(&s, 0);
    while (i + 3 <= n) {
        int op = d[i] % 6, a = d[i + 1], b = d[i + 2] % 40;
        i += 3;
        switch (op) {
        case 0: /* append */
            model = xrealloc(model, (size_t)mlen + (size_t)b + 1);
            memset(model + mlen, 'a' + (a % 26), (size_t)b);
            {
                char tmp[40];
                memset(tmp, 'a' + (a % 26), (size_t)b);
                str_putn(&s, tmp, b);
            }
            mlen += b;
            break;
        case 1: /* insert, possibly from itself */
            if (a % 2 && s.len > 0) {
                int from = a % s.len, cnt = b > s.len - from ? s.len - from : b, at = (a * 7) % (s.len + 1);
                char *tmp = xstrndup(model + from, (size_t)cnt);
                str_insert(&s, at, s.s + from, cnt);
                model = xrealloc(model, (size_t)mlen + (size_t)cnt + 1);
                memmove(model + at + cnt, model + at, (size_t)(mlen - at));
                memcpy(model + at, tmp, (size_t)cnt);
                mlen += cnt;
                free(tmp);
            } else {
                int at = mlen ? a % (mlen + 1) : 0;
                str_insert(&s, at, "XYZ", 3);
                model = xrealloc(model, (size_t)mlen + 4);
                memmove(model + at + 3, model + at, (size_t)(mlen - at));
                memcpy(model + at, "XYZ", 3);
                mlen += 3;
            }
            break;
        case 2: { /* delete, possibly past the end */
            int at = a - 10, cnt = b * 3 - 5;
            str_delete(&s, at, cnt);
            if (!(at < 0 || at >= mlen || cnt <= 0)) {
                if (cnt > mlen - at) cnt = mlen - at;
                memmove(model + at, model + at + cnt, (size_t)(mlen - at - cnt));
                mlen -= cnt;
            }
            break;
        }
        case 3:
            str_printf(&s, "%d-%s", a, "p");
            {
                char tmp[32];
                int k = sprintf(tmp, "%d-%s", a, "p");
                model = xrealloc(model, (size_t)mlen + (size_t)k + 1);
                memcpy(model + mlen, tmp, (size_t)k);
                mlen += k;
            }
            break;
        case 4: str_putc(&s, 'c'); model = xrealloc(model, (size_t)mlen + 2); model[mlen++] = 'c'; break;
        case 5: str_insert(&s, a, "q", -b); break; /* negative count: no-op */
        }
        CHECK(s.len == mlen);
        CHECK(s.s[s.len] == 0);
        CHECK(!memcmp(s.s, model, (size_t)mlen));
    }
    free(model);
    {
        char *r = str_detach(&s);
        free(r);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s;
    int mode;
    if (size < 1) return 0;
    mode = data[0] % 4;
    data++;
    size--;
    if (size > 1 << 20) return 0;
    s = xmalloc(size ? size : 1); /* exact size: no NUL, ASan catches over-reads */
    memcpy(s, data, size);
    switch (mode) {
    case 0: test_utf8(s, (int)size); break;
    case 1: test_encode(data, size); break;
    case 2: { /* glob: pattern NUL path */
        char *z = xstrndup(s, size), *path;
        size_t pl = strlen(z);
        path = pl < size ? z + pl + 1 : z + pl;
        if (strlen(z) <= 12 && strlen(path) <= 24) {
            int a = glob_match(z, path), b = ref_glob(z, path);
            CHECK(a == b);
        } else {
            (void)glob_match(z, path);
        }
        free(z);
        break;
    }
    case 3: {
        char *z = xstrndup(s, size);
        int argc, i;
        char **v = shell_split(z, &argc);
        for (i = 0; i < argc; i++) CHECK(v[i] != NULL);
        CHECK(v[argc] == NULL);
        free_strv(v);
        test_str(data, size);
        free(z);
        break;
    }
    }
    free(s);
    return 0;
}
