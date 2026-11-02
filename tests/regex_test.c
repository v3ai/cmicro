/* Regression tests for src/regex.c: semantics checked against Go's regexp,
 * plus inputs that used to crash, overflow or blow up.  Exit 1 on failure.
 *
 *   cc -O2 -o tests/regex_test tests/regex_test.c src/regex.c src/util.c \
 *       src/platform.c src/platform_win32.c && ./tests/regex_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/regex.h"

static int fails, total;

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void fail(const char *what, const char *pat, const char *msg) {
    printf("FAIL %s: pattern %.60s%s: %s\n", what, pat, strlen(pat) > 60 ? "..." : "", msg);
    fails++;
}

/* compile must fail */
static void bad(const char *pat) {
    char *err = NULL;
    Regex *re = re_compile(pat, 0, &err);
    total++;
    if (re) fail("should not compile", pat, "compiled");
    else if (!err) fail("error message", pat, "missing");
    free(err);
    re_free(re);
}

/* expect: space-separated capture offsets of the first match from start,
   or "" for no match */
static void t(const char *pat, int flags, const char *subj, int len, int start, const char *expect) {
    char *err = NULL, buf[512];
    Regex *re = re_compile(pat, flags, &err);
    int caps[20], n, i, o = 0;
    double t0 = now();
    total++;
    if (!re) { fail("compile", pat, err ? err : "?"); free(err); return; }
    n = re_ncaps(re);
    if (n > 10) n = 10;
    buf[0] = 0;
    if (len < 0) len = (int)strlen(subj);
    if (re_search(re, subj, len, start, caps, n))
        for (i = 0; i < 2 * n; i++) o += snprintf(buf + o, sizeof buf - (size_t)o, i ? " %d" : "%d", caps[i]);
    if (strcmp(buf, expect)) {
        char msg[700];
        snprintf(msg, sizeof msg, "got \"%s\", want \"%s\"", buf, expect);
        fail("match", pat, msg);
    }
    re_free(re);
    if (getenv("REGEX_TEST_TIME") && now() - t0 > 0.05) printf("%.2f s: %.60s\n", now() - t0, pat);
}

static char *repeat(const char *s, int n) {
    size_t l = strlen(s);
    char *r = malloc(l * (size_t)n + 1);
    int i;
    for (i = 0; i < n; i++) memcpy(r + l * (size_t)i, s, l);
    r[l * (size_t)n] = 0;
    return r;
}

