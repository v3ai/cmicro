/*
 * io_fuzz.c - libFuzzer target for text loading and editing:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o tests/io_fuzz \
 *         tests/io_fuzz.c (every src file except main.c)
 *   ./tests/io_fuzz -max_total_time=60
 *
 * The input's first half is loaded as a file; the rest drives edits,
 * undo and redo.  Checks: load/serialize round trip, undo-all restores
 * the loaded text, redo-all restores the edited text.
 */
#include "../src/micro.h"
#include "../src/platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int term_ready;
int term_prompt(const char *msg, const char *const *choices, int nchoices) {
    (void)msg; (void)choices;
    return nchoices - 2;
}

int buf_serialize(const Buffer *b, int (*put)(void *ctx, const char *p, size_t n), void *ctx);

static int put_str(void *ctx, const char *p, size_t n) { str_putn(ctx, p, (int)n); return 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static int init;
    size_t half = size / 2, i;
    Buffer *b;
    char *orig, *final, *now;
    int ol, fl, nl;
    if (!init) {
        options_init_defaults();
        init = 1;
    }
    b = buf_new_empty();
    buf_set_text(b, (const char *)data, (int)half);
    {
        /* LF files (or CRLF-only ones) come back byte for byte */
        Str s;
        int dos = !strcmp(BOPTS(b, O_FILEFORMAT), "dos");
        const char *t = (const char *)data;
        size_t k;
        int lone_lf = 0;
        for (k = 0; k < half; k++)
            if (t[k] == '\n' && (k == 0 || t[k - 1] != '\r')) lone_lf = 1;
        str_init(&s);
        str_reserve(&s, 0);
        buf_serialize(b, put_str, &s);
        if (!(dos && lone_lf)) {
            if ((size_t)s.len != half || memcmp(s.s, t, half) != 0) abort();
        }
        str_free(&s);
    }
    orig = buf_full_text(b, &ol);
    for (i = half; i + 3 <= size; i += 4) {
        int op = data[i] % 4;
        Loc a = loc_make(data[i + 1] % 40 - 2, data[i + 2] % (b->nlines + 2) - 1);
        if (op == 0 || op == 1) {
            int len = i + 3 < size ? data[i + 3] % 8 : 0;
            if (i + 4 + (size_t)len > size) len = 0;
            buf_insert(b, a, (const char *)data + i + 4, len);
        } else if (op == 2) {
            Loc z = loc_make(i + 3 < size ? data[i + 3] % 40 : 0, a.y + 1);
            buf_remove(b, a, z);
        } else {
            free(buf_get_text(b, a, buf_end(b), &nl));
        }
    }
    final = buf_full_text(b, &fl);
    while (buf_undo(b)) {}
    now = buf_full_text(b, &nl);
    if (nl != ol || memcmp(now, orig, (size_t)ol)) abort();
    free(now);
    while (buf_redo(b)) {}
    now = buf_full_text(b, &nl);
    if (nl != fl || memcmp(now, final, (size_t)fl)) abort();
    free(now);
    free(orig);
    free(final);
    buf_free(b);
    return 0;
}
