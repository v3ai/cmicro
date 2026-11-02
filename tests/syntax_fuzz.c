/* libFuzzer harness for syntax.c (+ conf.c, colors.c, runtime.c):
 * loads a fuzzed syntax definition and highlights fuzzed text with it.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o syntax_fuzz \
 *       tests/syntax_fuzz.c src/syntax.c src/regex.c src/conf.c src/util.c \
 *       src/colors.c src/runtime.c src/runtime_data.c src/platform.c src/platform_win32.c
 *   ./syntax_fuzz -max_len=8192 -timeout=10 corpus/
 *
 * Input: <syntax yaml> NUL <text>.  The definition is written to a
 * temporary config dir (the user-override path of runtime_read).
 * "include" is disabled (renamed) so every rule and regex is owned by the
 * fuzzed syntax and can be freed after each run.
 *
 * Checks: every hl entry is a valid group id; the returned state is NULL
 * or one of the syntax's regions; highlighting with and without an hl
 * buffer gives the same state; nothing is written past hl[len-1]. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/conf.h"
#include "../src/runtime.h"
#include "../src/syntax.h"
#include "../src/util.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static char *syn_path;

/* "\x{" followed by more than 8 hex digits overflows an int in
   regex.c's parse_escape (signed overflow, reported separately); skip
   such inputs so the fuzzer keeps exploring this code */
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

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    char tmpl[] = "/tmp/syntax_fuzz_XXXXXX";
    char *d, *sd;
    (void)argc; (void)argv;
    d = mkdtemp(tmpl);
    if (!d) abort();
    config_dir = xstrdup(d);
    sd = xsprintf("%s/syntax", d);
    mkdir(sd, 0700);
    syn_path = xsprintf("%s/fz.yaml", sd);
    free(sd);
    colorscheme_load("default", NULL);
    syntax_init();
    return 0;
}

static Region **regs;
static int nregs, cregs;

static void collect(Rules *r) {
    int i;
    for (i = 0; i < r->nregions; i++) {
        if (nregs == cregs) {
            cregs = cregs ? cregs * 2 : 64;
            regs = xrealloc(regs, sizeof *regs * (size_t)cregs);
        }
        regs[nregs++] = r->regions[i];
        collect(&r->regions[i]->rules);
    }
}

static void free_rules(Rules *r) {
    int i;
    for (i = 0; i < r->npats; i++) {
        re_free(r->pats[i].re);
        free(r->pats[i].flt);
    }
    for (i = 0; i < r->nregions; i++) {
        Region *g = r->regions[i];
        re_free(g->start);
        re_free(g->end);
        re_free(g->skip);
        free(g->start_flt);
        free(g->end_flt);
        free(g->skip_flt);
        free_rules(&g->rules);
        free(g);
    }
    free(r->pats);
    free(r->regions);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uint8_t *nul = memchr(data, 0, size);
    size_t ylen = nul ? (size_t)(nul - data) : size;
    const char *text = nul ? (const char *)nul + 1 : "";
    size_t tlen = nul ? size - ylen - 1 : 0;
    char *yaml = xstrndup((const char *)data, ylen), *p, *err = NULL;
    Syntax s;
    FILE *f;
    Region *st = NULL, *st2 = NULL;
    size_t pos;
    int ng;
    while ((p = strstr(yaml, "include"))) p[4] = 'x';
    if (long_hex_escape((const uint8_t *)yaml, strlen(yaml))) { free(yaml); return 0; }
    f = fopen(syn_path, "wb");
    if (!f) abort();
    fwrite(yaml, 1, strlen(yaml), f);
    fclose(f);
    free(yaml);
    memset(&s, 0, sizeof s);
    s.filetype = "fz";
    s.file = "fz";
    syntax_load(&s, &err);
    free(err);
    nregs = 0;
    collect(&s.rules);
    ng = group_count();
    for (pos = 0; pos <= tlen;) {
        const char *e = memchr(text + pos, '\n', tlen - pos);
        size_t len = e ? (size_t)(e - (text + pos)) : tlen - pos;
        char *line = xstrndup(text + pos, len);
        unsigned short *hl = xmalloc(len ? len * sizeof *hl : 1); /* exact size */
        size_t i;
        int k, ok;
        st = syntax_highlight_line(&s, st, line, (int)len, hl);
        st2 = syntax_highlight_line(&s, st2, line, (int)len, NULL);
        CHECK(st == st2);
        for (i = 0; i < len; i++) CHECK(hl[i] < ng);
        ok = st == NULL;
        for (k = 0; k < nregs && !ok; k++) ok = regs[k] == st;
        CHECK(ok);
        free(hl);
        free(line);
        if (!e) break;
        pos += len + 1;
    }
    free_rules(&s.rules);
    return 0;
}
