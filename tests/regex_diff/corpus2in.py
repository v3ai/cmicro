#!/usr/bin/env python3
"""Turn a regex_fuzz corpus into differential-test input, so that the odd
patterns libFuzzer discovers are also checked against Go:

  corpus2in.py CORPUS_DIR > in
  cdiff < in > c.out; godiff < in > go.out; cmp.py in c.out go.out

Input files: one flags byte, the pattern, NUL, the subject (see
tests/regex_fuzz.c).  Patterns Go cannot take (invalid UTF-8, NUL) are
skipped.
"""
import os
import sys


def main():
    d = sys.argv[1]
    for name in sorted(os.listdir(d)):
        data = open(os.path.join(d, name), 'rb').read()
        if not data:
            continue
        flags = data[0] & 3
        pat, _, subj = data[1:].partition(b'\0')
        try:
            pat.decode('utf-8')
        except UnicodeDecodeError:
            continue
        f = ('i' if flags & 1 else '') + ('l' if flags & 2 else '') or '-'
        sys.stdout.write('%s x%s x%s\n' % (f, pat.hex(), subj.hex()))


if __name__ == '__main__':
    main()
