/*
 * glob_match(pat, s) (and the static glob_class), all byte values, on both
 * platforms (separator '/' or '/' and '\\').
 * Precondition (only caller: options.c, glob-keyed settings): both are
 * non-NULL NUL-terminated strings.
 *
 * With a symbolic strlen(s) the proof is intractable (the 2x256-byte state
 * arrays plus the heap alternative make every access a case split), so
 * run.sh runs `harness` once per pair of EXACT lengths (CBMC_EXACT:
 * strlen(p) == CBMC_PLEN, strlen(s) == CBMC_SLEN, contents otherwise
 * nondet); together the runs cover every pattern/string up to the bounds.
 *
 * harness       small-buffer path (strlen(s) < 256): safety for all
 *               length pairs up to 4 x 4.  With -DCBMC_REF (length pairs
 *               up to 2 x 2) additionally FUNCTIONAL: the O(len(p)*len(s))
 *               set-of-positions algorithm agrees with a direct
 *               backtracking reference matcher (ref below) written from
 *               the documented semantics.
 * harness_heap  strlen(s) == 255 (n + 1 == 256: the last length that
 *               still uses the stack buffer) and 256 (the heap buffer of
 *               2 * (n + 1) bytes), EMPTY pattern only: allocation size,
 *               memset of n + 1 bytes, the final cur[n] read and the
 *               free.  With a 1-byte pattern (even with string bytes
 *               reduced to the 4 classes glob_match can distinguish) CBMC
 *               was killed for memory; the DP loops over the heap buffer
 *               are therefore NOT proved beyond the small-buffer runs
 *               (same code, different base pointer).
 *
 * The small-buffer runs use -DCBMC_LOOP_COPY: CBMC's built-in memset model
 * did not zero glob_match's 2-D local array in a counterexample (see
 * stubs.c), which matters for the functional check; the heap runs keep
 * the built-in model (sound for safety: it only over-approximates the
 * values) because the loop model over 257 bytes ran out of memory.
 * util.c is #included (glob_class is static); the code under test is
 * unchanged.
 */
#include "harness.h"
#include "platform.h"
#include "../../../../src/util.c"

#ifndef CBMC_PLEN
#define CBMC_PLEN 4
#endif
#ifndef CBMC_SLEN
#define CBMC_SLEN 4
#endif

/* Reference: plain backtracking over the same syntax.
   '**' (plus one following separator) matches any run of characters,
   '*' any run without separators, '?' one non-separator character,
   [...] one character via glob_class, a separator matches any separator,
   anything else matches itself. */
#ifdef CBMC_REF
static int ref(const char *p, const char *s) {
    if (!*p) return !*s;
    if (p[0] == '*' && p[1] == '*') {
        const char *q = p + 2;
        if (plat_is_sep((unsigned char)*q)) q++;
        /* match nothing more, or consume one character and stay */
        return ref(q, s) || (*s && ref(p, s + 1));
    }
    if (*p == '*')
        return ref(p + 1, s) || (*s && !plat_is_sep((unsigned char)*s) && ref(p, s + 1));
    if (!*s) return 0;
    {
        int m;
        const char *np = p + 1;
        if (*p == '[') np = glob_class(p, *s, &m);
        else if (*p == '?') m = !plat_is_sep((unsigned char)*s);
        else m = *p == *s || (plat_is_sep((unsigned char)*p) && plat_is_sep((unsigned char)*s));
        return m && ref(np, s + 1);
    }
}
#endif

void harness(void) {
#ifdef CBMC_EXACT
    char *p = nondet_cstr_exact(CBMC_PLEN), *s = nondet_cstr_exact(CBMC_SLEN);
#else
    char *p = nondet_cstr(CBMC_PLEN), *s = nondet_cstr(CBMC_SLEN);
#endif
    int r;
    HARNESS_PLATFORM();
    r = glob_match(p, s);
    __CPROVER_assert(r == 0 || r == 1, "boolean result");
#ifdef CBMC_REF
    __CPROVER_assert(r == ref(p, s), "agrees with the backtracking reference");
#endif
    free(p);
    free(s);
}

void harness_heap(void) {
    char *p = nondet_cstr_exact(CBMC_PLEN), *s = nondet_cstr_exact(CBMC_SLEN);
    int r;
    HARNESS_PLATFORM();
    r = glob_match(p, s);
    __CPROVER_assert(r == 0 || r == 1, "boolean result");
    free(p);
    free(s);
}
