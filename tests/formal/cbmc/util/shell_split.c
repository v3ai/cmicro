/*
 * shell_split(s, &argc) + free_strv(v), on both platforms (POSIX:
 * backslash escapes; Windows: backslashes literal except before '"'),
 * argc pointer NULL or not.
 * Precondition (callers: commands.c, backup.c pass a NUL-terminated
 * command line / option value): s is a non-NULL C string.
 *
 * harness: run.sh runs it once per EXACT input length L = 0..5
 * (CBMC_EXACT, CBMC_SLEN=L), so the runs together cover every line of
 * <= 5 bytes.  Bytes are drawn from {' ', '\t', '"', '\'', '\\', 'a'}:
 * shell_split compares bytes only against the first five, and copies every
 * other non-NUL byte without looking at it, so 'a' stands for all of them
 * (data independence) - memory safety for these inputs implies it for all.
 *
 * Checks: every UB/memory check (incl. reads past the NUL after a
 * trailing backslash or an unterminated quote), the vector is
 * NULL-terminated at index argc, each word is a NUL-terminated heap
 * string no longer than the input, argc <= (len + 1) / 2 (words are
 * separated), and free_strv releases everything (memory-leak-check).
 *
 * NOT covered: the argv vector's growth (cap 8 -> 16, needs >= 7 words,
 * i.e. a >= 13-byte line).  A harness for it (13 bytes forming 7 words)
 * was tried; CBMC 5.95 was killed for memory in propositional reduction
 * (MiniSat) and timed out with CVC4, even with the line structure fixed.
 */
#include "harness.h"

#ifndef CBMC_SLEN
#define CBMC_SLEN 8
#endif

/* Model used in place of str_putc for the calls made by shell_split
   (run.sh: goto-instrument --replace-calls str_putc:str_putc_model).  It
   is str_putc's fast path verbatim, and it ASSERTS (does not assume) that
   the fast path applies: shell_split reserves 16 bytes for each word
   (str_reserve(&cur, 0) on an empty Str gives cap 16) and words here are
   shorter than 16 bytes, so str_putc's slow path (str_putn -> realloc) is
   unreachable within the bound.  str_putc itself, both paths, is proved
   for every Str state in str_puts.c.  Inlining the real str_putc ~100
   times made symbolic execution of this harness intractable. */
void str_putc_model(Str *s, char c) {
    __CPROVER_assert(str_inv(s), "str_putc precondition: Str invariant");
    __CPROVER_assert(s->len + 1 < s->cap, "str_putc fast path applies");
    s->s[s->len++] = c;
    s->s[s->len] = 0;
}

static void check(const char *s, char **v, int argc, int have_argc) {
    size_t len = strlen(s);
    int i;
    __CPROVER_assert(v != NULL, "vector allocated");
    if (have_argc) {
        __CPROVER_assert(argc >= 0 && (size_t)argc <= (len + 1) / 2, "argc bounded by the input");
        __CPROVER_assert(v[argc] == NULL, "NULL-terminated at argc");
        for (i = 0; i < argc; i++)
            __CPROVER_assert(v[i] != NULL && strlen(v[i]) <= len, "words are strings within the input size");
    }
}

void harness(void) {
#ifdef CBMC_EXACT
    char *s = nondet_cstr_exact(CBMC_SLEN);
    size_t k;
    /* data independence: shell_split only compares bytes against these
       five; every other non-NUL byte takes the same path as 'a' */
    for (k = 0; k < CBMC_SLEN; k++)
        __CPROVER_assume(s[k] == ' ' || s[k] == '\t' || s[k] == '"' || s[k] == '\'' ||
                         s[k] == '\\' || s[k] == 'a');
#else
    char *s = nondet_cstr(CBMC_SLEN);
#endif
    char **v;
    int argc = -1, want = nondet_int() != 0;
    HARNESS_PLATFORM();
    v = shell_split(s, want ? &argc : NULL);
    check(s, v, argc, want);
    free_strv(v);
    free_strv(NULL); /* its NULL guard (a no-op), for coverage */
    free(s);
}
