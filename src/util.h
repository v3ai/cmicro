#ifndef UTIL_H
#define UTIL_H

#include <stdarg.h>
#include <stddef.h>

#include "port.h"

/* The editor needs int to be at least 32 bits (line lengths, file offsets,
   key codes).  POSIX guarantees this on every system the editor runs on. */
P_STATIC_ASSERT(INT_MAX >= 2147483647L, int_is_32_bits);

void *cm_xmalloc(size_t n);
void *cm_xcalloc(size_t n, size_t m);
void *cm_xrealloc(void *p, size_t n);
char *cm_xstrdup(const char *s);
char *cm_xstrndup(const char *s, size_t n);
char *cm_xsprintf(const char *fmt, ...);

/* growable string */
typedef struct {
    char *s;
    int len, cap;
} Str;

void cm_str_init(Str *s);
void cm_str_free(Str *s);
void cm_str_clear(Str *s);
void cm_str_reserve(Str *s, int n);
void cm_str_putn(Str *s, const char *p, int n);
void cm_str_puts(Str *s, const char *p);
void cm_str_putc(Str *s, char c);
void cm_str_printf(Str *s, const char *fmt, ...);
void cm_str_vprintf(Str *s, const char *fmt, va_list ap); /* C89 printf subset */
void cm_str_insert(Str *s, int at, const char *p, int n);
void cm_str_delete(Str *s, int at, int n);
char *cm_str_detach(Str *s); /* returns malloc'd, resets s */

/* utf-8 */
int  cm_utf8_decode(const char *s, int len, int *cp); /* returns bytes consumed (>=1) */
int  cm_utf8_encode(int cp, char *out);               /* returns bytes */
int  cm_utf8_next(const char *s, int len, int i);     /* index of next char */
int  cm_utf8_prev(const char *s, int i);              /* index of prev char */
int  cm_utf8_count(const char *s, int len);           /* number of characters */
int  cm_utf8_offset(const char *s, int len, int nchars); /* byte offset of char index */
int  cm_char_width(int cp);                           /* 0, 1, or 2 columns */
int  cm_is_word_char(int cp);
int  cm_is_space_char(int cp);

/* misc */
int   cm_streq(const char *a, const char *b);
int   cm_strieq(const char *a, const char *b);
int   cm_starts_with(const char *s, const char *prefix);
int   cm_ends_with(const char *s, const char *suffix);
const char *cm_path_last_sep(const char *p); /* last '/' */
char *cm_path_join(const char *a, const char *b);
const char *cm_path_base(const char *p);
char *cm_path_dir(const char *p);
char *cm_expand_home(const char *p);
char *cm_read_file(const char *path, long *len);  /* malloc'd, NUL terminated */
int   cm_glob_match(const char *pat, const char *s);
int   cm_clamped_atoi(const char *s); /* like atoi, but clamped to +-1e9 */
int   cm_clampi(int v, int lo, int hi);
int   cm_mini(int a, int b);
int   cm_maxi(int a, int b);
int   cm_digits(int n);

/* split command line like a shell (quotes, backslash escapes) */
char **cm_shell_split(const char *s, int *argc);
void   cm_free_strv(char **v);

#endif
