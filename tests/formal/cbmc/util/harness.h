/*
 * Shared helpers for the CBMC proofs of src/util.c.
 *
 * Everything here builds NONDETERMINISTIC inputs: a buffer of nondet size
 * (within a stated bound) whose bytes are all nondet.  CBMC's malloc returns
 * an object whose contents are unconstrained, so a harness that only fixes
 * the size and the terminating NUL covers every string up to the bound
 * (including strings with embedded NULs, i.e. every shorter string living
 * in a larger object).
 */
#ifndef CBMC_UTIL_HARNESS_H
#define CBMC_UTIL_HARNESS_H

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int nondet_int(void);
unsigned nondet_unsigned(void);
size_t nondet_size_t(void);
char nondet_char(void);
long nondet_long(void);

/* Platform selector consumed by the plat_* stubs in stubs.c: 0 = POSIX
   (platform.c), 1 = Windows (platform_win32.c).  Every harness sets it
   nondeterministically so each proof covers both real implementations. */
extern int cbmc_plat_win;
#define HARNESS_PLATFORM() (cbmc_plat_win = (nondet_int() != 0))

/* malloc for the HARNESS's own inputs: never fails (the input exists). */
static inline void *h_malloc(size_t n) {
    void *p = malloc(n);
    __CPROVER_assume(p != NULL);
    return p;
}

/* A NUL-terminated string in an object of nondet size 1..max+1. */
static inline char *nondet_cstr(size_t max) {
    size_t n = nondet_size_t();
    char *s;
    __CPROVER_assume(n <= max);
    s = h_malloc(n + 1);
    s[n] = 0;
    return s;
}

/* A NUL-terminated string of EXACTLY n bytes (no interior NUL) in an
   object of n + 1 bytes.  Used where a symbolic strlen makes the proof
   intractable: run.sh then runs the harness once per length 0..bound, so
   the runs together still cover every string up to the bound. */
static inline char *nondet_cstr_exact(size_t n) {
    char *s = h_malloc(n + 1);
    size_t k;
    for (k = 0; k < n; k++) __CPROVER_assume(s[k] != 0);
    s[n] = 0;
    return s;
}

/* The Str invariant every Str function relies on and (as proved by the
   str_* harnesses) re-establishes:
     s == NULL  ->  len == 0 && cap == 0                 (str_init state)
     s != NULL  ->  s is a heap object of exactly cap bytes,
                    0 <= len < cap, s[len] == 0
   Direct writers outside util.c (backup.c, input.c) only shrink len and
   re-terminate, which preserves it. */
static inline int str_inv(const Str *s) {
    if (!s->s) return s->len == 0 && s->cap == 0;
    return s->cap >= 1 && s->len >= 0 && s->len < s->cap &&
           __CPROVER_OBJECT_SIZE(s->s) == (size_t)s->cap &&
           __CPROVER_POINTER_OFFSET(s->s) == 0 && s->s[s->len] == 0;
}

/* Assertions about buffer CONTENTS after memcpy/memmove/realloc.  With a
   fully symbolic (INT_MAX-sized) object they make the array decision
   procedure blow up, so the Str proofs run twice: the safety proof
   (symbolic capacity up to INT_MAX, all UB/memory checks, structural
   asserts) and the content proof (-DCBMC_CONTENT, small capacity bound,
   additionally these assertions). */
#ifdef CBMC_CONTENT
#define CONTENT_ASSERT(c, msg) __CPROVER_assert((c), msg)
/* content runs also bound the byte counts (loops in the copy models) */
#define CONTENT_BOUND(x) __CPROVER_assume((x) <= CBMC_CAP)
#else
#define CONTENT_ASSERT(c, msg) ((void)0)
#define CONTENT_BOUND(x) ((void)0)
#endif

/* Upper bound on a nondet Str's capacity (run.sh sets it per proof). */
#ifndef CBMC_CAP
#define CBMC_CAP INT_MAX
#endif

/* An arbitrary Str satisfying str_inv with cap <= maxcap (maxcap may be
   INT_MAX: the contents are never iterated byte by byte, so the size can
   stay fully symbolic). */
static inline void nondet_str(Str *s, int maxcap) {
    if (nondet_int()) {
        str_init(s);
        return;
    }
    s->cap = nondet_int();
    s->len = nondet_int();
    __CPROVER_assume(s->cap >= 1 && s->cap <= maxcap);
    __CPROVER_assume(s->len >= 0 && s->len < s->cap);
    s->s = h_malloc((size_t)s->cap);
    s->s[s->len] = 0;
}

#endif
