/* stdlib.h (nolibc) */
#ifndef NOLIBC_STDLIB_H
#define NOLIBC_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
int abs(int x);
long labs(long x);
char *getenv(const char *name);
int rand(void);
void srand(unsigned seed);
int atexit(void (*fn)(void));
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int system(const char *cmd);
#endif
