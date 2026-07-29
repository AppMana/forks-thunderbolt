#!/usr/bin/env python3
"""Derive the driver's lock acquisition graph from source.

Complements lockdep. lockdep only sees orderings the KUnit suite actually
exercises; this walks every function in kernel/*.c, records the lock/unlock
events in source order, and propagates acquisitions through the intra-module
call graph. It answers two questions the tests cannot:

  * does any function acquire a lock it may already hold (self deadlock),
  * which ordered lock pairs exist at all, so lockdep coverage can be judged.

This is a syntactic approximation. It does not resolve function pointers, does
not understand conditional acquisition, and treats a lock expression by the
base variable name. Over-approximating is the point: it produces a superset of
real orderings, and anything it does NOT report is not reachable by a direct
named call.

Usage:
  tools/lock-graph.py [--root DIR] [--check-recursive] [--pairs] [--holders LOCK]
"""

import argparse
import os
import re
import sys
from collections import defaultdict

ACQUIRE = re.compile(
    r'\b(mutex_lock_interruptible|mutex_lock_nested|mutex_lock|mutex_trylock|'
    r'spin_lock_irqsave|spin_lock_irq|spin_lock_bh|spin_lock|'
    r'raw_spin_lock_irqsave|raw_spin_lock|'
    r'down_read|down_write|read_lock|write_lock)\s*\(\s*&?([^,)]+)')
RELEASE = re.compile(
    r'\b(mutex_unlock|spin_unlock_irqrestore|spin_unlock_irq|spin_unlock_bh|'
    r'spin_unlock|raw_spin_unlock_irqrestore|raw_spin_unlock|'
    r'up_read|up_write|read_unlock|write_unlock)\s*\(\s*&?([^,)]+)')
CALL = re.compile(r'\b([a-z_][a-z0-9_]*)\s*\(')
FUNC_DEF = re.compile(
    r'^(?:static\s+|inline\s+|const\s+|struct\s+\w+\s*\*?\s*|'
    r'void|int|bool|u\d+|s\d+|size_t|ssize_t|unsigned|long|char|'
    r'enum\s+\w+|__\w+)[\w\s\*]*?\b([a-z_][a-z0-9_]*)\s*\([^;]*$')

# base variable name -> canonical owning object, so tqp->lock and qp->lock and
# tbv_qp_of(x)->lock all name the same lock class.
VAR_CLASS = {
    'tqp': 'qp', 'qp': 'qp', 'sqp': 'qp', 'dqp': 'qp',
    'tcq': 'cq', 'cq': 'cq', 'scq': 'cq', 'rcq': 'cq',
    'path': 'path', 'tpath': 'path', 'p': 'path',
    'state': 'state', 's': 'state', 'tstate': 'state',
    'link': 'link', 'tlink': 'link',
    'peer': 'peer', 'tpeer': 'peer',
    'session': 'session',
    'identity': 'identity',
    'owner': 'link_owner',
    'send': 'send_ctx', 'sctx': 'send_ctx',
    'read': 'read_ctx', 'rctx': 'read_ctx',
    'dev': 'ibdev', 'tdev': 'ibdev', 'ibdev': 'ibdev',
    'net': 'tbnet', 'tbnet': 'tbnet',
}


def canon(expr):
    """Canonicalise a lock expression to <object>.<member>."""
    e = expr.strip().rstrip(')').strip()
    e = re.sub(r'\s+', '', e)
    if '->' not in e and '.' not in e:
        return 'global.' + e
    parts = re.split(r'->|\.', e)
    member = parts[-1]
    # walk backwards for the nearest recognisable owner
    for tok in reversed(parts[:-1]):
        tok = re.sub(r'\[.*\]', '', tok)
        if tok in VAR_CLASS:
            return VAR_CLASS[tok] + '.' + member
        if tok in ('owner', 'state', 'identity', 'session', 'peer', 'link',
                   'path', 'qp', 'cq'):
            return tok + '.' + member
    base = re.sub(r'\[.*\]', '', parts[0])
    base = re.sub(r'^.*\(', '', base)
    return VAR_CLASS.get(base, base) + '.' + member


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return text


def parse_file(path, scoped=True):
    """Return {func: {'acq': [(lock, held_frozenset)], 'calls': [(fn, held)]}}."""
    src = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    lines = src.split('\n')
    out = {}
    func = None
    depth = 0
    held = []
    pending = None
    for raw in lines:
        line = raw
        if func is None:
            if depth == 0:
                cand = FUNC_DEF.match(line.strip())
                if cand and not line.strip().endswith(';'):
                    pending = cand.group(1)
            if pending and '{' in line:
                func = pending
                out.setdefault(func, {'acq': [], 'calls': []})
                held = []
                depth = line.count('{') - line.count('}')
                pending = None
                continue
            if line.strip().endswith(';'):
                pending = None
            continue

        names = lambda: frozenset(l for l, _d in held)
        for m in ACQUIRE.finditer(line):
            lock = canon(m.group(2))
            out[func]['acq'].append((lock, names()))
            held.append((lock, depth))
        for m in RELEASE.finditer(line):
            lock = canon(m.group(2))
            # Only drop the lock when the unlock sits at or above the brace
            # depth that took it. An unlock inside a deeper error branch
            # (mutex_unlock(); return;) must not clear the lock for the code
            # that follows in the enclosing scope, or nesting is under-reported.
            for i in range(len(held) - 1, -1, -1):
                if held[i][0] == lock and (not scoped or depth <= held[i][1]):
                    del held[i]
                    break
        for m in CALL.finditer(line):
            name = m.group(1)
            if ACQUIRE.match(name + '(') or RELEASE.match(name + '('):
                continue
            out[func]['calls'].append((name, names()))

        depth += line.count('{') - line.count('}')
        if depth <= 0:
            func = None
            depth = 0
            held = []
    return out


