/* Checks the highlighter's regex prefilter (syntax.c: make_filter/search)
 * against plain re_search: for every start position the leftmost match
 * must be identical.  Includes syntax.c to reach its static functions.
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
                if ((s = cnode_str(v, "start"))) check_pattern(s, text, tlen, 0);
                if ((s = cnode_str(v, "end"))) check_pattern(s, text, tlen, 0);
                if ((s = cnode_str(v, "skip"))) check_pattern(s, text, tlen, 0);
                walk(cnode_get(v, "rules"));
            }
        }
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
    printf("%d syntax files, %ld filtered regexes, %ld searches compared: ok\n", total, filtered, checks);
    return 0;
}
#endif
