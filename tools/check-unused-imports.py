#!/usr/bin/env python3
"""Fail when a test imports a name it never uses.

WHY THIS EXISTS. Wiring a shared corpus into a suite is two steps -- add the
import, then feed the data to something -- and only the first one is visible in
a diff. Eight of ten suites here had the import line and used nothing: the tree
compiled, every suite ran, the gate was green, and the adversarial data reached
no assertion at all. An unused import is invisible to a compiler (these are
scripts), to a linter (there is none in the gate), and to the suite's own pass
count, which is exactly why it survived a full session of review.

The sibling defect, found the same day: the same import pasted INSIDE a
generated child script written to a temp directory, where the relative
specifier does not resolve -- so the child died before it could bind a port and
six cases failed as "connection refused". That one is checked too, because it
reads identically in a diff and fails somewhere else entirely.

Exit 1 on any finding; exit 0 clean. Names are reported per file so the fix is
mechanical: either use the data or drop the line.
"""
import re
import sys
import glob
import os

IMPORT = re.compile(r'import\s*\{([^}]*)\}\s*from\s*[\'"]([^\'"]+)[\'"]', re.S)
# Comments contain code EXAMPLES -- a doc block explaining how multi-line
# imports are handled parses as an import of `\n  a` from "m". Reporting those
# makes the check cry wolf on every run, which is how a probe decays into a
# known quirk nobody reads. Strip comments before scanning, not after.
BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.S)
LINE_COMMENT = re.compile(r'(?m)^\s*//.*$')


def strip_comments(src):
    return LINE_COMMENT.sub('', BLOCK_COMMENT.sub('', src))
# write(`...`) / spawn(`...`) templates: a relative import inside one resolves
# against the GENERATED file's directory, not this one.
TEMPLATE = re.compile(r'`[^`]*`', re.S)


def named_bindings(clause):
    """`a, b as c` -> the identifiers actually bound in this scope (a, c)."""
    out = []
    for part in clause.split(','):
        part = part.strip()
        if not part:
            continue
        out.append(part.split(' as ')[-1].strip() if ' as ' in part else part)
    return out


def check(path):
    try:
        src = open(path, encoding='utf-8', errors='ignore').read()
    except OSError as e:
        return [f'{path}: cannot read: {e}']

    findings = []
    src = strip_comments(src)

    # 1. a relative import sitting inside a template literal
    for tmpl in TEMPLATE.findall(src):
        for m in IMPORT.finditer(tmpl):
            spec = m.group(2)
            if spec.startswith('.'):
                findings.append(
                    f'{path}: `import ... from "{spec}"` is inside a template '
                    f'literal. If that string is written to another directory, '
                    f'the specifier will not resolve and the child dies at '
                    f'startup.')

    # 2. named imports that nothing in the file uses
    body = src
    for m in IMPORT.finditer(src):
        body = body.replace(m.group(0), '')
    for m in IMPORT.finditer(src):
        for name in named_bindings(m.group(1)):
            if not re.search(r'\b' + re.escape(name) + r'\b', body):
                findings.append(
                    f'{path}: imports `{name}` from "{m.group(2)}" and never '
                    f'uses it -- the wiring is cosmetic.')
    return findings


def main(argv):
    roots = argv[1:] or ['tests']
    files = []
    for r in roots:
        files.extend(glob.glob(os.path.join(r, '**', '*.js'), recursive=True)
                     if os.path.isdir(r) else [r])
    files = sorted(set(files))
    if not files:
        print('check-unused-imports: no files matched -- a probe that scans '
              'nothing is not a check', file=sys.stderr)
        return 2

    findings = []
    for f in files:
        findings.extend(check(f))

    if findings:
        print(f'check-unused-imports: {len(findings)} finding(s) '
              f'across {len(files)} file(s)')
        for f in findings:
            print('  ' + f)
        return 1
    print(f'check-unused-imports: {len(files)} files clean')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
