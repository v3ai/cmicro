#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

/* growable string */
typedef struct {
    char *s;
    int len, cap;
} Str;

void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);
void str_reserve(Str *s, int n);
void str_putn(Str *s, const char *p, int n);
void str_puts(Str *s, const char *p);
void str_putc(Str *s, char c);
void str_printf(Str *s, const char *fmt, ...);
void str_insert(Str *s, int at, const char *p, int n);
void str_delete(Str *s, int at, int n);
char *str_detach(Str *s); /* returns malloc'd, resets s */

/* utf-8 */
int  utf8_decode(const char *s, int len, int *cp); /* returns bytes consumed (>=1) */
int  utf8_encode(int cp, char *out);               /* returns bytes */
int  utf8_next(const char *s, int len, int i);     /* index of next char */
int  utf8_prev(const char *s, int i);              /* index of prev char */
int  utf8_count(const char *s, int len);           /* number of characters */
int  utf8_offset(const char *s, int len, int nchars); /* byte offset of char index */
int  char_width(int cp);                           /* 0, 1, or 2 columns */
int  is_word_char(int cp);
int  is_space_char(int cp);

/* misc */
int   streq(const char *a, const char *b);
int   strieq(const char *a, const char *b);
int   starts_with(const char *s, const char *prefix);
int   ends_with(const char *s, const char *suffix);
const char *path_last_sep(const char *p); /* last '/' (or '\\' on Windows) */
char *path_join(const char *a, const char *b);
const char *path_base(const char *p);
char *path_dir(const char *p);
char *expand_home(const char *p);
char *read_file(const char *path, long *len);  /* malloc'd, NUL terminated */
int   glob_match(const char *pat, const char *s);
int   clampi(int v, int lo, int hi);
int   mini(int a, int b);
int   maxi(int a, int b);
int   digits(int n);

/* split command line like a shell (quotes, backslash escapes) */
char **shell_split(const char *s, int *argc);
void   free_strv(char **v);

#endif
