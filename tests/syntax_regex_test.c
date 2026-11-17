/* Parses every micro syntax YAML file given on the command line and
 * compiles every regex in it.  Prints failures; exit 1 if any.
 *
 * Every regex is also run over the text of the file it came from, and the
 * positions of all matches are folded into a hash.  The printed summary must
 * be identical on every platform and compiler (tools/check.sh compares it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/conf.h"
#include "../src/regex.h"
#include "../src/util.h"

static int fails, sum;
static long nmatches;
static p_u32 hash = 2166136261UL; /* FNV-1a */
static const char *subject;
static int subject_len;

static void hash_int(int v) {
    p_u32 u = (p_u32)v;
    int i;
    for (i = 0; i < 4; i++) {
        hash ^= (u >> (8 * i)) & 0xFFu;
        hash = (hash * 16777619UL) & 0xFFFFFFFFUL;
    }
}

static void try_re(const char *file, const char *what, const char *pat) {
    char *err = NULL;
    Regex *re;
    if (getenv("SRT_VERBOSE")) fprintf(stderr, "[%s] %s\n", what, pat);
    re = cm_re_compile(pat, 0, &err);
    sum++;
    if (!re) {
        printf("%s: %s: %s\n    pattern: %s\n", file, what, err, pat);
        fails++;
    } else {
        int caps[2], start = 0;
        hash_int(sum);
        while (start <= subject_len && cm_re_search(re, subject, subject_len, start, caps, 1)) {
            hash_int(caps[0]);
            hash_int(caps[1]);
            nmatches++;
            start = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
        }
    }
    free(err);
    cm_re_free(re);
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
                if ((s = cm_cnode_str(v, "start"))) try_re(file, "start", s);
                if ((s = cm_cnode_str(v, "end"))) try_re(file, "end", s);
                if ((s = cm_cnode_str(v, "skip"))) try_re(file, "skip", s);
                walk_rules(file, cm_cnode_get(v, "rules"));
            }
        }
    }
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        char *err = NULL;
        char *text = cm_read_file(argv[i], NULL);
        CNode *root, *det;
        if (!text) { printf("cannot read %s\n", argv[i]); fails++; continue; }
        subject = text;
        subject_len = (int)strlen(text);
        root = cm_yaml_parse(text, &err);
        if (err) { printf("%s: yaml: %s\n", argv[i], err); fails++; free(err); }
        if (!cm_cnode_str(root, "filetype")) { printf("%s: no filetype\n", argv[i]); fails++; }
        det = cm_cnode_get(root, "detect");
        if (det) {
            const char *s;
            if ((s = cm_cnode_str(det, "filename"))) try_re(argv[i], "filename", s);
            if ((s = cm_cnode_str(det, "header"))) try_re(argv[i], "header", s);
            if ((s = cm_cnode_str(det, "signature"))) try_re(argv[i], "signature", s);
        }
        if (!cm_cnode_get(root, "rules")) { printf("%s: no rules\n", argv[i]); fails++; }
        walk_rules(argv[i], cm_cnode_get(root, "rules"));
        cm_cnode_free(root);
        free(text);
    }
    printf("%d regexes, %d failures, %ld matches, hash %08lx\n", sum, fails, nmatches,
           (unsigned long)hash);
    return fails != 0;
}
