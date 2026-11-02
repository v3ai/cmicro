/* syntax_bench - highlight files through the editor's syntax.c code path.
 *
 *   syntax_bench [-n iters] [-d] [-f filetype] [-c configdir] file...
 *
 * For each file: detect the syntax (or use -f), then highlight every line
 * the way buffer.c does (state carried from line to line, one group id per
 * byte).  -d dumps the per-byte group names as runs plus the end-of-line
 * region, so output can be diffed between versions, and checks that the
 * states-only pass (hl == NULL, used for lines above the view) ends every
 * line in the same region.  -c loads user syntax files from
 * configdir/syntax as the editor does.  Prints timings for syntax_init()
 * and for highlighting to stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/colors.h"
#include "../src/runtime.h"
#include "../src/syntax.h"
#include "../src/util.h"

static double now(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

static void region_name(Region *r, Str *o) {
    if (!r) { str_puts(o, "-"); return; }
    region_name(r->parent, o);
    str_putc(o, '/');
    str_puts(o, group_name(r->group));
}

int main(int argc, char **argv) {
    int iters = 1, dump = 0, a, it;
    const char *ft = NULL;
    double t0, t1;
    config_dir = NULL;
    for (a = 1; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-n") && a + 1 < argc) iters = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-d")) dump = 1;
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) ft = argv[++a];
        else if (!strcmp(argv[a], "-c") && a + 1 < argc) config_dir = xstrdup(argv[++a]);
    }
    colorscheme_load("default", NULL);
    t0 = now();
    syntax_init();
    t1 = now();
    group_styles_refresh();
    fprintf(stderr, "syntax_init: %.3f ms\n", (t1 - t0) * 1e3);
    for (; a < argc; a++) {
        long flen = 0;
        char *text = read_file(argv[a], &flen);
        char **lines = NULL;
        int *lens = NULL, n = 0, cap = 0, i, maxlen = 0;
        unsigned short *hl;
        Syntax *syn;
        char *p, *err = NULL;
        double best = 1e30;
        if (!text) { fprintf(stderr, "cannot read %s\n", argv[a]); continue; }
        for (p = text;;) {
            char *e = memchr(p, '\n', (size_t)(text + flen - p));
            int len = e ? (int)(e - p) : (int)(text + flen - p);
            if (n == cap) {
                cap = cap ? cap * 2 : 1024;
                lines = xrealloc(lines, sizeof *lines * (size_t)cap);
                lens = xrealloc(lens, sizeof *lens * (size_t)cap);
            }
            if (len > 0 && p[len - 1] == '\r') len--;
            lines[n] = p;
            lens[n++] = len;
            if (len > maxlen) maxlen = len;
            if (!e) break;
            p = e + 1;
        }
        if (ft) syn = syntax_by_filetype(ft);
        else {
            char *first = xstrndup(lines[0], (size_t)lens[0]);
            syn = syntax_detect(argv[a], first, NULL, 0, 0);
            free(first);
        }
        if (!syn) { fprintf(stderr, "%s: no syntax\n", argv[a]); continue; }
        t0 = now();
        syntax_load(syn, &err);
        t1 = now();
        if (err) { fprintf(stderr, "load: %s\n", err); free(err); }
        fprintf(stderr, "%s: filetype %s, load %.3f ms, %d lines, %ld bytes\n", argv[a],
                syn->filetype, (t1 - t0) * 1e3, n, flen);
        hl = xmalloc(sizeof *hl * (size_t)(maxlen + 1));
        for (it = 0; it < iters; it++) {
            Region *st = NULL, *st2 = NULL;
            Str o;
            str_init(&o);
            t0 = now();
            for (i = 0; i < n; i++) {
                /* NUL-terminate like the editor's line buffers */
                char save = lines[i][lens[i]];
                lines[i][lens[i]] = 0;
                st = syntax_highlight_line(syn, st, lines[i], lens[i], hl);
                if (dump && it == 0) {
                    st2 = syntax_highlight_line(syn, st2, lines[i], lens[i], NULL);
                    if (st2 != st) {
                        fprintf(stderr, "%s:%d: states-only pass ends in a different region\n", argv[a], i + 1);
                        exit(1);
                    }
                }
                lines[i][lens[i]] = save;
                if (dump && it == 0) {
                    int k = 0;
                    str_printf(&o, "%d:", i + 1);
                    while (k < lens[i]) {
                        int j = k;
                        while (j < lens[i] && hl[j] == hl[k]) j++;
                        str_printf(&o, " %d-%d %s", k, j, hl[k] ? group_name(hl[k]) : ".");
                        k = j;
                    }
                    str_puts(&o, " | ");
                    region_name(st, &o);
                    str_putc(&o, '\n');
                }
            }
            t1 = now();
            if (t1 - t0 < best) best = t1 - t0;
            if (o.s) fputs(o.s, stdout);
            str_free(&o);
        }
        fprintf(stderr, "%s: highlight best of %d: %.3f ms (%.1f MB/s)\n", argv[a], iters, best * 1e3,
                (double)flen / best / 1e6);
        free(hl);
        free(lines);
        free(lens);
        free(text);
    }
    return 0;
}
