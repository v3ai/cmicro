/*
 * stdio.c (nolibc) - buffered FILE streams on raw file descriptors and a
 * printf-family formatter.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_sys.h"
#include "bignum.h"

struct nolibc_file {
    int fd;
    int readable, writable;
    int err, eof;
    unsigned char *buf;
    int cap;
    int rpos, rlen; /* read buffer window */
    int wlen;       /* pending output bytes */
    int unbuffered;
    int owns_buf;
    struct nolibc_file *next;
};

static unsigned char inbuf[BUFSIZ], outbuf[BUFSIZ];
static FILE f_stdin = {0, 1, 0, 0, 0, inbuf, BUFSIZ, 0, 0, 0, 0, 0, NULL};
static FILE f_stdout = {1, 0, 1, 0, 0, outbuf, BUFSIZ, 0, 0, 0, 0, 0, NULL};
static FILE f_stderr = {2, 0, 1, 0, 0, NULL, 0, 0, 0, 0, 1, 0, NULL};
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;
static FILE *open_files; /* fopen'd streams, for fflush(NULL) */

/* write all of buf, retrying on interrupts and partial writes */
static int write_all(int fd, const unsigned char *buf, size_t n) {
    while (n > 0) {
        long w = sys3(SYS_write, fd, (long)buf, (long)n);
        if (w == -K_EINTR || w == -K_EAGAIN) continue;
        if (w <= 0) return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int flush_one(FILE *f) {
    if (!f->writable || f->wlen == 0) return 0;
    if (write_all(f->fd, f->buf, (size_t)f->wlen) != 0) {
        f->err = 1;
        f->wlen = 0;
        return EOF;
    }
    f->wlen = 0;
    return 0;
}

void __nolibc_stdio_flush_all(void) {
    FILE *f;
    flush_one(stdout);
    flush_one(stderr);
    for (f = open_files; f; f = f->next) flush_one(f);
}

int fflush(FILE *f) {
    if (!f) {
        __nolibc_stdio_flush_all();
        return 0;
    }
    return flush_one(f);
}

FILE *fopen(const char *path, const char *mode) {
    int flags, plus = strchr(mode, '+') != NULL;
    long fd;
    FILE *f;
    switch (mode[0]) {
    case 'r': flags = plus ? K_O_RDWR : K_O_RDONLY; break;
    case 'w': flags = (plus ? K_O_RDWR : K_O_WRONLY) | K_O_CREAT | K_O_TRUNC; break;
    case 'a': flags = (plus ? K_O_RDWR : K_O_WRONLY) | K_O_CREAT | K_O_APPEND; break;
    default: return NULL;
    }
    fd = sys3(SYS_open, (long)path, flags | K_O_CLOEXEC, 0666);
    if (fd < 0) return NULL;
    f = calloc(1, sizeof *f);
    if (f) f->buf = malloc(BUFSIZ);
    if (!f || !f->buf) {
        free(f);
        sys1(SYS_close, fd);
        return NULL;
    }
    f->fd = (int)fd;
    f->readable = mode[0] == 'r' || plus;
    f->writable = mode[0] != 'r' || plus;
    f->cap = BUFSIZ;
    f->owns_buf = 1;
    f->next = open_files;
    open_files = f;
    return f;
}

int fclose(FILE *f) {
    int r = flush_one(f), e = f->err;
    FILE **pp;
    if (sys1(SYS_close, f->fd) < 0) r = EOF;
    for (pp = &open_files; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->owns_buf) free(f->buf);
    if (f != stdin && f != stdout && f != stderr) free(f);
    return (r || e) ? EOF : 0;
}

/* refill the read buffer; returns bytes available, 0 at EOF/error */
static int fill(FILE *f) {
    long n;
    if (f == stdin) flush_one(stdout); /* prompts appear before reading */
    do n = sys3(SYS_read, f->fd, (long)f->buf, f->cap);
    while (n == -K_EINTR);
    if (n < 0) { f->err = 1; return 0; }
    if (n == 0) { f->eof = 1; return 0; }
    f->rpos = 0;
    f->rlen = (int)n;
    return (int)n;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f) {
    size_t want = size * n, got = 0;
    unsigned char *out = buf;
    if (!f->readable || want == 0) return 0;
    while (got < want) {
        int avail = f->rlen - f->rpos;
        if (avail > 0) {
            size_t k = (size_t)avail < want - got ? (size_t)avail : want - got;
            memcpy(out + got, f->buf + f->rpos, k);
            f->rpos += (int)k;
            got += k;
            continue;
        }
        if (want - got >= (size_t)f->cap) {
            /* large read: straight into the caller's buffer */
            long r;
            do r = sys3(SYS_read, f->fd, (long)(out + got), (long)(want - got));
            while (r == -K_EINTR);
            if (r < 0) { f->err = 1; break; }
            if (r == 0) { f->eof = 1; break; }
            got += (size_t)r;
            continue;
        }
        if (!fill(f)) break;
    }
    return size ? got / size : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f) {
    size_t len = size * n;
    const unsigned char *p = buf;
    if (!f->writable || len == 0) return 0;
    if (f->unbuffered || len >= (size_t)f->cap) {
        if (flush_one(f) != 0) return 0;
        if (write_all(f->fd, p, len) != 0) { f->err = 1; return 0; }
        return n;
    }
    if ((size_t)(f->cap - f->wlen) < len && flush_one(f) != 0) return 0;
    memcpy(f->buf + f->wlen, p, len);
    f->wlen += (int)len;
    return n;
}

int fgetc(FILE *f) {
    if (f->rpos >= f->rlen && !fill(f)) return EOF;
    return f->buf[f->rpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

char *fgets(char *buf, int n, FILE *f) {
    int i = 0, c;
    if (n <= 0) return NULL;
    while (i < n - 1 && (c = fgetc(f)) != EOF) {
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = 0;
    return buf;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n || n == 0 ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

int ferror(FILE *f) { return f->err; }
int feof(FILE *f) { return f->eof; }
void clearerr(FILE *f) { f->err = f->eof = 0; }

int remove(const char *path) {
    long r = sys1(SYS_unlink, (long)path);
    return r < 0 ? -1 : 0;
}

/* ================================================================== */
/* formatting                                                          */

typedef struct {
    char *buf;  /* snprintf target (NULL for FILE output) */
    size_t cap;
    FILE *f;
    size_t total;
} Out;

static void out(Out *o, const char *s, size_t n) {
    if (o->f) {
        fwrite(s, 1, n, o->f);
    } else if (o->buf && o->total + 1 < o->cap) {
        size_t room = o->cap - 1 - o->total, k = n < room ? n : room;
        memcpy(o->buf + o->total, s, k);
    }
    o->total += n;
}

static void pad(Out *o, char c, int n) {
    char b[32];
    memset(b, c, sizeof b);
    while (n > 0) {
        int k = n < 32 ? n : 32;
        out(o, b, (size_t)k);
        n -= k;
    }
}

/* emit sign/prefix + zeros + digits, honoring width and flags */
static void emit_num(Out *o, const char *prefix, const char *digits, int ndig, int width, int left, int zero) {
    int plen = (int)strlen(prefix), len = plen + ndig;
    if (!left && !zero) pad(o, ' ', width - len);
    out(o, prefix, (size_t)plen);
    if (!left && zero) pad(o, '0', width - len);
    out(o, digits, (size_t)ndig);
    if (left) pad(o, ' ', width - len);
}

static int fmt_uint(char *end, unsigned long long v, int base, int upper) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = end;
    do {
        *--p = dig[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v);
    return (int)(end - p);
}

/* write fixed notation for digits d (first digit has exponent e) */
static int put_fixed(char *out, const char *d, int nd, int e, int prec, int alt) {
    int k = 0, i;
    if (e < 0) out[k++] = '0';
    else for (i = 0; i <= e; i++) out[k++] = i < nd ? d[i] : '0';
    if (prec == 0 && alt) out[k++] = '.';
    if (prec > 0) {
        out[k++] = '.';
        for (i = 1; i <= prec; i++) {
            int idx = e + i;
            out[k++] = (idx >= 0 && idx < nd) ? d[idx] : '0';
        }
    }
    return k;
}

static int put_exp(char *out, const char *d, int e, int prec, int upper, int alt) {
    int k = 0, i, ae = e < 0 ? -e : e;
    out[k++] = d[0];
    if (prec == 0 && alt) out[k++] = '.';
    if (prec > 0) {
        out[k++] = '.';
        for (i = 1; i <= prec; i++) out[k++] = d[i];
    }
    out[k++] = upper ? 'E' : 'e';
    out[k++] = e < 0 ? '-' : '+';
    if (ae >= 100) out[k++] = (char)('0' + ae / 100);
    out[k++] = (char)('0' + ae / 10 % 10);
    out[k++] = (char)('0' + ae % 10);
    return k;
}

static void fmt_float(Out *o, double v, int conv, int prec, int width, int left, int zero, const char *sign,
                      int alt) {
    static char d[400], buf[1600];
    char prefix[2] = {0, 0};
    int n = 0, e, upper = conv == 'F' || conv == 'E' || conv == 'G';
    int c = conv | 32; /* lowercase */
    if (prec < 0) prec = 6;
    if (prec > 300) prec = 300;
    if (v < 0 || (v == 0 && 1 / v < 0)) { prefix[0] = '-'; v = -v; }
    else if (*sign) prefix[0] = *sign;
    if (v != v || v > 1.7976931348623157e308) {
        const char *t = v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit_num(o, prefix, t, 3, width, left, 0);
        return;
    }
    if (c == 'g') {
        int p = prec == 0 ? 1 : prec;
        if (p > 350) p = 350;
        if (v != 0) __nl_dtoa(v, p, d, &e);
        else e = 0;
        if (v == 0) memset(d, '0', (size_t)p);
        if (e >= -4 && e < p) {
            n = put_fixed(buf, d, p, e, p - 1 - e, alt);
        } else {
            n = put_exp(buf, d, e, p - 1, upper, alt);
        }
        if (!alt) {
            /* strip trailing zeros of the fraction (and a bare '.') */
            char *ep = memchr(buf, upper ? 'E' : 'e', (size_t)n), *dot = memchr(buf, '.', (size_t)n);
            if (dot) {
                int end = ep ? (int)(ep - buf) : n, z = end;
                while (z > dot - buf + 1 && buf[z - 1] == '0') z--;
                if (z == dot - buf + 1) z--;
                memmove(buf + z, buf + end, (size_t)(n - end));
                n -= end - z;
            }
        }
    } else if (c == 'e') {
        int p = prec > 350 ? 350 : prec;
        if (v != 0) __nl_dtoa(v, p + 1, d, &e);
        else e = 0;
        if (v == 0) memset(d, '0', (size_t)(p + 1));
        n = put_exp(buf, d, e, p, upper, alt);
    } else {
        static char big[1500];
        int nd, got;
        if (v == 0) e = -1;
        else __nl_dtoa(v, -1, d, &e); /* exponent of the first digit */
        nd = e + 1 + prec;            /* digits up to the last printed place */
        if (v == 0 || nd < 0) {
            n = put_fixed(buf, "0", 0, -1, prec, alt);
        } else {
            got = __nl_dtoa(v, nd, big, &e);
            n = put_fixed(buf, big, got, e, prec, alt);
        }
    }
    emit_num(o, prefix, buf, n, width, left, zero);
}

static int format(Out *o, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, alt = 0;
        char sign[2] = {0, 0};
        const char *start = fmt;
        if (*fmt != '%') {
            const char *e = strchr(fmt, '%');
            size_t n = e ? (size_t)(e - fmt) : strlen(fmt);
            out(o, fmt, n);
            fmt += n - 1;
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') sign[0] = '+';
            else if (*fmt == ' ') { if (!sign[0]) sign[0] = ' '; }
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else { prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        }
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lng = 2;
            else if (*fmt == 'h') {}
            else break;
        }
        if (left) zero = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            char buf[32];
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            int n = fmt_uint(buf + sizeof buf, u, 10, 0);
            char pre[2] = {0, 0};
            if (v < 0) pre[0] = '-';
            else pre[0] = sign[0];
            if (prec >= 0) zero = 0;
            if (prec > n) {
                char z[64];
                int k = prec - n < 32 ? prec - n : 32;
                memset(z, '0', (size_t)k);
                memcpy(z + k, buf + sizeof buf - n, (size_t)n);
                emit_num(o, pre, z, k + n, width, left, zero);
            } else {
                emit_num(o, pre, buf + sizeof buf - n, n, width, left, zero);
            }
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            char buf[32];
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            int n = fmt_uint(buf + sizeof buf, v, base, *fmt == 'X');
            if (prec >= 0) zero = 0;
            while (prec > n && n < 30) { buf[sizeof buf - 1 - n] = '0'; n++; }
            emit_num(o, "", buf + sizeof buf - n, n, width, left, zero);
            break;
        }
        case 'p': {
            char buf[32];
            void *ptr = va_arg(ap, void *);
            int n;
            if (!ptr) { emit_num(o, "", "(nil)", 5, width, left, 0); break; }
            n = fmt_uint(buf + sizeof buf, (unsigned long long)(unsigned long)ptr, 16, 0);
            emit_num(o, "0x", buf + sizeof buf - n, n, width, left, 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_num(o, "", &c, 1, width, left, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n;
            if (!s) s = "(null)";
            n = (int)strlen(s);
            if (prec >= 0 && prec < n) n = prec;
            emit_num(o, "", s, n, width, left, 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_float(o, va_arg(ap, double), *fmt, prec, width, left, zero, sign, alt);
            break;
        case '%':
            out(o, "%", 1);
            break;
        case 0:
            fmt--;
            break;
        default:
            out(o, start, (size_t)(fmt - start + 1));
            break;
        }
    }
    return (int)o->total;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    Out o;
    int r;
    o.buf = buf;
    o.cap = n;
    o.f = NULL;
    o.total = 0;
    r = format(&o, fmt, ap);
    if (buf && n > 0) buf[o.total < n ? o.total : n - 1] = 0;
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 / 2, fmt, ap); }

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    Out o;
    o.buf = NULL;
    o.cap = 0;
    o.f = f;
    o.total = 0;
    return format(&o, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* sscanf: integers (%d, %ld) and literal text, which is all the editor needs */
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int count = 0;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == ' ') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            fmt++;
        } else if (fmt[0] == '%' && (fmt[1] == 'd' || (fmt[1] == 'l' && fmt[2] == 'd'))) {
            char *end;
            long v;
            int is_long = fmt[1] == 'l';
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            v = strtol(s, &end, 10);
            if (end == s) break;
            if (is_long) *va_arg(ap, long *) = v;
            else *va_arg(ap, int *) = (int)v;
            count++;
            s = end;
            fmt += is_long ? 3 : 2;
        } else {
            if (*fmt != *s) break;
            fmt++;
            s++;
        }
    }
    va_end(ap);
    return count;
}
