/* Parses every micro syntax YAML file given on the command line and
 * compiles every regex in it.  Prints failures; exit 1 if any. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/conf.h"
#include "../src/regex.h"
#include "../src/util.h"

static int fails, total;

static void try_re(const char *file, const char *what, const char *pat) {
    char *err = NULL;
    Regex *re;
    if (getenv("SRT_VERBOSE")) fprintf(stderr, "[%s] %s\n", what, pat);
    re = re_compile(pat, 0, &err);
    total++;
    if (!re) {
        printf("%s: %s: %s\n    pattern: %s\n", file, what, err, pat);
        fails++;
    }
    free(err);
    re_free(re);
}

static void walk_rules(const char *file, CNode *rules) {
    int i, k;
    if (!rules || rules->type != CN_LIST) return;
    for (i = 0; i < rules->n; i++) {
        CNode *r = rules->vals[i];
        if (r->type != CN_MAP) continue;
        for (k = 0; k < r->n; k++) {
            CNode *v = r->vals[k];
            if (!strcmp(r->keys[k], "include")) continue;
            if (v->type == CN_STR) try_re(file, r->keys[k], v->str);
            else if (v->type == CN_MAP) {
                const char *s;
                if ((s = cnode_str(v, "start"))) try_re(file, "start", s);
                if ((s = cnode_str(v, "end"))) try_re(file, "end", s);
                if ((s = cnode_str(v, "skip"))) try_re(file, "skip", s);
                walk_rules(file, cnode_get(v, "rules"));
            }
        }
    }
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        char *err = NULL;
        char *text = read_file(argv[i], NULL);
        CNode *root, *det;
        if (!text) { printf("cannot read %s\n", argv[i]); fails++; continue; }
        root = yaml_parse(text, &err);
        if (err) { printf("%s: yaml: %s\n", argv[i], err); fails++; free(err); }
        if (!cnode_str(root, "filetype")) { printf("%s: no filetype\n", argv[i]); fails++; }
        det = cnode_get(root, "detect");
        if (det) {
            const char *s;
            if ((s = cnode_str(det, "filename"))) try_re(argv[i], "filename", s);
            if ((s = cnode_str(det, "header"))) try_re(argv[i], "header", s);
            if ((s = cnode_str(det, "signature"))) try_re(argv[i], "signature", s);
        }
        if (!cnode_get(root, "rules")) { printf("%s: no rules\n", argv[i]); fails++; }
        walk_rules(argv[i], cnode_get(root, "rules"));
        cnode_free(root);
        free(text);
    }
    printf("%d regexes, %d failures\n", total, fails);
    return fails != 0;
}
