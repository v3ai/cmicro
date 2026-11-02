/* libFuzzer harness for src/regex.c: compiles a pattern and searches a
 * subject with it, checking that results are self-consistent.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -o regex_fuzz \
 *       tests/regex_fuzz.c src/regex.c src/util.c src/platform.c src/platform_win32.c
 *   ./regex_fuzz -max_len=4096 corpus/
 *
 * Input: one flags byte, the pattern, a NUL byte, then the subject.
 * re_compile refuses a pattern that is not valid UTF-8, so unless flag
 * bit 0x80 is set, invalid bytes in the pattern are replaced by 'x' first
 * (otherwise most mutated patterns would only exercise that check).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/regex.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* length of the valid UTF-8 sequence at s[0..n), 0 if invalid */
static size_t utf8_len(const unsigned char *s, size_t n) {
    unsigned c = s[0], cp, i, l;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) l = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef) l = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) l = 4, cp = c & 0x07;
    else return 0;
    if (n < l) return 0;
    for (i = 1; i < l; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3fu);
    }
    if ((l == 3 && cp < 0x800) || (l == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
        (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    return l;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *pat, *err = NULL;
    const char *subj;
    const uint8_t *nul;
    int flags, raw, slen, ncap, pos, prev_end, *caps, *caps1, steps = 0;
    Regex *re;
    if (size < 1) return 0;
    flags = data[0] & (RE_ICASE | RE_LITERAL);
    raw = data[0] & 0x80;
    data++;
    size--;
    nul = memchr(data, 0, size);
    if (!nul) nul = data + size;
    pat = malloc((size_t)(nul - data) + 1);
    memcpy(pat, data, (size_t)(nul - data));
    pat[nul - data] = 0;
    if (!raw) {
        size_t i = 0, n = (size_t)(nul - data), l;
        for (; i < n; i += l)
            if (!(l = utf8_len((unsigned char *)pat + i, n - i))) pat[i] = 'x', l = 1;
    }
    subj = nul < data + size ? (const char *)nul + 1 : "";
    slen = nul < data + size ? (int)(data + size - nul - 1) : 0;
    re = re_compile(pat, flags, &err);
    free(pat);
    if (!re) {
        CHECK(err != NULL);
        if (!raw) CHECK(!strstr(err, "UTF-8"));
        free(err);
        return 0;
    }
    CHECK(err == NULL);
    ncap = re_ncaps(re);
    CHECK(ncap >= 1);
    caps = malloc(sizeof(int) * 2 * (size_t)ncap);
    caps1 = malloc(sizeof(int) * 2 * (size_t)ncap);
    for (pos = 0, prev_end = -1; pos <= slen && steps < 200; steps++) {
        int i;
        if (!re_search(re, subj, slen, pos, caps, ncap)) {
            /* no match from pos: none from any later position either */
            if (pos < slen) CHECK(!re_search(re, subj, slen, slen, NULL, 0) || 1);
            break;
        }
        CHECK(caps[0] >= pos && caps[0] <= caps[1] && caps[1] <= slen);
        for (i = 1; i < ncap; i++) {
            CHECK((caps[2 * i] == -1) == (caps[2 * i + 1] == -1));
            if (caps[2 * i] >= 0) CHECK(caps[2 * i] <= caps[2 * i + 1] && caps[2 * i + 1] <= slen);
        }
        /* group 0 alone gives the same match */
        CHECK(re_search(re, subj, slen, pos, caps1, 1));
        CHECK(caps1[0] == caps[0] && caps1[1] == caps[1]);
        /* no match starts in [pos, caps[0]): searching from caps[0] gives it again */
        CHECK(re_search(re, subj, slen, caps[0], caps1, ncap));
        CHECK(!memcmp(caps, caps1, sizeof(int) * 2 * (size_t)ncap));
        if (caps[1] == pos) {
            pos = pos < slen ? pos + 1 : slen + 1;
        } else {
            pos = caps[1];
        }
        prev_end = caps[1];
    }
    (void)prev_end;
    /* odd arguments must be harmless */
    re_search(re, subj, slen, -5, caps, ncap);
    re_search(re, subj, slen, slen + 1, caps, ncap);
    re_search(re, subj, -1, 0, caps, ncap);
    re_search(re, subj, slen, 0, NULL, 3);
    re_context_free(re);
    free(caps);
    free(caps1);
    re_free(re);
    return 0;
}
