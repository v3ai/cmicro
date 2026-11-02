/*
 * io_bench.c - time opening and saving a file with the editor's buffer
 * code (no terminal).  Built by `make tests/io_bench`; links every editor
 * object except main.o.
 *
 *   tests/io_bench FILE [OUT]     open FILE, report time; save to OUT
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/micro.h"
#include "../src/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* main.c provides these in the real editor */
int term_ready;
int term_prompt(const char *msg, const char *const *choices, int nchoices) {
    (void)msg; (void)choices;
    return nchoices - 2; /* "ignore" */
}

int main(int argc, char **argv) {
    long long t0, t1, t2;
    Buffer *b;
    char *err = NULL;
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [OUT]\n", argv[0]);
        return 2;
    }
    options_init_defaults();
    syntax_init();
    t0 = plat_now_ms();
    b = buf_open(argv[1], &err);
    t1 = plat_now_ms();
    if (!b) {
        fprintf(stderr, "open: %s\n", err ? err : "?");
        return 1;
    }
    printf("open: %lld ms, %d lines\n", t1 - t0, b->nlines);
    if (argc > 2) {
        if (buf_save(b, argv[2], &err) != 0) {
            fprintf(stderr, "save: %s\n", err ? err : "?");
            return 1;
        }
        t2 = plat_now_ms();
        printf("save: %lld ms\n", t2 - t1);
    }
    buf_free(b);
    {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        printf("maxrss: %ld KB\n", ru.ru_maxrss);
    }
    return 0;
}
