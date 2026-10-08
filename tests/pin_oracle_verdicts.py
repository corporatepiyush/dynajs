#!/usr/bin/env python3
"""Pin the five oracle_* parser differentials (B1-13).

Each of these files accumulates a hash of every case's result and PRINTS it.
Nothing compared it to anything, so the only way the suite could fail was a
crash. This rewrites the trailing print into a pinned verdict: the case count,
the rejected/positions count and the hash are all compared, and a mismatch
prints FAIL lines and throws (nonzero exit).

Run from the repo root after a DELIBERATE behaviour change, reviewing the new
line before updating the pins.
"""
import re
import subprocess
import sys

# path -> (the accumulator variable the file already keeps, what it counts)
PINS = {
    "tests/oracle_asi.js": ("errs", "ASI"),
    "tests/oracle_expr_precedence.js": ("errs", "expression precedence"),
    "tests/oracle_line_col.js": ("withPos", "line/column"),
    "tests/oracle_parse_ident.js": ("errs", "identifier lexing"),
    "tests/oracle_parse_string.js": ("errs", "string/template parsing"),
}

TMPL = '''// ============================================================================
// PINNED VERDICT (B1-13). This file used to accumulate a hash and PRINT it.
// Nothing compared it against anything, so the only way it could ever fail was
// a crash: six core-stage suites read as parser coverage while asserting
// nothing about the parser. The pin below is the differential this suite was
// written for, made load-bearing inside a one-binary gate -- same generator,
// same case list, and any change in {desc} changes the hash and turns the
// stage red.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). To move a pin, run this file's sibling tool
// tests/pin_oracle_verdicts.py and REVIEW the new line: a pin that moves
// without a reviewed behaviour change is a deleted test.
const PIN_CASES = {cases};
const PIN_COUNT = {count};
const PIN_HASH = "{hsh}";
const gotCount = {var};
const gotHash = acc.toString(16);
let pinBad = 0;
if (cases !== PIN_CASES) {{
    print("FAIL: {name} fed " + cases + " cases, the pinned generator feeds " + PIN_CASES);
    pinBad++;
}}
if (gotCount !== PIN_COUNT) {{
    print("FAIL: {name} " + gotCount + " {label}, pinned " + PIN_COUNT);
    pinBad++;
}}
if (gotHash !== PIN_HASH) {{
    print("FAIL: {name} hash " + gotHash + ", pinned " + PIN_HASH + " -- parser behaviour changed");
    pinBad++;
}}
if (pinBad === 0)
    print("{name}: OK (" + cases + " cases, " + gotCount + " {label}, hash " + gotHash + " matches the pin)");
if (pinBad !== 0)
    throw new Error("{name}: " + pinBad + " pinned verdict(s) changed");
'''


def observed(path):
    out = subprocess.run(["./dynajs", path], capture_output=True, text=True)
    line = [l for l in out.stdout.splitlines() if l.startswith(path.split("/")[-1][:-3] + ":")]
    if not line:
        raise SystemExit("cannot read the verdict line from %s: %s" % (path, out.stdout[-400:]))
    m = re.match(r"\S+: (\d+) cases, (\d+) \S+, hash ([0-9a-f]+)", line[-1])
    if not m:
        raise SystemExit("unparsable verdict line: %r" % line[-1])
    return int(m.group(1)), int(m.group(2)), m.group(3)


def main():
    for path, (var, desc) in sorted(PINS.items()):
        name = path.split("/")[-1][:-3]
        src = open(path, encoding="utf-8").read()
        pat = re.compile(
            r'print\("%s: " \+ cases \+ " cases, " \+ %s \+ " \w+, hash " \+\s*\n\s*acc\.toString\(16\)\);'
            % (re.escape(name), re.escape(var)))
        m = pat.search(src)
        if m:
            cases, count, hsh = observed(path)
            tail = TMPL.format(cases=cases, count=count, hsh=hsh, var=var,
                               name=name, desc=desc, label="positions" if var == "withPos" else "rejected")
            src = src[:m.start()] + tail + src[m.end():]
            open(path, "w", encoding="utf-8").write(src)
            print("pinned %-34s %d cases, %d, %s" % (path, cases, count, hsh))
        else:
            print("already pinned (or the tail moved): %s" % path)


if __name__ == "__main__":
    sys.exit(main())