int main(void) {
    double t0;
    char *a, *b, *p;
    int i, caps[8];
    Regex *re;

    /* --- semantics (expected values from Go's regexp) --- */
    t("a+", 0, "baaa", -1, 0, "1 4");
    t("(a+)(b)?", 0, "xaab", -1, 0, "1 4 1 3 3 4");
    t("(a|ab)(c|bcd)(d*)", 0, "abcd", -1, 0, "0 4 0 1 1 4 4 4");
    t("(a*)*", 0, "b", -1, 0, "0 0 0 0");
    t("(a*)+", 0, "b", -1, 0, "0 0 0 0");
    t("(a|)+", 0, "aab", -1, 0, "0 2 1 2");
    t("(?:a*?)*?b", 0, "aab", -1, 0, "0 3");
    t("x*", 0, "\xe2\x98\x83x", -1, 1, "1 1");          /* mid-character start */
    t("x+", 0, "\xe2\x98\x83x", -1, 1, "3 4");
    t("\\bfoo\\b", 0, "a foo b", -1, 0, "2 5");
    t("\\bfoo", 0, "afoo foo", -1, 1, "5 8");
    t("\\Bfoo", 0, "afoo", -1, 1, "1 4");
    t("^a", 0, "aa", -1, 1, "");
    t("(?m)^a", 0, "b\na", -1, 1, "2 3");
    t("a$", 0, "a\na", -1, 0, "2 3");
    t("(?m)a$", 0, "a\na", -1, 0, "0 1");
    t("\\Aa|b\\z", 0, "ab", -1, 1, "1 2");
    t("(?s).+", 0, "a\nb", -1, 0, "0 3");
    t(".+", 0, "a\nb", -1, 0, "0 1");
    t("(?U)a+", 0, "aaa", -1, 0, "0 1");
    t("(?U)a+?", 0, "aaa", -1, 0, "0 3");
    t("a{2,3}", 0, "aaaa", -1, 0, "0 3");
    t("a{2,3}?", 0, "aaaa", -1, 0, "0 2");
    t("(?:ab){2}", 0, "abab", -1, 0, "0 4");
    t("a{,2}", 0, "a{,2}", -1, 0, "0 5");
    t("a{01}", 0, "a{01}", -1, 0, "0 5");
    t("\\Qa.b\\E+", 0, "a.bbb", -1, 0, "0 5");
    t("\\Qa.b\\E{0}", 0, "xa.", -1, 0, "1 3");
    t("a(?i)b|c", 0, "aB", -1, 0, "0 2");
    t("ba*(?i)*", 0, "baa", -1, 0, "0 3");                /* (?i) is no atom */
    t("ba*\\Q\\E*c", 0, "baac", -1, 0, "0 4");
    t("(?i)k", 0, "\xe2\x84\xaa", -1, 0, "0 3");          /* KELVIN SIGN */
    t("(?i)\xc3\xa9", 0, "\xc3\x89", -1, 0, "0 2");        /* é / É */
    t("(?i)\xcf\x83", 0, "\xcf\x82", -1, 0, "0 2");        /* σ / ς */
    t("\xc3\xa9", RE_ICASE, "\xc3\x89", -1, 0, "0 2");
    t("(?i)[k]", 0, "\xe2\x84\xaa", -1, 0, "0 3");
    t("(?i)\\W", 0, "\xe2\x84\xaa", -1, 0, "");
    t("(?i)[^k]", 0, "K", -1, 0, "");
    t("[^a]", 0, "\xff", -1, 0, "0 1");
    t(".", 0, "\xed\xa0\x80", -1, 0, "0 1");               /* surrogate: 3 bytes */
    t("\xc3\xa9", 0, "\xc3\xc3\xa9", -1, 0, "1 3");
    t("\\x{1F600}", 0, "x\xf0\x9f\x98\x80", -1, 0, "1 5");
    t("\\012\\0", 0, "\n", 2, 0, "0 2");
    t("[[:^alpha:]]+", 0, "ab12c", -1, 0, "2 4");
    t("\\p{Any}+", 0, "a\n\xc3\xa9", -1, 0, "0 4");
    t("\\pL+", 0, "1a\xc3\xa9\xce\xa3_", -1, 0, "1 6");
    t("\\p{Greek}", 0, "a\xce\xa3", -1, 0, "1 3");
    t("\\P{Latin}", 0, "a\xce\xa3", -1, 0, "1 3");
    t("\\p{^Latin}", 0, "a\xce\xa3", -1, 0, "1 3");
    t("[\\p{Sm}]", 0, "a+", -1, 0, "1 2");
    t("\\p{Arabic}", 0, "a\xd8\xa7", -1, 0, "1 3");
    t("[]a]+", 0, "x]a]", -1, 0, "1 4");
    t("[a-]+", 0, "x-a", -1, 0, "1 3");
    t("(?P<x>a)(?<y>b)", 0, "ab", -1, 0, "0 2 0 1 1 2");
    t("", 0, "abc", -1, 2, "2 2");
    t("$", 0, "abc", -1, 0, "3 3");
    t("abc", RE_LITERAL, "xxa.cabc", -1, 0, "5 8");
    t("a.c", RE_LITERAL, "abca.c", -1, 0, "3 6");
    t("A.C", RE_LITERAL | RE_ICASE, "abca.c", -1, 0, "3 6");
    t("a", 0, "a", 1, 5, "");                           /* start > len */
    t("a", 0, "ba", 2, -3, "1 2");                      /* negative start */
    t("a*", 0, "", 0, 0, "0 0");

    /* --- syntax errors (Go rejects these too) --- */
    bad("a**");
    bad("a*+");
    bad("*a");
    bad("(?i)*");
    bad("{2}");
    bad("a{1001}");
    bad("a{2}{3}");
    bad("(a{500}){3}");
    bad("((a{30}){30}){30}");
    bad("\\x{110000}");
    bad("\\x{FFFFFFFFFFFFFFFF}");                        /* overflowed an int */
    bad("\\x{}");
    bad("\\1");
    bad("\\8");
    bad("[[:foo:]]");
    bad("\\pd");
    bad("\\p{Foo}");
    bad("[\\P{Bar}]");
    bad("\\p");
    bad("[z-a]");
    bad("(?P<>a)");
    bad("(?-)");
    bad("(?i-)");
    bad("(?z)");
    bad("(a");
    bad("a)");
    bad("[a");
    bad("a\\");

    /* --- resource limits: these used to be risky --- */
    p = malloc(200002);
    memset(p, '(', 100000);
    memset(p + 100000, ')', 100000);
    p[200000] = 0;
    bad(p);                                             /* deep nesting */
    memset(p, '[', 200000);
    bad(p);
    for (i = 0; i < 200000; i += 5) memcpy(p + i, "*(?i)", 5);
    p[0] = 'a';
    bad(p);                                             /* a*(?i)*(?i)*... nests */
    for (i = 0; i < 200000; i += 5) memcpy(p + i, "*\\Q\\E", 5);
    p[0] = 'a';
    bad(p);
    for (i = 0; i < 200000; i += 2) { p[i] = 'a'; p[i + 1] = '|'; }
    p[199999] = 'a';
    t(p, 0, "xxa", -1, 0, "2 3");                       /* wide alternation */
    free(p);
    a = repeat("(?:", 999);
    b = repeat(")", 999);
    p = malloc(strlen(a) + strlen(b) + 2);
    sprintf(p, "%sx%s", a, b);
    t(p, 0, "yx", -1, 0, "1 2");                        /* 999 levels is fine */
    free(a); free(b); free(p);
    p = repeat("a{1000}", 300);                          /* 300k instructions */
    re = re_compile(p, 0, NULL);
    total++;
    if (!re) fail("compile", "a{1000} x 300", "failed");
    else {
        a = repeat("a", 400000);
        if (!re_search(re, a, 400000, 0, caps, 1) || caps[0] || caps[1] != 300000)
            fail("match", "a{1000} x 300", "wrong");
        free(a);
    }
    re_free(re);
    free(p);
    p = repeat("x", 600000);                             /* long literal */
    re = re_compile(p, RE_LITERAL, NULL);
    a = malloc(1300001);
    memset(a, 'x', 1300000);
    a[599999] = 'y';
    total++;
    if (!re || !re_search(re, a, 1300000, 0, caps, 1) || caps[0] != 600000 || caps[1] != 1200000)
        fail("match", "600k literal", "wrong");
    re_free(re);
    free(a);
    free(p);
    a = repeat("ab", 100);                              /* KMP with overlaps */
    p = repeat("ab", 20);
    a = realloc(a, 256);
    strcat(a, "abc");
    p = realloc(p, 64);
    strcat(p, "c");
    t(p, 0, a, -1, 0, "162 203");
    t(p, 0, a, -1, 163, "");
    a[201] = 0;
    t(p, RE_LITERAL, a, -1, 0, "");
    free(a);
    free(p);
    /* many groups: submatches would need prog * groups memory */
    p = repeat("(a)", 4000);
    re = re_compile(p, 0, NULL);
    a = repeat("a", 4000);
    total++;
    if (!re) fail("compile", "(a) x 4000", "failed");
    else {
        int *c = malloc(sizeof(int) * 2 * 4001);
        if (!re_search(re, a, 4000, 0, c, 4001) || c[0] != 0 || c[1] != 4000 || c[2] != -1)
            fail("match", "(a) x 4000", "wrong");
        if (!re_search(re, a, 4000, 0, c, 3) || c[2] != 0 || c[3] != 1 || c[4] != 1 || c[5] != 2)
            fail("match", "(a) x 4000, 2 groups", "wrong");
        free(c);
    }
    re_free(re);
    free(a);
    free(p);

    /* --- linear time on long lines --- */
    t0 = now();
    a = repeat("a", 3000000);
    t("a*b", 0, a, -1, 0, "");
    t("(x+x+)+y", 0, a, -1, 0, "");
    t("(a|aa)*c", 0, a, -1, 0, "");
    t("(a*)*$", 0, a, -1, 0, "0 3000000 0 3000000");
    t("(a+)$", 0, a, -1, 0, "0 3000000 0 3000000");
    t("a.*b|a", 0, a, -1, 0, "0 1");
    free(a);
    a = repeat("ab", 100000);                          /* DFA cache thrash */
    a[200000 - 13] = 'a';
    t("(?:a|b)*a(?:a|b){12}", 0, a, -1, 0, "0 200000");
    t("(a|b)*a(a|b){12}$", 0, a, -1, 0, "0 200000 199986 199987 199999 200000");
    free(a);
    if (now() - t0 > 20) {
        printf("FAIL long lines took %.1f s\n", now() - t0);
        fails++;
    }
    printf("%d regex tests, %d failures\n", total, fails);
    return fails != 0;
}
