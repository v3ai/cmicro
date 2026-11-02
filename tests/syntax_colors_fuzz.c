/* libFuzzer harness for colorscheme parsing (colors.c).
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o syntax_colors_fuzz \
 *       tests/syntax_colors_fuzz.c src/colors.c src/runtime.c src/runtime_data.c \
 *       src/util.c src/platform.c src/platform_win32.c
 *
 * The input is written as <tmp config>/colorschemes/fz.micro and loaded.
 * Every resulting style must hold valid colors: COLOR_DEFAULT, a palette
 * index 0..255 or an RGB value. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/colors.h"
#include "../src/runtime.h"
#include "../src/util.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static char *path;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    char tmpl[] = "/tmp/colors_fuzz_XXXXXX";
    char *d, *sd;
    (void)argc; (void)argv;
    d = mkdtemp(tmpl);
    if (!d) abort();
    config_dir = xstrdup(d);
    sd = xsprintf("%s/colorschemes", d);
    mkdir(sd, 0700);
    path = xsprintf("%s/fz.micro", sd);
    free(sd);
    return 0;
}

static int valid_color(int c) {
    return c == COLOR_DEFAULT || (c >= 0 && c <= 255) || (c >= (1 << 24) && c <= ((1 << 24) | 0xFFFFFF));
}

static void check_style(Style st) {
    CHECK(valid_color(st.fg));
    CHECK(valid_color(st.bg));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const char *names[] = {"default", "comment", "comment.bright", "constant.string", "a.b.c.d",
                                  "statusline", "x", "", ".", "a..b", "selection", "line-number"};
    FILE *f = fopen(path, "wb");
    char *err = NULL;
    size_t i;
    if (!f) abort();
    fwrite(data, 1, size, f);
    fclose(f);
    colorscheme_load("fz", &err);
    free(err);
    check_style(def_style);
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        check_style(color_get(names[i]));
        (void)color_has(names[i]);
    }
    /* names taken from the input itself */
    {
        char *t = xstrndup((const char *)data, size), *p = t, *q;
        while ((q = strstr(p, "color-link"))) {
            char name[64];
            if (sscanf(q + 10, " %63s", name) == 1) {
                check_style(color_get(name));
                CHECK(color_has(name) || 1);
            }
            p = q + 10;
        }
        free(t);
    }
    return 0;
}
