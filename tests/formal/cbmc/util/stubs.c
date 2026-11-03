/*
 * Stubs for what util.c uses from OUTSIDE its module (platform layer) and
 * for the one libc routine whose CBMC model is too weak to be useful
 * (vsnprintf: CBMC's model neither writes the buffer nor ties the return
 * value to the size argument).  Everything else (malloc/realloc/free,
 * memcpy/memmove/strlen/strcmp/strncmp, ctype, exit, fputs) uses CBMC's
 * own library models.
 *
 * Assumptions made by each stub are stated next to it.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

int nondet_int(void);
size_t nondet_size_t(void);

/* 0: POSIX build (src/platform.c), 1: Windows build (src/platform_win32.c).
   Set nondeterministically by each harness (HARNESS_PLATFORM()). */
int cbmc_plat_win;

/* Exact copies of both real implementations, selected by cbmc_plat_win. */
int plat_is_sep(int c) { return c == '/' || (cbmc_plat_win && c == '\\'); }

int plat_is_abs(const char *p) {
    if (!cbmc_plat_win) return p[0] == '/';
    if (plat_is_sep((unsigned char)p[0])) return 1;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
           plat_is_sep((unsigned char)p[2]);
}

/* plat_home_dir: NULL (no home) or a fresh heap string of nondet contents
   and length 0..CBMC_HOME_MAX, which the caller owns (as both real
   implementations return a malloc'd copy). */
#ifndef CBMC_HOME_MAX
#define CBMC_HOME_MAX 4
#endif
char *plat_home_dir(void) {
    size_t n = nondet_size_t();
    char *r;
    if (nondet_int()) return NULL;
    __CPROVER_assume(n <= CBMC_HOME_MAX);
    r = malloc(n + 1);
    if (!r) return NULL; /* the real ones return NULL when out of memory too */
    r[n] = 0;
    return r;
}

#ifdef CBMC_ABSTRACT_COPY
/* Safety-only models of memcpy/memmove (-DCBMC_ABSTRACT_COPY, used by the
   Str proofs whose objects have fully symbolic sizes up to INT_MAX, where
   CBMC's array-copy models make the decision procedure blow up).  They
   check exactly what the standard requires of the caller - source readable
   and destination writable for n bytes, and (memcpy) no overlap - and then
   make the destination bytes nondeterministic.  That over-approximates the
   real effect, so any memory-safety / UB property proved with them also
   holds for the real functions; only properties about the copied VALUES
   are lost (those are checked by the -DCBMC_CONTENT runs with CBMC's own
   models).  The Str code never branches on buffer contents, and every
   terminating NUL it relies on is written explicitly after the copy. */
void *memcpy(void *dst, const void *src, size_t n) {
    __CPROVER_assert(__CPROVER_r_ok(src, n), "memcpy: source readable");
    __CPROVER_assert(__CPROVER_w_ok(dst, n), "memcpy: destination writable");
    __CPROVER_assert(n == 0 || !__CPROVER_same_object(dst, src) ||
                     (char *)dst + n <= (char *)src || (char *)src + n <= (char *)dst,
                     "memcpy: no overlap");
    __CPROVER_havoc_slice(dst, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    __CPROVER_assert(__CPROVER_r_ok(src, n), "memmove: source readable");
    __CPROVER_assert(__CPROVER_w_ok(dst, n), "memmove: destination writable");
    __CPROVER_havoc_slice(dst, n);
    return dst;
}
#endif

#ifdef CBMC_LOOP_COPY
/* Byte-loop reference models of memcpy/memmove (-DCBMC_LOOP_COPY, used by
   the -DCBMC_CONTENT runs on small buffers): the C standard's semantics
   written out, with the same preconditions asserted.  Loops are bounded by
   n, i.e. by the proof's capacity bound (unwinding assertions on). */
void *memcpy(void *dst, const void *src, size_t n) {
    size_t i;
    __CPROVER_assert(__CPROVER_r_ok(src, n), "memcpy: source readable");
    __CPROVER_assert(__CPROVER_w_ok(dst, n), "memcpy: destination writable");
    __CPROVER_assert(n == 0 || !__CPROVER_same_object(dst, src) ||
                     (char *)dst + n <= (char *)src || (char *)src + n <= (char *)dst,
                     "memcpy: no overlap");
    for (i = 0; i < n; i++) ((char *)dst)[i] = ((const char *)src)[i];
    return dst;
}

/* CBMC 5.95's built-in memset on a pointer into a two-dimensional local
   array (glob_match's `unsigned char small[2][256]`) did not zero the bytes
   in a counterexample (small[0][1] == 128 right after memset(cur, 0, 3)),
   making the functional glob_match check fail spuriously.  This model is
   the plain standard semantics. */
void *memset(void *dst, int c, size_t n) {
    size_t i;
    __CPROVER_assert(__CPROVER_w_ok(dst, n), "memset: destination writable");
    for (i = 0; i < n; i++) ((unsigned char *)dst)[i] = (unsigned char)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    size_t i;
    __CPROVER_assert(__CPROVER_r_ok(src, n), "memmove: source readable");
    __CPROVER_assert(__CPROVER_w_ok(dst, n), "memmove: destination writable");
    if (!__CPROVER_same_object(dst, src) || (char *)dst < (const char *)src)
        for (i = 0; i < n; i++) ((char *)dst)[i] = ((const char *)src)[i];
    else
        for (i = n; i > 0; i--) ((char *)dst)[i - 1] = ((const char *)src)[i - 1];
    return dst;
}
#endif

/* vsnprintf model.
   - A "formatting job" has an output length L (any int >= -1; -1 models an
     encoding error).  util.c always calls vsnprintf twice with the same
     fmt/args: first with (NULL, 0) to measure, then with a buffer of
     L + 1 bytes.  The measuring call picks a fresh nondet L; the following
     call with a buffer reuses it (same format, same arguments), but may
     still fail with a negative result (allowed by the standard).
   - With a buffer: the buffer must be writable for `size` bytes (checked),
     min(L, size-1) bytes of nondet output are written, then a NUL. */
static int vsn_len;
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    (void)fmt;
    (void)ap;
    if (buf == NULL || size == 0) {
        __CPROVER_assert(size == 0, "vsnprintf: NULL buffer only with size 0");
        vsn_len = nondet_int();
        __CPROVER_assume(vsn_len >= -1);
#ifdef CBMC_CONTENT
        __CPROVER_assume(vsn_len <= CBMC_CAP); /* content runs: small buffers */
#endif
        return vsn_len;
    }
    __CPROVER_assert(__CPROVER_w_ok(buf, size), "vsnprintf: buffer writable for size bytes");
    if (vsn_len < 0 || nondet_int()) {
        if (vsn_len < 0) return vsn_len;
        return -1; /* second call failed although the first succeeded */
    }
    {
        size_t k = (size_t)vsn_len < size - 1 ? (size_t)vsn_len : size - 1;
        __CPROVER_havoc_slice(buf, k);
        buf[k] = 0;
    }
    return vsn_len;
}
