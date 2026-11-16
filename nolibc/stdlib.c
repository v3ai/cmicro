/*
 * stdlib.c (nolibc) - program start, exit, environment, memory allocation,
 * number parsing, qsort, rand and system().
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_sys.h"
#include "bignum.h"

char **__nolibc_environ;
void __nolibc_stdio_flush_all(void);

/* ================================================================== */
/* start / exit                                                        */

int main(int argc, char **argv);
void __nolibc_start(long *sp) __attribute__((noreturn, used));

void __nolibc_start(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    __nolibc_environ = argv + argc + 1;
    exit(main(argc, argv));
}

static void (*atexit_fns[32])(void);
static int natexit;

int atexit(void (*fn)(void)) {
    if (natexit >= 32) return -1;
    atexit_fns[natexit++] = fn;
    return 0;
}

void exit(int status) {
    while (natexit > 0) atexit_fns[--natexit]();
    __nolibc_stdio_flush_all();
    sys1(SYS_exit_group, status);
    for (;;)
        ;
}

void abort(void) {
    sys2(SYS_kill, sys0(SYS_getpid), 6 /* SIGABRT */);
    sys1(SYS_exit_group, 127);
    for (;;)
        ;
}

char *getenv(const char *name) {
    size_t n = strlen(name);
    char **e;
    if (!__nolibc_environ) return NULL;
    for (e = __nolibc_environ; *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/* ================================================================== */
/* memory: size-class free lists carved from mmap'd arenas; big blocks
   get their own mapping (grown with mremap).                          */

#define HDR 16                    /* keeps payloads 16-byte aligned */
#define NCLASS 17                 /* block sizes 16 << 0 .. 16 << 16 (1 MiB) */
#define BIG ((size_t)16 << 16)
#define ARENA ((size_t)4 << 20)
#define TAG_MMAP 0xFFFFu

typedef struct {
    size_t usable; /* bytes available to the caller */
    size_t tag;    /* size class, or TAG_MMAP */
} Hdr;

static void *freelist[NCLASS];
static char *arena_cur, *arena_end;

static void *map(size_t n) {
    long p = sys6(SYS_mmap, 0, (long)n, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    return (p < 0 && p > -4096) ? NULL : (void *)p;
}

static size_t page_round(size_t n) { return (n + 4095) & ~(size_t)4095; }

void *malloc(size_t n) {
    size_t total = n + HDR;
    Hdr *h;
    int k = 1; /* smallest block is 32 bytes: a free block keeps its link in the payload */
    if (n > (size_t)LONG_MAX - 8192) return NULL;
    if (total > BIG) {
        size_t sz = page_round(total);
        h = map(sz);
        if (!h) return NULL;
        h->usable = sz - HDR;
        h->tag = TAG_MMAP;
        return (char *)h + HDR;
    }
    while (((size_t)16 << k) < total) k++;
    if (freelist[k]) {
        h = freelist[k];
        freelist[k] = *(void **)((char *)h + HDR);
    } else {
        size_t bs = (size_t)16 << k;
        if ((size_t)(arena_end - arena_cur) < bs) {
            char *a = map(ARENA);
            if (!a) return NULL;
            arena_cur = a;
            arena_end = a + ARENA;
        }
        h = (Hdr *)(void *)arena_cur;
        arena_cur += bs;
    }
    h->usable = ((size_t)16 << k) - HDR;
    h->tag = (size_t)k;
    return (char *)h + HDR;
}

void free(void *p) {
    Hdr *h;
    if (!p) return;
    h = (Hdr *)(void *)((char *)p - HDR);
    if (h->tag == TAG_MMAP) {
        sys2(SYS_munmap, (long)h, (long)(h->usable + HDR));
        return;
    }
    *(void **)p = freelist[h->tag];
    freelist[h->tag] = h;
}

void *calloc(size_t n, size_t m) {
    void *p;
    if (m && n > (size_t)LONG_MAX / m) return NULL;
    p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void *realloc(void *p, size_t n) {
    Hdr *h;
    void *q;
    if (!p) return malloc(n);
    h = (Hdr *)(void *)((char *)p - HDR);
    if (n <= h->usable) return p;
    if (h->tag == TAG_MMAP) {
        size_t sz = page_round(n + HDR);
        long r = sys5(SYS_mremap, (long)h, (long)(h->usable + HDR), (long)sz, K_MREMAP_MAYMOVE, 0);
        if (r < 0 && r > -4096) return NULL;
        h = (Hdr *)r;
        h->usable = sz - HDR;
        return (char *)h + HDR;
    }
    q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->usable);
    free(p);
    return q;
}

/* ================================================================== */
/* numbers                                                             */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* Parse digits; *ovf is set when the magnitude exceeds ULONG_MAX. */
static unsigned long parse_ul(const char *s, char **end, int base, int *neg, int *ovf) {
    const char *p = s;
    unsigned long v = 0;
    int any = 0;
    *neg = 0;
    *ovf = 0;
    while (isspace(*p)) p++;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        ((p[2] >= '0' && p[2] <= '9') || ((p[2] | 32) >= 'a' && (p[2] | 32) <= 'f'))) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        if (v > (~0UL - (unsigned long)d) / (unsigned long)base) *ovf = 1;
        else v = v * (unsigned long)base + (unsigned long)d;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long v = parse_ul(s, end, base, &neg, &ovf);
    if (ovf) return ~0UL;
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long v = parse_ul(s, end, base, &neg, &ovf);
    unsigned long lim = neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
    if (ovf || v > lim) return neg ? LONG_MIN : LONG_MAX;
    return neg ? (long)(0 - v) : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

static double pow10_int(int e) {
    double r = 1.0, b = 10.0;
    int neg = e < 0;
    if (neg) e = -e;
    while (e) {
        if (e & 1) r *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? 1.0 / r : r;
}

static int lower_eq(const char *s, const char *w) {
    for (; *w; s++, w++)
        if ((*s | 32) != *w) return 0;
    return 1;
}

/* Is the decimal value digits * 10^dexp above (1), at (0) or below (-1)
   x * 2^f?  Exact, with big integers. */
static int cmp_dec_bin(const bignum *digits, int dexp, unsigned long long x, int f) {
    static bignum a, b;
    a = *digits;
    __nl_bn_set(&b, x);
    if (dexp >= 0) __nl_bn_mul_pow10(&a, dexp);
    else __nl_bn_mul_pow10(&b, -dexp);
    if (f >= 0) __nl_bn_shl(&b, f);
    else __nl_bn_shl(&a, -f);
    return __nl_bn_cmp(&a, &b);
}

#define SD_MAX 800 /* significant digits kept; 767 decide any double */

/* Correctly rounded (round-half-to-even) decimal to double conversion:
   take a close approximation, then step it one unit in the last place at
   a time until the exact decimal value lies within half a unit of it. */
double strtod(const char *s, char **end) {
    const char *p = s;
    static char sd[SD_MAX + 1];
    int nsd = 0, dexp = 0, neg = 0, any = 0, sticky = 0, i;
    unsigned long long m;
    int e2;
    double v;
    static bignum dig;

    while (isspace(*p)) p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (lower_eq(p, "inf")) {
        p += lower_eq(p, "infinity") ? 8 : 3;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (lower_eq(p, "nan")) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    m = 0;
    v = 0;
    if (p[0] == '0' && (p[1] | 32) == 'x') {
        const char *q = p + 2;
        int hexp = 0, hany = 0;
        for (;; q++) {
            int d = *q >= '0' && *q <= '9' ? *q - '0' : ((*q | 32) >= 'a' && (*q | 32) <= 'f') ? (*q | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            if (m >> 56) hexp += 4; else m = m * 16 + (unsigned)d;
            hany = 1;
        }
        if (*q == '.') {
            for (q++;; q++) {
                int d = *q >= '0' && *q <= '9' ? *q - '0' : ((*q | 32) >= 'a' && (*q | 32) <= 'f') ? (*q | 32) - 'a' + 10 : -1;
                if (d < 0) break;
                if (!(m >> 56)) { m = m * 16 + (unsigned)d; hexp -= 4; }
                hany = 1;
            }
        }
        if (hany) {
            if ((*q | 32) == 'p') {
                const char *r = q + 1;
                int eneg = 0, ev = 0, eany = 0;
                if (*r == '+' || *r == '-') eneg = *r++ == '-';
                for (; *r >= '0' && *r <= '9'; r++, eany = 1)
                    if (ev < 100000) ev = ev * 10 + (*r - '0');
                if (eany) { hexp += eneg ? -ev : ev; q = r; }
            }
            if (end) *end = (char *)q;
            v = (double)m;
            while (hexp > 0) { v *= 2; hexp--; }
            while (hexp < 0) { v /= 2; hexp++; }
            return neg ? -v : v;
        }
        /* "0x" with no hex digits parses as the number 0 */
        if (end) *end = (char *)(p + 1);
        return neg ? -0.0 : 0.0;
    }
    for (; *p >= '0' && *p <= '9'; p++, any = 1) {
        if (nsd == 0 && *p == '0') continue; /* leading zeros */
        if (nsd < SD_MAX) sd[nsd++] = *p;
        else { dexp++; if (*p != '0') sticky = 1; }
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++, any = 1) {
            if (nsd == 0 && *p == '0') { dexp--; continue; }
            if (nsd < SD_MAX) { sd[nsd++] = *p; dexp--; }
            else if (*p != '0') sticky = 1;
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, ev = 0, eany = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        for (; *q >= '0' && *q <= '9'; q++, eany = 1)
            if (ev < 100000) ev = ev * 10 + (*q - '0');
        if (eany) {
            dexp += eneg ? -ev : ev;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    /* a digit past the kept ones makes the value strictly larger than the
       kept prefix, which is all that matters for rounding */
    if (sticky) { sd[nsd++] = '1'; dexp--; }
    while (nsd > 0 && sd[nsd - 1] == '0') { nsd--; dexp++; }
    if (nsd == 0) return neg ? -0.0 : 0.0;
    if (nsd - 1 + dexp > 310) return neg ? -__builtin_inf() : __builtin_inf();
    if (nsd - 1 + dexp < -330) return neg ? -0.0 : 0.0;

    /* exact fast path: up to 15 digits and an exactly representable 10^k */
    m = 0;
    for (i = 0; i < nsd && i < 19; i++) m = m * 10 + (unsigned)(sd[i] - '0');
    if (nsd <= 15 && dexp >= -22 && dexp <= 22) {
        v = (double)m;
        v = dexp >= 0 ? v * pow10_int(dexp) : v / pow10_int(-dexp);
        return neg ? -v : v;
    }
    /* approximation from the first 19 digits */
    {
        int ex = dexp + (nsd > 19 ? nsd - 19 : 0);
        v = (double)m;
        if (ex < -300) v = v * pow10_int(-300) * pow10_int(ex + 300);
        else if (ex > 300) v = v * pow10_int(300) * pow10_int(ex - 300);
        else v *= pow10_int(ex);
    }
    /* exact digits as a big integer */
    dig.n = 0;
    for (i = 0; i + 9 <= nsd; i += 9) {
        unsigned int c = 0;
        int j;
        for (j = 0; j < 9; j++) c = c * 10 + (unsigned)(sd[i + j] - '0');
        __nl_bn_mul_add(&dig, 1000000000u, c);
    }
    if (i < nsd) {
        unsigned int c = 0, mul = 1;
        for (; i < nsd; i++) { c = c * 10 + (unsigned)(sd[i] - '0'); mul *= 10; }
        __nl_bn_mul_add(&dig, mul, c);
    }
    /* candidate as m * 2^e2 with m < 2^53 */
    {
        union { double f; unsigned long long u; } bits;
        bits.f = v;
        if (v != v || v > 1.7976931348623157e308) {
            m = (1ULL << 53) - 1;
            e2 = 971;
        } else {
            m = bits.u & ((1ULL << 52) - 1);
            e2 = (int)((bits.u >> 52) & 0x7ff);
            if (e2) { m |= 1ULL << 52; e2 -= 1075; }
            else e2 = -1074;
        }
    }
    for (;;) {
        int c = cmp_dec_bin(&dig, dexp, 2 * m + 1, e2 - 1); /* vs upper midpoint */
        if (c > 0 || (c == 0 && (m & 1))) {
            m++;
            if (m == 1ULL << 53) { m >>= 1; e2++; }
            if (e2 > 971) return neg ? -__builtin_inf() : __builtin_inf();
            continue;
        }
        if (m == 0) break;
        if (m == 1ULL << 52 && e2 > -1074)
            c = cmp_dec_bin(&dig, dexp, 4 * m - 1, e2 - 2); /* below a power of two */
        else
            c = cmp_dec_bin(&dig, dexp, 2 * m - 1, e2 - 1);
        if (c < 0 || (c == 0 && (m & 1))) {
            m--;
            if (m < 1ULL << 52 && e2 > -1074) { m = (m << 1) | 1; e2--; }
            continue;
        }
        break;
    }
    /* assemble m * 2^e2 */
    {
        union { double f; unsigned long long u; } bits;
        if (m >= 1ULL << 52) bits.u = ((unsigned long long)(e2 + 1075) << 52) | (m & ((1ULL << 52) - 1));
        else bits.u = m; /* subnormal (e2 == -1074) or zero */
        v = bits.f;
    }
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, NULL); }

/* ================================================================== */
/* qsort (heapsort: in place, O(n log n), no recursion)                */

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift(char *base, size_t size, size_t root, size_t n, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1, big = root;
        if (child < n && cmp(base + child * size, base + big * size) > 0) big = child;
        if (child + 1 < n && cmp(base + (child + 1) * size, base + big * size) > 0) big = child + 1;
        if (big == root) return;
        swap_bytes(base + root * size, base + big * size, size);
        root = big;
    }
}

void qsort(void *b, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *base = b;
    size_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) sift(base, size, i, n, cmp);
    for (i = n - 1; i > 0; i--) {
        swap_bytes(base, base + i * size, size);
        sift(base, size, 0, i, cmp);
    }
}

/* ================================================================== */
/* rand                                                                */

static unsigned long rand_next = 1;
int rand(void) {
    rand_next = rand_next * 1103515245 + 12345;
    return (int)((rand_next / 65536) % 32768);
}
void srand(unsigned seed) { rand_next = seed; }

/* ================================================================== */
/* system(): fork, exec /bin/sh -c, wait                               */

static void set_sig(int sig, void (*h)(int), void (**old)(int)) {
    struct k_sigaction sa, oa;
    memset(&sa, 0, sizeof sa);
    sa.handler = h;
    sys4(SYS_rt_sigaction, sig, (long)&sa, (long)&oa, 8);
    if (old) *old = oa.handler;
}

int system(const char *cmd) {
    long pid, r;
    int status = 0;
    void (*oint)(int), (*oquit)(int);
    if (!cmd) return 1;
    __nolibc_stdio_flush_all();
    /* like the C library's system(): the caller ignores ^C / ^\ meanwhile */
    set_sig(K_SIGINT, K_SIG_IGN, &oint);
    set_sig(K_SIGQUIT, K_SIG_IGN, &oquit);
    pid = sys0(SYS_fork);
    if (pid == 0) {
        char *argv[4];
        argv[0] = (char *)"sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)cmd;
        argv[3] = NULL;
        set_sig(K_SIGINT, oint, NULL);
        set_sig(K_SIGQUIT, oquit, NULL);
        sys3(SYS_execve, (long)"/bin/sh", (long)argv, (long)__nolibc_environ);
        sys1(SYS_exit_group, 127);
    }
    if (pid < 0) {
        status = -1;
    } else {
        do r = sys4(SYS_wait4, pid, (long)&status, 0, 0);
        while (r == -K_EINTR);
        if (r < 0) status = -1;
    }
    set_sig(K_SIGINT, oint, NULL);
    set_sig(K_SIGQUIT, oquit, NULL);
    return status;
}
