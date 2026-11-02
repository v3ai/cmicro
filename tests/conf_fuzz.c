/* libFuzzer harness for conf.c (YAML subset, lenient JSON, JSON writer).
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o conf_fuzz \
 *       tests/conf_fuzz.c src/conf.c src/util.c src/platform.c src/platform_win32.c
 *   ./conf_fuzz -max_len=65536 corpus/
 *
 * Checks, besides memory safety and bounded recursion:
 *  - yaml_parse_skip() gives the same tree and error as yaml_parse(),
 *    except that the root-level "rules" value is CN_NULL;
 *  - JSON that parses without error survives json_write_file() and a
 *    re-parse unchanged. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/conf.h"
#include "../src/util.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static int same(const CNode *a, const CNode *b, int skip_rules_at_root) {
    int i;
    if (!a || !b) return a == b;
    if (a->type != b->type || a->n != b->n) return 0;
    if ((a->str == NULL) != (b->str == NULL)) return 0;
    if (a->str && strcmp(a->str, b->str)) return 0;
    if (a->type == CN_BOOL && a->b != b->b) return 0;
    for (i = 0; i < a->n; i++) {
        if (a->type == CN_MAP) {
            if (strcmp(a->keys[i], b->keys[i])) return 0;
            if (skip_rules_at_root && !strcmp(a->keys[i], "rules")) {
                if (b->vals[i]->type != CN_NULL) return 0;
                continue;
            }
        }
        if (!same(a->vals[i], b->vals[i], 0)) return 0;
    }
    return 1;
}

static char tmpl[64];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text;
    int mode;
    if (size < 1) return 0;
    mode = data[0] % 3;
    text = xstrndup((const char *)data + 1, size - 1);
    if (mode == 0) {
        char *e1 = NULL, *e2 = NULL;
        CNode *a = yaml_parse(text, &e1), *b = yaml_parse_skip(text, "rules", &e2);
        CHECK(a && b);
        CHECK(same(a, b, 1));
        CHECK((e1 == NULL) == (e2 == NULL));
        if (e1) CHECK(!strcmp(e1, e2));
        (void)cnode_str(a, "filetype");
        (void)cnode_get(cnode_get(a, "detect"), "filename");
        cnode_free(a);
        cnode_free(b);
        free(e1);
        free(e2);
    } else if (mode == 1) {
        char *err = NULL;
        CNode *n = json_parse(text, &err);
        CHECK(n);
        if (!err) {
            char *back, *err2 = NULL;
            CNode *m;
            if (!tmpl[0]) sprintf(tmpl, "/tmp/conf_fuzz_%d.json", (int)(size_t)&tmpl % 100000);
            CHECK(json_write_file(tmpl, n) == 0);
            back = read_file(tmpl, NULL);
            CHECK(back);
            m = json_parse(back, &err2);
            if (err2) fprintf(stderr, "reparse error: %s\n%s\n", err2, back);
            CHECK(!err2);
            CHECK(same(n, m, 0));
            cnode_free(m);
            free(back);
        }
        free(err);
        cnode_free(n);
    } else {
        /* editing API on whatever the parser produced (maybe not a map) */
        CNode *n = json_parse(text, NULL), *v;
        size_t i;
        for (i = 0; i + 1 < size && i < 64; i += 2) {
            char key[2] = {(char)('a' + data[i] % 4), 0};
            if (data[i + 1] & 1) {
                v = cnode_new(CN_STR);
                v->str = xstrdup("x");
                cnode_set(n, key, v);
            } else {
                cnode_del(n, key);
            }
        }
        cnode_free(n);
    }
    free(text);
    return 0;
}
