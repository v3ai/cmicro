/*
 * read_file(path, &len) with stdio stubbed (the OS is external).
 *
 * File model (stated assumptions):
 *   - plat_fopen fails (NULL) or opens a file of nondet length FL, any
 *     long in [0, LONG_MAX] - no bound;
 *   - fseek may fail (e.g. a pipe or a special file); ftell returns -1 or
 *     ANY non-negative long, unrelated to FL (a file that grows or shrinks
 *     between ftell and fread, or a /proc file reporting size 0);
 *   - fread(ptr, 1, room, f) must be given room writable bytes (asserted),
 *     writes nondet data and returns min(room, bytes left); it may instead
 *     fail with a short count and set the error flag (I/O error);
 *   - ferror reports that flag; fclose closes (asserted: exactly once);
 *     clearerr clears the flag.
 * Allocations may fail (exit path in xrealloc).
 *
 * Because no byte loop runs over the data, this is complete for every file
 * length: the read loop runs at most ~33 times (the buffer roughly doubles
 * each round until INT_MAX - 2) and str_reserve's loop at most 32 times;
 * unwinding assertions prove both bounds.
 *
 * Checks: every UB/memory check, fread never writes outside the buffer,
 * the result is NULL or a heap string NUL-terminated at index *len with
 * *len == number of bytes delivered by fread, *len <= INT_MAX - 2, the
 * FILE is always closed exactly once, no leak on any path.
 */
#include <stdio.h>
#include "harness.h"
#include "platform.h"

static int file_open, file_closes, file_err;
static long file_len, file_pos, delivered;
static char the_file; /* identity of the one FILE */

FILE *plat_fopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    if (nondet_int()) return NULL;
    file_len = nondet_long();
    __CPROVER_assume(file_len >= 0);
#ifdef CBMC_FILE_MAX
    __CPROVER_assume(file_len <= CBMC_FILE_MAX);
#endif
    file_pos = 0;
    file_open = 1;
    return (FILE *)(void *)&the_file;
}

int fseek(FILE *f, long off, int whence) {
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "fseek on the open file");
    (void)whence;
    if (nondet_int()) return -1;
    file_pos = off; /* only (0, SEEK_END) and (0, SEEK_SET) are used */
    return 0;
}

long ftell(FILE *f) {
    long r = nondet_long();
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "ftell on the open file");
    __CPROVER_assume(r >= -1);
    return r;
}

void clearerr(FILE *f) {
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "clearerr on the open file");
    file_err = 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    size_t want = size * nmemb, n;
    long left = file_len - delivered;
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "fread on the open file");
    __CPROVER_assert(size == 1, "byte reads");
    __CPROVER_assert(__CPROVER_w_ok(ptr, want), "fread: destination writable");
    n = (unsigned long)left < want ? (size_t)left : want;
    if (nondet_int()) { /* I/O error: short read */
        size_t m = nondet_size_t();
        __CPROVER_assume(m < want && m <= n);
        n = m;
        file_err = 1;
    }
    __CPROVER_havoc_slice(ptr, n);
    delivered += (long)n;
    return n;
}

int ferror(FILE *f) {
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "ferror on the open file");
    return file_err;
}

int fclose(FILE *f) {
    __CPROVER_assert(f == (FILE *)(void *)&the_file && file_open, "fclose of the open file");
    file_open = 0;
    file_closes++;
    return 0;
}

void harness(void) {
    long len = -7;
    char *r;
    HARNESS_PLATFORM();
    r = read_file("x", nondet_int() ? &len : NULL);
    __CPROVER_assert(!file_open, "FILE closed on every path");
    __CPROVER_assert(file_closes <= 1, "closed at most once");
    if (r) {
        __CPROVER_assert(file_closes == 1 && !file_err, "success only after a clean read");
        __CPROVER_assert(delivered <= INT_MAX - 2, "length fits the Str limit");
        __CPROVER_assert(__CPROVER_POINTER_OFFSET(r) == 0 &&
                         __CPROVER_OBJECT_SIZE(r) > (size_t)delivered && r[delivered] == 0,
                         "NUL-terminated after the data");
        if (len != -7) __CPROVER_assert(len == delivered, "*len == bytes read");
        __CPROVER_assert(delivered <= file_len, "never more than the file");
        free(r);
    }
}
