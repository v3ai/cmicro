/* stdio.h (nolibc) */
#ifndef NOLIBC_STDIO_H
#define NOLIBC_STDIO_H
#include <stdarg.h>
#include <stddef.h>
#define EOF (-1)
#define BUFSIZ 8192
typedef struct nolibc_file FILE;
extern FILE *stdin, *stdout, *stderr;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
char *fgets(char *buf, int n, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int ferror(FILE *f);
int feof(FILE *f);
void clearerr(FILE *f);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);
int remove(const char *path);
#endif
