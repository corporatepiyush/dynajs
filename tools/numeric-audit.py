#!/usr/bin/env python3
"""numeric-audit.py -- scan C sources for integer/float/string misuse patterns.

High-signal detectors, each mapping to a real bug class this project has met
or that CERT/CWE name:

  alloc-mul      malloc/calloc/realloc size argument multiplies two variables;
                 needs an explicit bound check nearby or it is CVE-shaped
  narrow-len     (int)/(uint32_t)/(int32_t) cast applied to an expression that
                 mentions len/count/size/n -- silent truncation at 2^31
  f2i-cast       (int)(...) around a floating expression: UB when out of int
                 range, silent truncation otherwise
  float-eq       == / != between float-ish operands (not vs 0/NULL literal)
  nan-filter     !(x > t) / !(x < t) style negated compares on doubles: NaN
                 passes BOTH arms of a split filter and writes OOB (met here)
  char-index     table[<char var>] indexing without an (unsigned char) cast:
                 signed-char UB / negative index for bytes >= 0x80

Usage: python3 tools/numeric-audit.py [root]   (default src/)
Exit is always 0: this is a review list, not a gate.
"""
import os, re, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "src"

ALLOC = re.compile(r'\b(malloc|calloc|realloc)\s*\(')
MUL_IN_CALL = re.compile(r'[\w.\)\]]+\s*\*\s*[\w.\(]+')
NARROW = re.compile(r'\((?:uint32_t|int32_t|int|unsigned(?:\s+int)?)\)\s*\(?[^)]*\b(?:len|count|size|total|nrows|rowcount)\w*', re.I)
F2I = re.compile(r'\(int(?:32_t)?\)\s*\(?\s*(?:[a-z_]*\((?:float|double)|[a-z0-9_]*(?:floor|ceil|round|sqrt|pow|log|exp)\b|[a-z_]*(?:_f|_d)\b)')
FLOAT_EQ = re.compile(r'[a-zA-Z_][a-zA-Z0-9_]*(?:f|d)\b\s*(==|!=)\s*(?!0\b|NULL\b)[0-9]')
NAN_FILTER = re.compile(r'!\s*\(\s*[A-Za-z_]\w*\s*[<>]=?\s*[A-Za-z0-9_.]+\s*\)')
CHAR_DECL = re.compile(r'\b(?:signed\s+)?char\s+([a-z]\w*)\s*(?:=|;|\[)', re.I)

def scan(path, text):
    hits = []
    lines = text.split("\n")
    # char vars declared in this file (rough): index-without-cast detector
    charvars = set()
    for i, l in enumerate(lines):
        for m in CHAR_DECL.finditer(l):
            # exclude unsigned char declarations
            if re.search(r'\bunsigned\s+char\b', l):
                continue
            charvars.add(m.group(1))
    for i, l in enumerate(lines, 1):
        t = l.strip()
        if t.startswith(("*", "/*", "//", "#")) or not t:
            continue
        if ALLOC.search(l) and MUL_IN_CALL.search(l):
            # look 2 lines ahead/behind for a bound check keyword
            ctx = "\n".join(lines[max(0, i-3):i+2])
            if not re.search(r'MAX|LIMIT|cap\b|overflow|SIZE_MAX|UINT32_MAX|too large|exceed', ctx, re.I):
                hits.append((i, "alloc-mul", t[:110]))
        m = NARROW.search(l)
        if m:
            hits.append((i, "narrow-len", t[:110]))
        if F2I.search(l):
            hits.append((i, "f2i-cast", t[:110]))
        if FLOAT_EQ.search(l) and "const" not in l:
            hits.append((i, "float-eq", t[:110]))
        if NAN_FILTER.search(l) and re.search(r'>|<', l):
            hits.append((i, "nan-filter", t[:110]))
        for cv in charvars:
            if re.search(r'\[\s*' + cv + r'\s*\]', l) and "(unsigned char)" not in l \
               and "(uint8_t)" not in l and "unsigned" not in l:
                hits.append((i, "char-index", t[:110]))
                break
    return hits

def main():
    total = {}
    for root, _, files in os.walk(ROOT):
        for f in sorted(files):
            if not f.endswith((".c", ".h")):
                continue
            p = os.path.join(root, f)
            try:
                text = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for (ln, kind, snip) in scan(p, text):
                total.setdefault(kind, []).append(f"{p}:{ln}: {snip}")
    grand = 0
    for kind in sorted(total):
        rows = total[kind]
        grand += len(rows)
        print(f"\n=== {kind}: {len(rows)} ===")
        for r in rows[:40]:
            print("  " + r)
        if len(rows) > 40:
            print(f"  ... and {len(rows)-40} more")
    print(f"\ntotal findings: {grand} (review list, not a gate)")

if __name__ == "__main__":
    main()
