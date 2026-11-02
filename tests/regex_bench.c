/* Regex engine benchmark (not run by `make test`).
 *
 *   regex_bench hl FILETYPE FILE...   highlight FILEs with the real
 *                                     highlighter (src/syntax.c)
 *   regex_bench all PATFILE FILE...   every pattern of PATFILE (one per line)
 *                                     over every line: all-matches loop
 *
 * Build: see tests/regex_diff/run.sh or
 *   cc -O2 -o regex_bench tests/regex_bench.c src/syntax.c src/colors.c \
 *      src/runtime.c src/runtime_data.c src/conf.c src/regex.c src/util.c \
 *      src/platform.c src/platform_win32.c   (plus whatever they need)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/regex.h"
#include "../src/syntax.h"
#include "../src/util.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

typedef struct { char *s; int len; } Line;

static Line *split_lines(char *text, long n, int *nl) {
    Line *v = NULL;
    int cnt = 0, cap = 0;
    long i, st = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || text[i] == '\n') {
            if (cnt == cap) { cap = cap ? cap * 2 : 1024; v = realloc(v, sizeof *v * (size_t)cap); }
            v[cnt].s = text + st;
            v[cnt].len = (int)(i - st);
            cnt++;
            st = i + 1;
        }
    }
    *nl = cnt;
    return v;
}

int main(int argc, char **argv) {
    int a, total_lines = 0;
    long matches = 0;
    unsigned long hash = 0;
    double t0, t;
    if (argc < 4) { fprintf(stderr, "usage: regex_bench hl|all ARG FILE...\n"); return 2; }
    if (!strcmp(argv[1], "hl")) {
        Syntax *syn;
        char *err = NULL;
        syntax_init();
        syn = syntax_by_filetype(argv[2]);
        if (!syn || syntax_load(syn, &err) < 0) { fprintf(stderr, "no syntax %s: %s\n", argv[2], err ? err : ""); return 1; }
        t0 = now();
        for (a = 3; a < argc; a++) {
            long n;
            int nl, i, k, maxlen = 0;
            char *text = read_file(argv[a], &n);
            Line *ls;
            unsigned short *hl;
            Region *st = NULL;
            if (!text) continue;
            ls = split_lines(text, n, &nl);
            for (i = 0; i < nl; i++) if (ls[i].len > maxlen) maxlen = ls[i].len;
            hl = malloc(sizeof *hl * (size_t)(maxlen + 1));
            for (i = 0; i < nl; i++) {
                st = syntax_highlight_line(syn, st, ls[i].s, ls[i].len, hl);
                for (k = 0; k < ls[i].len; k++) hash = hash * 31 + hl[k];
            }
            total_lines += nl;
            free(hl);
            free(ls);
            free(text);
        }
        t = now() - t0;
    } else {
        long n;
        char *pt = read_file(argv[2], &n);
        int np, i, k;
        Line *pl;
        Regex **res;
        double tc, *per = NULL;
        if (!pt) return 1;
        pl = split_lines(pt, n, &np);
        res = calloc((size_t)np, sizeof *res);
        if (getenv("BENCH_PER")) per = calloc((size_t)np, sizeof *per);
        t0 = now();
        for (i = 0; i < np; i++) {
            char *p = malloc((size_t)pl[i].len + 1);
            memcpy(p, pl[i].s, (size_t)pl[i].len);
            p[pl[i].len] = 0;
            res[i] = re_compile(p, 0, NULL);
            free(p);
        }
        tc = now() - t0;
        t0 = now();
        for (a = 3; a < argc; a++) {
            int nl;
            char *text = read_file(argv[a], &n);
            Line *ls;
            if (!text) continue;
            ls = split_lines(text, n, &nl);
            for (k = 0; k < nl; k++) {
                for (i = 0; i < np; i++) {
                    int pos = 0, caps[2];
                    double tp = per ? now() : 0;
                    if (!res[i]) continue;
                    while (pos <= ls[k].len && re_search(res[i], ls[k].s, ls[k].len, pos, caps, 1)) {
                        matches++;
                        hash = hash * 31 + (unsigned long)(caps[0] * 7 + caps[1]);
                        pos = caps[1] > pos ? caps[1] : pos + 1;
                    }
                    if (per) per[i] += now() - tp;
                }
            }
            total_lines += nl;
            free(ls);
            free(text);
        }
        t = now() - t0;
        printf("compile %d patterns: %.3f s\n", np, tc);
        for (i = 0; per && i < np; i++)
            printf("%8.4f  %.*s\n", per[i], pl[i].len > 100 ? 100 : pl[i].len, pl[i].s);
        free(per);
        for (i = 0; i < np; i++) re_free(res[i]);
        free(res);
        free(pl);
        free(pt);
    }
    printf("%d lines, %ld matches, hash %lx, %.3f s\n", total_lines, matches, hash, t);
    return 0;
}
