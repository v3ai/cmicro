/*
 * regex.h - a small regular expression engine implementing the subset of
 * Go/RE2 syntax used by micro (syntax files, find, replace).
 *
 * Semantics: leftmost-first (like Go's regexp, Perl) over UTF-8 text, where
 * an invalid byte reads as U+FFFD; (?i) uses Go's simple case folding.
 * Like Go, re_compile refuses a pattern that is not valid UTF-8 (also with
 * RE_LITERAL), and a surrogate such as \x{D800} never matches.
 * Matching uses lazily built DFAs plus a Pike VM for submatches, so a search
 * takes O(prog * text) time at worst, needs no recursion, and keeps a bounded
 * cache of DFA states (1 MB per direction for typical patterns) inside the
 * Regex.  A Regex is therefore not thread-safe:
 * re_search updates that cache.
 */
#ifndef REGEX_H
#define REGEX_H

#define RE_ICASE   1   /* case-insensitive, like (?i) */
#define RE_LITERAL 2   /* treat pattern as a literal string */

typedef struct Regex Regex;

Regex *re_compile(const char *pattern, int flags, char **err); /* err: malloc'd message */
void   re_free(Regex *re);
int    re_ncaps(const Regex *re); /* number of capture groups incl. group 0 */

/* Search s[start..len) for the leftmost match.  '^' matches only at
 * offset 0 of s, '$' only at len (unless (?m)); \b and (?m)^ look at the
 * byte before start.  On success fills caps[0..2*ncap) (-1 for unmatched
 * groups) and returns 1.  caps may be NULL; a negative start counts as 0. */
int re_search(const Regex *re, const char *s, int len, int start, int *caps, int ncap);

/* 1 if matches never depend on where the subject string begins (no ^, \A,
   \b or \B), so a search result from an earlier offset can be reused */
int re_context_free(const Regex *re);

/* convenience: match anywhere in NUL-terminated string */
int re_match_str(const Regex *re, const char *s);

/* quote metacharacters (malloc'd) */
char *re_quote(const char *s);

#endif
