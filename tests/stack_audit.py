#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""stack_audit.py OUTDIR BUDGET : worst-case stack below every patch entry point.

Frames come from gcc's .su files and calls from its assembly (.s):
 - a jsr/jbsr/bsr/jra/jbra/jmp to a symbol is a call (tail calls included);
 - a jsr through a6 is an operating-system call: not followed (its own use
   of the stack is the OS's, on top of the budget below);
 - a jsr through another register inside clip_rp_q/clip_rp reaches the render
   callbacks (CALLBACK: every *_cb whose address is taken); anywhere else it
   reaches a board hook (HOOK: every driver function whose address a driver
   object takes - the callbacks and PrismOps it installs). A board hook's own
   calls into a P96 .card are external code and not followed.
Extra edges can be listed in tests/stack_indirect.txt. Entry points are the
h_*, P_*, C_*, f_* functions (the patches); STUB_FRAME adds what stubs.S
pushes before calling them."""
import collections, glob, os, re, sys

out, budget = sys.argv[1], int(sys.argv[2])
STUB_FRAME = 15 * 4 + 4 + 4          # movem d0-d7/a0-a6, the Regs pointer, the return address
CALLBACK_INVOKERS = {'clip_rp_q', 'clip_rp'}

def clean(n):
    return re.sub(r'\.(part|constprop|isra|cold)\.\d+.*$', '', n).lstrip('_')

frame = {}
for su in glob.glob(os.path.join(out, '*.su')):
    for line in open(su):
        parts = line.rstrip('\n').split('\t')
        if len(parts) >= 2:
            name = clean(parts[0].rsplit(':', 1)[-1])
            frame[name] = max(frame.get(name, 0), int(parts[1]))

calls = collections.defaultdict(set)
taken = collections.defaultdict(set)       # object -> functions whose address it takes
for s in glob.glob(os.path.join(out, '*.s')):
    obj = os.path.basename(s)[:-2]
    cur = None
    for line in open(s, errors='replace'):
        if not line.strip() or line.lstrip().startswith(('.', '#', '|')):
            # directives (.globl, .long tables): a table of function pointers
            # is an address taken by this object
            for sym in re.findall(r'\.long\s+(_[A-Za-z_][\w.]*)', line):
                taken[obj].add(clean(sym))
            continue
        m = re.match(r'^(_?[A-Za-z_][\w.]*):', line)
        if m:
            if clean(m.group(1)) in frame:
                cur = clean(m.group(1))
            continue
        if not cur:
            continue
        ins = line.strip()
        op = ins.split()[0] if ins.split() else ''
        if op in ('jsr', 'jbsr') and re.match(r'^\w+\s+\(?a[0-5]\)?|^\w+\s+a[0-5]@', ins):
            calls[cur].add('CALLBACK' if cur in CALLBACK_INVOKERS else 'HOOK')
            continue
        if op in ('jsr', 'jbsr', 'bsr', 'jra', 'jbra', 'jmp', 'bra', 'jbsr.l') or op.startswith(('jbsr', 'bsr', 'jra')):
            for sym in re.findall(r'(?<![\w.])(_[A-Za-z_][\w.]*)', ins):
                c = clean(sym)
                if c in frame and c != cur:
                    calls[cur].add(c)
            continue
        # an address taken (pea, lea, move #_f): a callback or a hook installed.
        # A render callback runs below the function that hands it to clip_rp:
        # charge it there, through clip_rp's own frames (VIA node).
        for sym in re.findall(r'#?(?<![\w.])(_[A-Za-z_][\w.]*)', ins):
            c = clean(sym)
            if c in frame:
                taken[obj].add(c)
                if c.endswith('_cb'):
                    calls[cur].add('VIA:' + c)

callbacks = {f for o in taken for f in taken[o] if f.endswith('_cb')}
hooks = {f for o in taken if o.startswith('drv_') or o in ('rtg_ops',) for f in taken[o]}
frame['CALLBACK'] = frame['HOOK'] = 0
calls['CALLBACK'] = set()            # charged to the caller instead (VIA nodes)
calls['HOOK'] = hooks
via = frame.get('clip_rp', 0) + frame.get('clip_rp_q', 0)
for cb in callbacks:
    frame['VIA:' + cb] = via
    calls['VIA:' + cb] = {cb}

ind = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'stack_indirect.txt')
if os.path.exists(ind):
    for line in open(ind):
        line = line.split('#')[0].strip()
        if not line or ':' not in line:
            continue
        a, b = line.split(':', 1)
        calls[a.strip()].update(x for x in b.split() if x)

memo, recursion = {}, set()


def depth(f, stack=()):
    if f in stack:
        recursion.add(' -> '.join(stack[stack.index(f):] + (f,)))
        return 0, []
    if f in memo:
        return memo[f]
    best, path = 0, []
    for c in calls.get(f, ()):
        d, p = depth(c, stack + (f,))
        if d > best:
            best, path = d, p
    label = ('[%s]' % f.lower() if f in ('CALLBACK', 'HOOK') else
             '[clip_rp %d]' % frame[f] if f.startswith('VIA:') else '%s(%d)' % (f, frame.get(f, 0)))
    r = (frame.get(f, 0) + best, [label] + path)
    memo[f] = r
    return r


entries = sorted(f for f in frame if re.match(r'^(h|P|C|f)_[A-Za-z]', f))
rows = sorted(((depth(e)[0] + STUB_FRAME, e, depth(e)[1]) for e in entries), reverse=True)
over = [r for r in rows if r[0] > budget]
print('stack audit: %d patch entry points, %d render callbacks, %d board hooks; budget %d bytes '
      '(stub frame %d included)' % (len(rows), len(callbacks), len(hooks), budget, STUB_FRAME))
for d, e, p in rows[:int(os.environ.get('SHOW', '12'))]:
    print('  %5d  %s' % (d, ' > '.join(p)))
for r in sorted(recursion)[:8]:
    print('  recursion cut: ' + r)
if over:
    print('FAIL: %d entry points over budget' % len(over))
    sys.exit(1)
print('stack audit passed')
