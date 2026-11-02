/* Checks the highlighter's regex prefilter (syntax.c: make_filter/search)
 * against plain re_search: for every start position the leftmost match
 * must be identical.  Includes syntax.c to reach its static functions.
 *
 * The plain test also checks the anchored versions of region start/end/skip
 * regexes, compares highlighting with and without syntax.c's per-line
 * search caches for every bundled syntax, and times long lines that used to
 * take quadratic time.
 *
 * As a libFuzzer target (input: <pattern> NUL <text>):
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -DFUZZ -o syntax_filter_fuzz \
 *       tests/syntax_filter_fuzz.c src/regex.c src/conf.c src/util.c src/colors.c \
 *       src/runtime.c src/runtime_data.c src/platform.c src/platform_win32.c
 *
 * As a plain test over every rule of the given syntax files and text files:
 *   cc -O2 -o syntax_filter_test tests/syntax_filter_fuzz.c src/regex.c src/conf.c \
 *       src/util.c src/colors.c src/runtime.c src/runtime_data.c src/platform.c src/platform_win32.c
 *   ./syntax_filter_test runtime/syntax/c.yaml runtime/syntax/go.yaml -- file.c file.go
 */
#include "../src/syntax.c"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

static long checks, filtered;

static void check_one(const char *pat, const Regex *re, const ReFilter *f, const char *s, int len, int start) {
    int c1[2] = {-1, -1}, c2[2] = {-1, -1};
    int a = search(re, f, s, len, start, c1);
    int b = re_search(re, s, len, start, c2, 1);
    checks++;
    if (a != b || (a && (c1[0] != c2[0] || c1[1] != c2[1]))) {
        fprintf(stderr, "prefilter mismatch for /%s/ at start %d: %d [%d,%d] vs %d [%d,%d]\ntext: %.*s\n", pat,
                start, a, c1[0], c1[1], b, c2[0], c2[1], len, s);
        abort();
    }
}

static void check_pattern(const char *pat, const char *text, int tlen, int all_starts) {
    char *err = NULL;
    ReFilter *f;
    Regex *re = compile_rule(pat, &f, &err);
    free(err);
    if (!re) return;
    if (f && f->active) {
        int st;
        filtered++;
        if (all_starts) {
            for (st = 0; st <= tlen; st++) check_one(pat, re, f, text, tlen, st);
        } else {
            /* line by line, from the line start and a later position */
            int a = 0;
            while (a <= tlen) {
                const char *e = memchr(text + a, '\n', (size_t)(tlen - a));
                int l = e ? (int)(e - (text + a)) : tlen - a;
                check_one(pat, re, f, text + a, l, 0);
                if (l > 4) check_one(pat, re, f, text + a, l, l / 3);
                a += l + 1;
            }
        }
    }
    free(f);
    re_free(re);
}

#ifdef FUZZ
/* "\x{" followed by more than 8 hex digits overflows an int in
   regex.c's parse_escape (signed overflow, reported separately) */
