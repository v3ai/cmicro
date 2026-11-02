#!/usr/bin/env python3
"""Compare the outputs of the C and Go sides of the differential test.

  cmp.py IN C_OUT GO_OUT [MAX_SHOWN]

A pattern Go rejects but cmicro accepts is a tolerated extension (counted,
not a failure); everything else must be identical.  Exit status 1 on any
real difference.
"""
import sys


def main():
    ins = open(sys.argv[1]).read().split('\n')
    c = open(sys.argv[2]).read().split('\n')
    g = open(sys.argv[3]).read().split('\n')
    shown = int(sys.argv[4]) if len(sys.argv) > 4 else 15
    if len(c) != len(g):
        print('output length differs: C %d lines, Go %d lines' % (len(c), len(g)))
        return 1
    bad = lenient = 0
    for line, a, b in zip(ins, c, g):
        if a == b:
            continue
        if b == 'E' and a != 'E':
            lenient += 1
            continue
        bad += 1
        if bad <= shown:
            f = line.split(' ')
            pat = bytes.fromhex(f[1][1:])
            subj = bytes.fromhex(f[2][1:])
            print('flags %s pattern %r subject %r' % (f[0], pat.decode('utf-8', 'replace'), subj))
            print('     C: %s' % a[:300])
            print('    Go: %s' % b[:300])
    print('%d cases, %d differ, %d accepted only by cmicro' % (len(ins) - 1, bad, lenient))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
