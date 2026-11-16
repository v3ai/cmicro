/* ctype.h (nolibc) - the "C" locale: ASCII only */
#ifndef NOLIBC_CTYPE_H
#define NOLIBC_CTYPE_H
int isalnum(int c);
int isalpha(int c);
int isdigit(int c);
int islower(int c);
int isupper(int c);
int isspace(int c);
int isxdigit(int c);
int isprint(int c);
int isgraph(int c);
int ispunct(int c);
int iscntrl(int c);
int tolower(int c);
int toupper(int c);
#endif
