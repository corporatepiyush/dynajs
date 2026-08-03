#!/usr/bin/env python3
"""Fail when a tests/test_*.js is run by nothing, anywhere.

A test nobody runs is not a test. Found five here, including test_textcodec.js
-- which covers exactly the bytes.encode/decode utf-8 defect that was instead
found by hand, because nothing executed it.

A target that enumerates what it covers goes stale silently, and its own
comment ("excluded on purpose, the docker stage runs them") is what stops
anyone re-checking. So this looks in every place that can run one: the
Makefile, docker/, .github/, tools/.

KNOWN_UNRUN is an ALLOW LIST WITH REASONS, not a dumping ground. A file listed
there must state why, and "diagnostic, cannot fail" is a legitimate reason --
adding an assertion-free script to a gate is adding decoration.
"""
import glob
import os
import sys

KNOWN_UNRUN = {
    'tests/test_atod_diff.js':
        'diagnostic: prints a total, has no assertion and cannot fail',
    'tests/test_prop_hash_order.js':
        'diagnostic: prints key order, has no assertion and cannot fail',
    'tests/test_worker_module.js':
        'launched AS a worker by os.Worker, not as a main script; run '
        'standalone it throws on onmessage. Needs a driver before it can be '
        'gated.',
}

RUNNERS = ['Makefile'] + sorted(
    glob.glob('docker/*') + glob.glob('.github/**/*', recursive=True) +
    glob.glob('tools/*'))


def main():
    blob = ''
    me = os.path.abspath(__file__)
    for r in RUNNERS:
        # This file lists test paths in KNOWN_UNRUN, so reading itself would
        # report every allowed entry as "something runs it now".
        if os.path.isfile(r) and os.path.abspath(r) != me:
            try:
                blob += open(r, encoding='utf-8', errors='ignore').read()
            except OSError:
                pass

    tests = sorted(glob.glob('tests/test_*.js'))
    if not tests:
        print('check-orphan-tests: no tests matched -- a probe that scans '
              'nothing is not a check', file=sys.stderr)
        return 2

    orphans, stale = [], []
    for t in tests:
        run = os.path.basename(t) in blob or t in blob
        if run and t in KNOWN_UNRUN:
            stale.append(t)
        elif not run and t not in KNOWN_UNRUN:
            orphans.append(t)

    for t in orphans:
        print(f'  {t}: run by nothing. Add it to a target, or to '
              f'KNOWN_UNRUN with a reason.')
    for t in stale:
        print(f'  {t}: listed in KNOWN_UNRUN but something runs it now -- '
              f'drop the entry.')

    n = len(orphans) + len(stale)
    if n:
        print(f'check-orphan-tests: {n} finding(s) across {len(tests)} tests')
        return 1
    print(f'check-orphan-tests: {len(tests)} tests, '
          f'{len(KNOWN_UNRUN)} allowed unrun with reasons')
    return 0


if __name__ == '__main__':
    sys.exit(main())