def build(root, scoped=True):
    funcs = {}
    for dirpath, _dirs, files in os.walk(root):
        if os.path.basename(dirpath) in ('.git',):
            continue
        for f in sorted(files):
            if not f.endswith('.c'):
                continue
            if f.endswith('.mod.c'):
                continue
            for name, info in parse_file(os.path.join(dirpath, f), scoped).items():
                cur = funcs.setdefault(name, {'acq': [], 'calls': []})
                cur['acq'] += info['acq']
                cur['calls'] += info['calls']
    return funcs


def transitive_acquires(funcs):
    """func -> set of locks it may acquire, directly or via named callees."""
    direct = {f: {l for l, _ in i['acq']} for f, i in funcs.items()}
    acq = {f: set(s) for f, s in direct.items()}
    changed = True
    rounds = 0
    while changed and rounds < 100:
        changed = False
        rounds += 1
        for f, info in funcs.items():
            for callee, _held in info['calls']:
                if callee in acq and not acq[callee] <= acq[f]:
                    acq[f] |= acq[callee]
                    changed = True
    return acq


def analyse(funcs, acq):
    """(ordered pairs, recursive acquisitions) implied by funcs/acq."""
    pairs = defaultdict(set)      # (outer, inner) -> {evidence}
    recursive = defaultdict(set)  # lock -> {evidence}
    for f, info in funcs.items():
        # direct nesting inside one function body
        for lock, held in info['acq']:
            for h in held:
                if h == lock:
                    recursive[lock].add('%s: re-acquires while held' % f)
                else:
                    pairs[(h, lock)].add(f)
        # nesting through a call made while holding
        for callee, held in info['calls']:
            if callee not in acq:
                continue
            for inner in acq[callee]:
                for h in held:
                    if h == inner:
                        recursive[inner].add(
                            '%s holds %s and calls %s which may take %s'
                            % (f, h, callee, inner))
                    else:
                        pairs[(h, inner)].add('%s -> %s' % (f, callee))
    return pairs, recursive


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'kernel'))
    ap.add_argument('--pairs', action='store_true',
                    help='print every ordered lock pair (outer -> inner)')
    ap.add_argument('--check-recursive', action='store_true',
                    help='report a lock possibly acquired while already held')
    ap.add_argument('--holders', metavar='LOCK',
                    help='print every function that reaches LOCK while holding it')
    ap.add_argument('--coverage', action='store_true',
                    help='which locks the KUnit suite under kernel/tests can reach')
    args = ap.parse_args()

    root = os.path.normpath(args.root)
    funcs = build(root)
    acq = transitive_acquires(funcs)

    if args.coverage:
        tests = build(os.path.join(root, 'tests'))
        reached = set()
        for f, info in tests.items():
            for callee, _h in info['calls']:
                reached |= acq.get(callee, set())
        allocks = {l for s in acq.values() for l in s}
        print('\n## locks reachable from the KUnit suite')
        for l in sorted(reached):
            print('    %s' % l)
        print('\n## locks NOT reachable from the KUnit suite')
        for l in sorted(allocks - reached):
            print('    %s' % l)
        return 0

    pairs, recursive = analyse(funcs, acq)

    # Second opinion. Neither release model is sound on its own: the linear
    # model drops a lock on any unlock, so an unlock in an error branch hides
    # the nesting that follows it (under-reports). The scoped model keeps a
    # lock until an unlock at its own brace depth, so an if/else that unlocks
    # in both arms keeps it held to end of function (over-reports). A finding
    # both models agree on is worth reading; a scoped-only finding is a lead.
    lfuncs = build(root, scoped=False)
    lacq = transitive_acquires(lfuncs)
    lpairs, lrecursive = analyse(lfuncs, lacq)

    print('# functions parsed: %d' % len(funcs))
    print('# locks seen: %d' % len({l for s in acq.values() for l in s}))

    if args.check_recursive or not (args.pairs or args.holders):
        print('\n## possible recursive acquisition')
        if not recursive:
            print('none')
        for lock in sorted(recursive):
            print('%s:' % lock)
            for ev in sorted(recursive[lock]):
                tag = 'BOTH MODELS' if ev in lrecursive.get(lock, ()) \
                    else 'scoped only'
                print('    [%s] %s' % (tag, ev))

    if args.pairs or not (args.check_recursive or args.holders):
        print('\n## ordered lock pairs (outer -> inner)')
        for (a, b) in sorted(pairs):
            tag = 'BOTH' if (a, b) in lpairs else 'scpd'
            print('[%s] %-24s -> %-24s  (%d site(s), e.g. %s)'
                  % (tag, a, b, len(pairs[(a, b)]), sorted(pairs[(a, b)])[0]))
        print('\n## inversions (both orders observed)')
        inv = sorted({tuple(sorted((a, b))) for (a, b) in pairs
                      if (b, a) in pairs})
        if not inv:
            print('none')
        for a, b in inv:
            print('%s <-> %s' % (a, b))

    if args.holders:
        want = args.holders
        print('\n## functions that may take %s while holding it' % want)
        hits = [e for ev in recursive.get(want, ()) for e in (ev,)]
        if not hits:
            print('none')
        for h in sorted(hits):
            print('    %s' % h)
        print('\n## direct acquirers of %s' % want)
        for f, info in sorted(funcs.items()):
            for lock, held in info['acq']:
                if lock == want:
                    print('    %s (holding: %s)'
                          % (f, ', '.join(sorted(held)) or 'nothing'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
