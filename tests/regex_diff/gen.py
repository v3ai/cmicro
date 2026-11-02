#!/usr/bin/env python3
"""Generate input for the regex differential test (see run.sh).

  gen.py random N SEED          N random RE2 patterns, each with subjects
  gen.py syntax PATFILE LINES   every pattern in PATFILE against sample lines

Output lines: "FLAGS xHEXPATTERN xHEXSUBJECT".
"""
import random
import sys

ATOMS = ['a', 'b', 'c', 'x', 'A', 'B', '.', '\\.', '\\w', '\\W', '\\d', '\\D', '\\s', '\\S',
         '[abc]', '[^ab]', '[a-c]', '[[:alpha:]]', '[[:^digit:]]', '[\\d\\s]', '[^\\n]',
         '\\b', '\\B', '^', '$', '\\A', '\\z', 'é', 'ü', '日', '\\x41', '\\x{e9}', '\\pL',
         '[é-ü]', '\\n', ' ', '-', '_', '1', '\\*', '\\Qa.b\\E', '[]a]', '[^]a]', '[a-]',
         '(?i:ab)', '(?s:.)', '(?m:^)', '(?m:$)', '(?U:a*)', '[\\pN]', 'Σ', 'σ', 'k', 'K', 's',
         'ß', 'Å', '\u212a', 'ſ', '[k-s]', '[^k]', '(?i:[^k])', '[\\W]', '(?i:\\W)', '(?i:[^\\w])',
         '[A-Za-z]', '[^\\x00-\\x{10FFFF}]', '[\\x00-\\x{10FFFF}]', '[[:^alpha:]]',
         '\\x{FFFD}', '\\x{80}', '\\0', '\\012']
# (?i) is only used for a whole group: Go's alternation factoring mixes up
# case-sensitive and -insensitive prefixes ("K.|(?i)k." is parsed as "K(?:.|.)").
# \pL, \p{Greek} etc. are approximations, so only used where they are exact.
# syntax errors and other edge cases (each also checked against Go)
ODD = ['(?', ')', '(', '[', '{', '{2}', '{,3}', '{01}', '\\', '(?-i)', '(?i-s:a)', '(?-)', '(?i-)',
       '(?--i)', '(?P<', '(?P<x>a)', '(?P<>a)', '(?<y>b)', '(?P=x)', '\\1', '\\12', '\\8',
       '\\x{110000}', '\\x{}', '\\x{10FFFF}', '\\xg', '[z-a]', '[[:foo:]]', 'a{1001}',
       'x{2}{3}', '(?:)', '()', '|', '\\Q\\E', '\\Qa', '\\E', '\\z*', '^*', '(?i)', '(?)',
       'a**', 'a*?+', '(a{500}){3}', '{1,2}', '\\C', '\\pX', '\\p{Foo}', '[a', '[]', '[^]',
       '[a-\\d]', '[\\d-z]', '(?s)', '(?U)', '(?m)', '\\b+', '$+']
SUBJ_CHARS = ['a', 'b', 'c', 'x', 'A', 'B', 'C', ' ', '\n', '1', '2', '_', '-', '.', '*',
              'é', 'ü', 'É', '日', 'Σ', 'σ', '\t', 'é', 'k', 'K', 's', 'S', '\u212a', 'ſ', 'ß',
              'ẞ', 'å', 'Å', '\u212b', '\ufffd']


def gen(r, depth):
    k = r.random()
    if depth > 4 or k < 0.35:
        return r.choice(ODD) if r.random() < 0.05 else r.choice(ATOMS)
    if k < 0.55:
        return ''.join(gen(r, depth + 1) for _ in range(r.randint(2, 4)))
    if k < 0.70:
        return '|'.join(gen(r, depth + 1) for _ in range(r.randint(2, 3)))
    if k < 0.85:
        g = r.choice(['(%s)', '(?:%s)', '(?P<n%d>%%s)' % r.randint(0, 9), '(?:(?i)%s)', '(%s)'])
        return g % gen(r, depth + 1)
    op = r.choice(['*', '+', '?', '*?', '+?', '??', '{2}', '{1,3}', '{0,2}', '{2,}', '{0}',
                   '{1,2}?', '{3,}?'])
    a = gen(r, depth + 1)
    if a and a[-1] in '*+?}':
        a = '(?:%s)' % a
    return a + op


def subject(r):
    n = r.choice([0, 1, 3, 8, 20, 50])
    s = ''.join(r.choice(SUBJ_CHARS) for _ in range(n))
    b = s.encode()
    if r.random() < 0.1 and b:  # invalid UTF-8
        i = r.randrange(len(b) + 1)
        b = b[:i] + bytes([r.choice([0x80, 0xff, 0xc3, 0xe6])]) + b[i:]
    return b


def emit(out, flags, pat, subj):
    out.write('%s x%s x%s\n' % (flags, pat.hex(), subj.hex()))


def main():
    out = sys.stdout
    if sys.argv[1] == 'random':
        n, seed = int(sys.argv[2]), int(sys.argv[3])
        r = random.Random(seed)
        for _ in range(n):
            p = gen(r, 0).encode()
            flags = r.choice(['-'] * 8 + ['i', 'l'])
            for _ in range(3):
                emit(out, flags, p, subject(r))
    else:
        pats = []
        for line in open(sys.argv[2], encoding='utf-8', errors='surrogateescape'):
            line = line.rstrip('\n')
            if line.startswith('[') and '] ' in line:
                pats.append(line.split('] ', 1)[1].encode('utf-8', 'surrogateescape'))
        lines = [l.rstrip(b'\n') for l in open(sys.argv[3], 'rb')]
        r = random.Random(1)
        chunk = b'\n'.join(lines[:200])
        for p in pats:
            for l in r.sample(lines, min(40, len(lines))):
                emit(out, '-', p, l)
            emit(out, '-', p, chunk)


if __name__ == '__main__':
    main()
