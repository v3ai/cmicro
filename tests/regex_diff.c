/* Differential-test driver for src/regex.c (see tests/regex_diff/run.sh).
 *
 * Reads lines "FLAGS xHEXPATTERN xHEXSUBJECT" from stdin (FLAGS: i = ignore
 * case, l = literal, - = none) and prints, per line, either "E" (compile
 * error) or every match found with Go's FindAllSubmatchIndex iteration
 * rules, as space-separated capture offsets, matches separated by ';'.
 * tests/regex_diff/godiff.go prints the same for Go's regexp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/regex.h"
#include "../src/util.h"

static int unhex(const char *h, char *out) {
    int n = 0;
    while (h[0] && h[1] && h[0] != ' ' && h[0] != '\n') {
        unsigned v = 0;
        sscanf(h, "%2x", &v);
        out[n++] = (char)v;
        h += 2;
    }
    out[n] = 0;
    return n;
}

int main(void) {
    static char line[1 << 22];
    static char pat[1 << 21], subj[1 << 21];
    while (fgets(line, sizeof line, stdin)) {
        char *a = strchr(line, ' '), *b;
        int flags = 0, slen, ncap, pos, prev_end, *caps, first = 1;
        Regex *re;
        char *err = NULL;
        if (!a) continue;
        b = strchr(a + 1, ' ');
        *a = 0;
        if (strchr(line, 'i')) flags |= RE_ICASE;
        if (strchr(line, 'l')) flags |= RE_LITERAL;
        unhex(a + 2, pat);
        slen = b ? unhex(b + 2, subj) : 0;
        subj[slen] = 0;
        re = re_compile(pat, flags, &err);
        if (!re) { puts("E"); free(err); continue; }
        ncap = re_ncaps(re);
        caps = malloc(sizeof(int) * 2 * (size_t)ncap);
        for (pos = 0, prev_end = -1; pos <= slen;) {
            int i, accept = 1;
            if (!re_search(re, subj, slen, pos, caps, ncap)) break;
            if (caps[1] == pos) {
                if (caps[0] == prev_end) accept = 0;
                pos = pos < slen ? utf8_next(subj, slen, pos) : slen + 1;
            } else {
                pos = caps[1];
            }
            prev_end = caps[1];
            if (accept) {
                if (!first) putchar(';');
                first = 0;
                for (i = 0; i < 2 * ncap; i++) printf(i ? " %d" : "%d", caps[i]);
            }
        }
        putchar('\n');
        free(caps);
        re_free(re);
    }
    return 0;
}
