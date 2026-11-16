/*
 * bignum.c - exact binary <-> decimal conversion support for nolibc.
 */
#include <string.h>

#include "bignum.h"

void __nl_bn_set(bignum *b, unsigned long long v) {
    b->n = 0;
    while (v) {
        b->d[b->n++] = (unsigned int)v;
        v >>= 32;
    }
}

void __nl_bn_mul_add(bignum *b, unsigned int m, unsigned int add) {
    unsigned long long carry = add;
    int i;
    for (i = 0; i < b->n; i++) {
        carry += (unsigned long long)b->d[i] * m;
        b->d[i] = (unsigned int)carry;
        carry >>= 32;
    }
    if (carry && b->n < BN_LIMBS) b->d[b->n++] = (unsigned int)carry;
}

void __nl_bn_mul_pow10(bignum *b, int k) {
    static const unsigned int p10[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
                                         1000000000};
    for (; k >= 9; k -= 9) __nl_bn_mul_add(b, 1000000000u, 0);
    if (k > 0) __nl_bn_mul_add(b, p10[k], 0);
}

void __nl_bn_mul_pow5(bignum *b, int k) {
    static const unsigned int p5[14] = {1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125,
                                        9765625, 48828125, 244140625, 1220703125};
    for (; k >= 13; k -= 13) __nl_bn_mul_add(b, 1220703125u, 0);
    if (k > 0) __nl_bn_mul_add(b, p5[k], 0);
}

void __nl_bn_shl(bignum *b, int k) {
    int words = k / 32, bits = k % 32, i;
    if (b->n == 0) return;
    if (words + b->n + 1 > BN_LIMBS) words = BN_LIMBS - b->n - 1; /* cannot happen in practice */
    if (bits) {
        unsigned int top = b->d[b->n - 1] >> (32 - bits);
        for (i = b->n - 1; i > 0; i--) b->d[i] = (b->d[i] << bits) | (b->d[i - 1] >> (32 - bits));
        b->d[0] <<= bits;
        if (top) b->d[b->n++] = top;
    }
    if (words) {
        memmove(b->d + words, b->d, (size_t)b->n * sizeof b->d[0]);
        memset(b->d, 0, (size_t)words * sizeof b->d[0]);
        b->n += words;
    }
}

int __nl_bn_cmp(const bignum *a, const bignum *b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

unsigned int __nl_bn_div1e9(bignum *b) {
    unsigned long long rem = 0;
    int i;
    for (i = b->n - 1; i >= 0; i--) {
        unsigned long long cur = (rem << 32) | b->d[i];
        b->d[i] = (unsigned int)(cur / 1000000000u);
        rem = cur % 1000000000u;
    }
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
    return (unsigned int)rem;
}

int __nl_bn_to_dec(bignum *b, char *out, int cap) {
    /* collect base-10^9 chunks least significant first, then reverse */
    unsigned int chunks[BN_LIMBS * 32 / 29 + 2];
    int nc = 0, len = 0, i, j;
    while (b->n > 0) chunks[nc++] = __nl_bn_div1e9(b);
    for (i = nc - 1; i >= 0; i--) {
        char tmp[9];
        unsigned int c = chunks[i];
        for (j = 8; j >= 0; j--) { tmp[j] = (char)('0' + c % 10); c /= 10; }
        j = 0;
        if (i == nc - 1) while (j < 8 && tmp[j] == '0') j++; /* no leading zeros */
        for (; j < 9 && len < cap; j++) out[len++] = tmp[j];
    }
    return len;
}

/* All the decimal digits of a positive double: at most 767 significant
   digits plus leading zeros of the integer form, so 1100 is plenty. */
#define DTOA_MAX 1100

int __nl_dtoa(double v, int n, char *d, int *e) {
    static char all[DTOA_MAX];
    union { double f; unsigned long long u; } bits;
    unsigned long long m;
    int e2, k, len, i, up = 0;
    bignum b;

    bits.f = v;
    m = bits.u & ((1ULL << 52) - 1);
    e2 = (int)((bits.u >> 52) & 0x7ff);
    if (e2) m |= 1ULL << 52, e2 -= 1075;
    else e2 = -1074;
    /* v = m * 2^e2 = N * 10^-k */
    __nl_bn_set(&b, m);
    if (e2 >= 0) {
        __nl_bn_shl(&b, e2);
        k = 0;
    } else {
        __nl_bn_mul_pow5(&b, -e2);
        k = -e2;
    }
    len = __nl_bn_to_dec(&b, all, DTOA_MAX);
    *e = len - 1 - k;
    if (n < 0) return 0; /* exponent only */
    if (n <= len) {
        /* round at position n: compare the tail with one half */
        if (n < len) {
            int c = all[n] - '0';
            if (c > 5) up = 1;
            else if (c == 5) {
                for (i = n + 1; i < len && all[i] == '0'; i++)
                    ;
                if (i < len) up = 1;                                  /* above half */
                else up = n > 0 ? ((all[n - 1] - '0') & 1) : 0;       /* tie: to even */
            }
        }
        memcpy(d, all, (size_t)n);
    } else {
        memcpy(d, all, (size_t)len);
        memset(d + len, '0', (size_t)(n - len));
    }
    if (up) {
        for (i = n - 1; i >= 0; i--) {
            if (d[i] == '9') d[i] = '0';
            else { d[i]++; break; }
        }
        if (i < 0) { /* carried out of the first digit: 99.9 -> 100 */
            if (n > 0) memmove(d + 1, d, (size_t)(n - 1));
            d[0] = '1';
            ++*e;
            return n > 0 ? n : 1;
        }
    }
    return n;
}