static int long_hex_escape(const uint8_t *d, size_t n) {
    size_t i, k;
    for (i = 0; i + 3 < n; i++) {
        if (d[i] != '\\' || d[i + 1] != 'x' || d[i + 2] != '{') continue;
        for (k = i + 3; k < n && ((d[k] >= '0' && d[k] <= '9') || ((d[k] | 32) >= 'a' && (d[k] | 32) <= 'f')); k++)
            ;
        if (k - (i + 3) > 7) return 1;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uint8_t *nul = memchr(data, 0, size);
    char *pat, *text;
    size_t plen = nul ? (size_t)(nul - data) : size, tlen = nul ? size - plen - 1 : 0;
    if (plen > 256 || tlen > 512) return 0;
    if (long_hex_escape(data, plen)) return 0;
    pat = xstrndup((const char *)data, plen);
    text = xmalloc(tlen ? tlen : 1);
    memcpy(text, nul ? (const char *)nul + 1 : "", tlen);
    check_pattern(pat, text, (int)tlen, 1);
    free(pat);
    free(text);
    return 0;
}
#else
static char *text;
static int tlen;
static long anch_checks, hl_lines;

static void fail(const char *fmt, const char *a, int n) {
    fprintf(stderr, fmt, a, n);
    exit(1);
}

/* A region rule that isn't context-free gets an anchored version, which
   must match exactly when the rule's leftmost match starts at the subject
   start (find_cached and mask_valid_at rely on it). */
static void check_anchored(const char *pat) {
    char *err = NULL;
    ReFilter *f;
    Regex *re = compile_region_rule(pat, &f, &err);
    int a = 0;
    free(err);
    if (!re) return;
    if (!f->cf && !f->anch) fail("no anchored version of /%s/\n", pat, 0);
    while (!f->cf && a <= tlen) {
        const char *ln = text + a, *e = memchr(ln, '\n', (size_t)(tlen - a));
        int l = e ? (int)(e - ln) : tlen - a, k;
        int starts[6];
        starts[0] = 0; starts[1] = 1; starts[2] = l / 3; starts[3] = l / 2; starts[4] = l - 1; starts[5] = l;
        for (k = 0; k < 6; k++) {
            int st = starts[k], c1[2], c2[2];
            int m1 = st >= 0 && st <= l && re_search(re, ln + st, l - st, 0, c1, 1) && c1[0] == 0;
            int m2 = st >= 0 && st <= l && re_search(f->anch, ln + st, l - st, 0, c2, 1);
            if (st < 0 || st > l) continue;
            anch_checks++;
            if (m1 != m2 || (m1 && c1[1] != c2[1])) fail("anchored /%s/ differs at %d\n", pat, (int)(ln - text) + st);
        }
        a += l + 1;
    }
    free_filter(f);
    re_free(re);
}

static void walk(CNode *rules) {
    int i, k;
    if (!rules || rules->type != CN_LIST) return;
    for (i = 0; i < rules->n; i++) {
        CNode *r = rules->vals[i];
        if (r->type != CN_MAP) continue;
        for (k = 0; k < r->n; k++) {
            CNode *v = r->vals[k];
            if (!strcmp(r->keys[k], "include")) continue;
            if (v->type == CN_STR) check_pattern(v->str, text, tlen, 0);
            else if (v->type == CN_MAP) {
                const char *s;
                if ((s = cnode_str(v, "start"))) { check_pattern(s, text, tlen, 0); check_anchored(s); }
                if ((s = cnode_str(v, "end"))) { check_pattern(s, text, tlen, 0); check_anchored(s); }
                if ((s = cnode_str(v, "skip"))) { check_pattern(s, text, tlen, 0); check_anchored(s); }
                walk(cnode_get(v, "rules"));
            }
        }
    }
}

/* `n` lines of token soup (every 50th one long) appended to `o` */
static void token_soup(Str *o, const char *const *tok, int ntok, int n, unsigned seed) {
    int i, k;
    for (i = 0; i < n; i++) {
        int len = i % 50 == 0 ? 3000 : (int)(seed % 60);
        str_putc(o, '\n');
        for (k = 0; k < len; k++) {
            seed = seed * 1103515245u + 12345u;
            str_puts(o, tok[(seed >> 16) % (unsigned)ntok]);
        }
    }
}

/* Highlight every line of `t` with and without the per-line caches: the
   highlighting and the end-of-line regions must be identical. */
static void check_caches(Syntax *syn, Str *t) {
    unsigned short *h1 = xmalloc(sizeof *h1 * (size_t)(t->len + 1)), *h2 = xmalloc(sizeof *h2 * (size_t)(t->len + 1));
    Region *s1 = NULL, *s2 = NULL;
    int a = 0;
    while (a <= t->len) {
        char *ln = t->s + a, *e = memchr(ln, '\n', (size_t)(t->len - a));
        int l = e ? (int)(e - ln) : t->len - a;
        char save = ln[l];
        ln[l] = 0;
        syntax_nocache = 0;
        s1 = syntax_highlight_line(syn, s1, ln, l, h1);
        syntax_nocache = 1;
        s2 = syntax_highlight_line(syn, s2, ln, l, h2);
        syntax_nocache = 0;
        ln[l] = save;
        hl_lines++;
        if (s1 != s2 || memcmp(h1, h2, sizeof *h1 * (size_t)l))
            fail("cached highlighting differs: %s, offset %d\n", syn->filetype, a);
        a += l + 1;
    }
    free(h1);
    free(h2);
}

/* Region rules whose matches depend on where the subject starts (^, \A,
   \b, \B, (?m)^), skips that match empty, and multi-byte characters: the
   cases where cached skip masks and searches need the anchored check. */
static const char test_syntax[] =
    "rules:\n"
    "    - identifier: \"\\\\bq+\\\\b|^a\"\n"
    "    - constant.string:\n"
    "        start: \"\\\\b\\\"|^'\"\n"
    "        end: \"\\\"\\\\B|\\\\b'|^x\"\n"
    "        skip: \"\\\\\\\\.|\xc3\xa9*|\\\\b\"\n"
    "        rules:\n"
    "            - constant.number: \"\\\\Bq\\\\b\"\n"
    "            - special:\n"
    "                start: \"\\\\Ba\\\\{|(?m)^\\\\{\"\n"
    "                end: \"\\\\}\\\\b|\\\\}$\"\n"
    "                skip: \"\\\\Bq*\"\n"
    "    - comment:\n"
    "        start: \"(^|[[:space:]])*(//|#)\"\n"
    "        end: \"$\"\n"
    "    - preproc:\n"
    "        start: \"\\\\b<|\\\\A<<\"\n"
    "        end: \">|\\\\b>\"\n"
    "        rules:\n"
    "            - statement:\n"
    "                start: \"\\\\Ab\\\\(|\\\\(\\\\b\"\n"
    "                end: \"\\\\)\\\\B|\\\\b\\\\)\"\n"
    "                skip: \"^\\\\)|\\\\b\\\\]|\xc3\xa9?\"\n"
    "            - constant.string:\n"
    "                start: \"'\"\n"
    "                end: \"'\"\n"
    "                skip: \"\\\\\\\\.|\\\\b'\\\\b\"\n"
    "    - type:\n"
    "        start: \"\\\\[\"\n"
    "        end: \"\\\\]\"\n"
    "        skip: \"^\\\\]|\\\\b\\\\]|(?m)^\\\\]\"\n"
    "    - constant.string:\n"
    "        start: \"<\"\n"
    "        end: \">\"\n"
    "        skip: \"^>|\\\\B>>|\\\\bcc\\\\b|(?m)^d|x*\"\n"
    "    - special:\n"
    "        start: \"\\\\{\\\\{\"\n"
    "        end: \"^|\\\\}\\\\}\"\n"
    "    - todo:\n"
    "        start: \"@\"\n"
    "        end: \"[a-z]\"\n";

static void check_all_caches(const char *t) {
    static const char *const tok[] = {"\"", "'", "`", "\\", "/*", "*/", "//", "#", "<!--", "-->", "<?php", "?>",
                                      "{", "}", "(", ")", "[", "]", "<", ">", "$", "${", "@", ";", ",", ".", "..",
                                      "0x1f", "123", "1..5", "a", "if", "x", "\xc3\xa9", "\xff", " ", " ", "\t",
                                      "<script>", "</script>", "<style>", "{{", "}}", "\"\"\"", "--", "--[[", "]]",
                                      "=", "<a href=\"/p\">", "</a>", "\\n", "'a'", "\"ab\"", "%", "(*", "*)"};
    static const char *const ttok[] = {"\"", "'", "\\", "\xc3\xa9", "\xe2\x82\xac", "a", "b", "q", "qq", "x", "{",
                                       "}", "{{", "}}", "<", "<<", ">", "(", ")", "[", "]", "#", "//", " ", " ",
                                       "\t", "\xff", "\xe2\x82", "a{", "b(", "1", "_", "cc", "d", "@", "@a"};
    Str all;
    Syntax ts;
    CNode *root;
    char *err = NULL;
    int i;
    str_init(&all);
    str_puts(&all, t);
    token_soup(&all, tok, (int)(sizeof tok / sizeof *tok), 400, 12345);
    for (i = 0; i < nsyntax; i++)
        if (syntax_load(syntaxes[i], NULL) == 0) check_caches(syntaxes[i], &all);
    str_free(&all);

    memset(&ts, 0, sizeof ts);
    ts.filetype = ts.file = "test";
    ts.loaded = 1;
    root = yaml_parse(test_syntax, NULL);
    parse_rules(&ts, cnode_get(root, "rules"), &ts.rules, NULL, &err);
    cnode_free(root);
    if (err || ts.rules.nregions != 7) fail("test syntax: %s\n", err ? err : "regions missing", 0);
    for (i = 0; i < 12; i++) {
        /* all tokens, or a few of them (so that regions also end) */
        const char *sub[6];
        int k, n = (int)(sizeof ttok / sizeof *ttok);
        unsigned seed = (unsigned)i * 7919u + 1;
        str_init(&all);
        if (i % 2) {
            for (k = 0; k < 6; k++) {
                seed = seed * 1103515245u + 12345u;
                sub[k] = ttok[(seed >> 16) % (unsigned)n];
            }
            token_soup(&all, sub, 6, 1500, seed);
        } else {
            token_soup(&all, ttok, n, 1500, seed);
        }
        check_caches(&ts, &all);
        str_free(&all);
    }
}

/* Long lines whose highlighting used to be quadratic (seconds to minutes);
   each must now take well under a second. */
static void check_long_lines(void) {
    static const struct { const char *ft, *head, *rep, *tail; int n; } c[] = {
        {"c++", "const char *t[]={", "\"ab\",", "};", 40000},  /* non-context-free skip (\b) */
        {"c++", "\"", "\\\\\"", "", 40000},
        {"php", "<?php $a=[", "\"a\",", "];", 60000},         /* non-context-free start (^) */
        {"php", "<?php ", "/*", "", 100000},
        {"svelte", "", "<li class=\"it\"><a href=\"/p/1\">Item 1</a></li>", "", 5000}, /* region end $ */
        {"nu", "let x = ", "0", "", 65536},                  /* nullable patterns */
    };
    int i, k;
    for (i = 0; i < (int)(sizeof c / sizeof *c); i++) {
        Syntax *syn = syntax_by_filetype(c[i].ft);
        Str l;
        unsigned short *hl;
        double t0, dt;
        if (!syn || syntax_load(syn, NULL) != 0) fail("no syntax %s\n", c[i].ft, 0);
        str_init(&l);
        str_puts(&l, c[i].head);
        for (k = 0; k < c[i].n; k++) str_puts(&l, c[i].rep);
        str_puts(&l, c[i].tail);
        hl = xmalloc(sizeof *hl * (size_t)(l.len + 1));
        t0 = (double)clock() / CLOCKS_PER_SEC;
        syntax_highlight_line(syn, NULL, l.s, l.len, hl);
        syntax_highlight_line(syn, NULL, l.s, l.len, NULL);
        dt = (double)clock() / CLOCKS_PER_SEC - t0;
        if (dt > 2.0) fail("long %s line took too long (%d ms)\n", c[i].ft, (int)(dt * 1000));
        free(hl);
        str_free(&l);
    }
}

int main(int argc, char **argv) {
    int i, sep = argc, total = 0;
    Str t;
    str_init(&t);
    str_reserve(&t, 0);
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--")) sep = i;
    for (i = sep + 1; i < argc; i++) {
        char *d = read_file(argv[i], NULL);
        if (d) { str_puts(&t, d); free(d); }
    }
    text = t.s;
    tlen = t.len;
    for (i = 1; i < sep; i++) {
        char *y = read_file(argv[i], NULL);
        CNode *root;
        long before = filtered;
        if (!y) continue;
        root = yaml_parse(y, NULL);
        walk(cnode_get(root, "rules"));
        cnode_free(root);
        free(y);
        total++;
        (void)before;
    }
    colorscheme_load("default", NULL);
    syntax_init();
    check_all_caches(text);
    check_long_lines();
    printf("%d syntax files, %ld filtered regexes, %ld searches compared, %ld anchored regex checks, "
           "%ld lines highlighted with and without caches, long lines: ok\n",
           total, filtered, checks, anch_checks, hl_lines);
    return 0;
}
#endif
