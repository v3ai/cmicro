/*
 * utf8_encode(cp, out): for EVERY int cp, writes only out[0..3] (out is
 * exactly 4 bytes here, the documented maximum; callers pass char[4..16]
 * buffers, or one with >= 5 bytes when they terminate the result) and
 * the output is valid UTF-8 that utf8_decode maps back to the scalar value
 * (cp itself, or U+FFFD for values that are not Unicode scalar values).
 * Loop-free: complete proof for all inputs.
 */
#include "harness.h"

void harness(void) {
    int cp = nondet_int();
    char *out = h_malloc(4); /* exactly 4: any write past out[3] fails */
    int n, back = -1, m;

    n = utf8_encode(cp, out);
    __CPROVER_assert(n >= 1 && n <= 4, "returns 1..4");

    m = utf8_decode(out, n, &back);
    __CPROVER_assert(m == n, "decoder consumes the whole encoding (valid UTF-8)");
    if (cp >= 0 && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF))
        __CPROVER_assert(back == cp, "round trip of a scalar value");
    else
        __CPROVER_assert(back == 0xFFFD, "non-scalar values encode U+FFFD");
    /* shortest form: length is determined by the value */
    if (back < 0x80) __CPROVER_assert(n == 1, "1-byte form");
    else if (back < 0x800) __CPROVER_assert(n == 2, "2-byte form");
    else if (back < 0x10000) __CPROVER_assert(n == 3, "3-byte form");
    else __CPROVER_assert(n == 4, "4-byte form");
    free(out);
}
