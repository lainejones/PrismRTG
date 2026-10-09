# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""compare.py [SRCDIR] : PrismRTG's code against the public sources fetched by fetch.sh.
Run from the repository root:  python3 tools/provenance/compare.py out/provenance-src
(THRESH=a,b,c sets the thresholds below; SHOW=1 prints the matching text).
For every PrismRTG source file and every reference file it reports
  A. the longest run of identical tokens (verbatim copying)
  B. the longest run with identifiers and literals normalised (copying with renaming)
  C. the longest run of identical numeric constants in order (tables / register programs -
     what survives translating assembly to C)
and prints every pair whose runs pass the thresholds, with the matching text from both sides."""
import os, re, sys, glob, collections

OURS = os.getcwd().replace(os.sep, '/')
REF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OURS, 'out', 'provenance-src')
THRESH_A, THRESH_B, THRESH_C = [int(x) for x in os.environ.get('THRESH', '40,70,10').split(',')]

C_KW = set('''auto break case char const continue default do double else enum extern float for goto if inline
int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile
while UBYTE UWORD ULONG BYTE WORD LONG BOOL APTR STRPTR TRUE FALSE NULL'''.split())

tok_re = re.compile(r'0[xX][0-9a-fA-F]+|\$[0-9a-fA-F]+|\d+|[A-Za-z_]\w*|==|!=|<=|>=|<<|>>|->|&&|\|\||\+\+|--|[{}()\[\];,.+\-*/%&|^!~<>=?:#]')


def strip_comments(t, asm=False):
    if asm:
        return re.sub(r';[^\n]*|\*[^\n]*$', '', t, flags=re.M)
    # a comment becomes as many newlines as it had, so line numbers stay true
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n') or ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    t = re.sub(r'"(\\.|[^"\\])*"', '"S"', t)
    return t


def tokens(path):
    raw = open(path, encoding='latin-1').read()
    asm = path.lower().endswith(('.asm', '.s', '.i')) and not path.endswith('.S')
    t = strip_comments(raw, asm)
    out = []
    for m in tok_re.finditer(t):
        out.append((m.group(0), t.count('\n', 0, m.start()) + 1))
    return out, raw, asm


def num(tok):
    if tok.startswith(('0x', '0X')):
        return int(tok, 16)
    if tok.startswith('$'):
        return int(tok[1:], 16)
    if tok.isdigit():
        return int(tok)
    return None


def norm(tok):
    if tok in C_KW:
        return tok
    if re.match(r'[A-Za-z_]', tok):
        return 'I'
    if num(tok) is not None:
        return 'N'
    return tok


def longest_common_run(a, b, k):
    """longest run of equal items in a and b (seeded by k-grams); returns (len, ia, ib)"""
    if len(a) < k or len(b) < k:
        return 0, 0, 0
    idx = collections.defaultdict(list)
    for i in range(len(b) - k + 1):
        idx[tuple(b[i:i + k])].append(i)
    best = (0, 0, 0)
    i = 0
    while i <= len(a) - k:
        hits = idx.get(tuple(a[i:i + k]))
        adv = 1
        if hits:
            for j in hits[:50]:
                n = k
                while i + n < len(a) and j + n < len(b) and a[i + n] == b[j + n]:
                    n += 1
                if n > best[0]:
                    best = (n, i, j)
        i += adv
    return best


ours = [p for p in glob.glob(OURS + '/src/*.[chS]') + glob.glob(OURS + '/tools/*.[chS]') +
        glob.glob(OURS + '/tests/*.c') if '/p96sdk/' not in p.replace('\\', '/')]
refs = [p for p in glob.glob(REF + '/**/*', recursive=True)
        if os.path.isfile(p) and p.lower().endswith(('.c', '.h', '.cpp', '.asm', '.i', '.s'))
        and '/.git/' not in p.replace('\\', '/')]
print('%d PrismRTG files against %d reference files' % (len(ours), len(refs)))

ref_data = {}
for r in refs:
    try:
        tk, raw, asm = tokens(r)
    except Exception:
        continue
    ref_data[r] = (tk, raw, asm, [x[0] for x in tk], [norm(x[0]) for x in tk],
                   [n for n in (num(x[0]) for x in tk) if n is not None and n > 15])

findings = []
for o in ours:
    otk, oraw, _ = tokens(o)
    oa = [x[0] for x in otk]
    ob = [norm(x) for x in oa]
    on = [n for n in (num(x) for x in oa) if n is not None and n > 15]
    olines = [x[1] for x in otk]
    onum_lines = [x[1] for x in otk if num(x[0]) is not None and num(x[0]) > 15]
    for r, (rtk, rraw, rasm, ra, rb, rn) in ref_data.items():
        rlines = [x[1] for x in rtk]
        rnum_lines = [x[1] for x in rtk if num(x[0]) is not None and num(x[0]) > 15]
        if not rasm:
            la, ia, ja = longest_common_run(oa, ra, 12)
            if la >= THRESH_A:
                findings.append(('A verbatim', la, o, olines[ia], olines[ia + la - 1], r, rlines[ja], rlines[ja + la - 1]))
            lb, ib, jb = longest_common_run(ob, rb, 20)
            if lb >= THRESH_B:
                findings.append(('B renamed', lb, o, olines[ib], olines[ib + lb - 1], r, rlines[jb], rlines[jb + lb - 1]))
        lc, ic, jc = longest_common_run(on, rn, 5)
        if lc >= THRESH_C:
            findings.append(('C constants', lc, o, onum_lines[ic], onum_lines[ic + lc - 1], r, rnum_lines[jc], rnum_lines[jc + lc - 1]))

findings.sort(key=lambda f: (f[0], -f[1]))
rel = lambda p: p.replace('\\', '/').replace(OURS + '/', '').replace(REF.replace('\\', '/') + '/', '')
print('%d findings over the thresholds (A>=%d, B>=%d tokens, C>=%d constants)' % (len(findings), THRESH_A, THRESH_B, THRESH_C))
for kind, n, o, ol0, ol1, r, rl0, rl1 in findings:
    print('\n[%s] %d  %s:%d-%d  <->  %s:%d-%d' % (kind, n, rel(o), ol0, ol1, rel(r), rl0, rl1))
    if os.environ.get('SHOW'):
        ol = open(o, encoding='latin-1').read().split('\n')[ol0 - 1:min(ol1, ol0 + 12)]
        rl = open(r, encoding='latin-1').read().split('\n')[rl0 - 1:min(rl1, rl0 + 12)]
        print('   ours: ' + '\n   ours: '.join(ol))
        print('   ref : ' + '\n   ref : '.join(rl))
