#!/usr/bin/env python3
"""Owner directive: engine C sources (src/**/*.c, *.h, *.inc.c and the root repl.c) carry ZERO comments.

Scans with a small C lexer (string and character literals are skipped, so "http://x" and '/' are not comments) and
prints every file:line that opens a // or /* comment.  Before scanning it runs a self-test that plants a comment of
each kind (and the literal look-alikes) in a scratch file and proves the lexer reports exactly the planted ones: a
checker that cannot fail proves nothing.

    tools/check-no-comments.py [ROOT]      exit 0 when clean, 1 with a list when not, 2 when the self-test fails
"""
import os
import sys
import tempfile


def comment_lines(text):
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in "\"'":
            q = c
            i += 1
            while i < n and text[i] != q and text[i] != "\n":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif text.startswith("//", i) or text.startswith("/*", i):
            out.append(line)
            if text.startswith("//", i):
                while i < n and text[i] != "\n":
                    i += 1
            else:
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
                line += text.count("\n", i, j)
                i = j
        else:
            i += 1
    return out


def selftest():
    sample = (
        'const char* u = "http://example/*not*/";\n'
        "char s = '/';\n"
        'const char* e = "\\"//still a string\\"";\n'
        "int a; // planted line comment\n"
        "/* planted\n   block */ int b;\n"
        "int c = 1 / 2;\n"
    )
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "plant.c")
        with open(p, "w") as f:
            f.write(sample)
        got = comment_lines(open(p, encoding="latin-1").read())
    return got == [4, 5]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    if not selftest():
        print("FAIL: check-no-comments self-test: the lexer missed a planted comment or flagged a literal")
        return 2
    files = []
    for base, _, names in os.walk(os.path.join(root, "src")):
        for name in names:
            if name.endswith((".c", ".h")):
                files.append(os.path.join(base, name))
    repl = os.path.join(root, "repl.c")
    if os.path.exists(repl):
        files.append(repl)
    bad = []
    for path in sorted(files):
        for ln in comment_lines(open(path, encoding="latin-1").read()):
            bad.append("%s:%d" % (os.path.relpath(path, root), ln))
    if bad:
        print("FAIL: %d comment(s) in engine sources (owner directive: zero comments):" % len(bad))
        for b in bad[:50]:
            print("  " + b)
        if len(bad) > 50:
            print("  ... and %d more" % (len(bad) - 50))
        return 1
    print("no-comments: %d engine source files clean (self-test planted 2, caught 2, ignored 3 literals)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
