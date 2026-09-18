#!/usr/bin/env python3
"""Regression test for bench/codegraph.py --defects.

Writes a fixture C file per defect class into a temp tree, runs the scanner
as a subprocess (the same way CI will), and asserts that every expected
check fires on its plant and that nothing fires on a clean file. Then pins
a baseline, plants one more defect, and asserts the gate reports it NEW.

Usage: python3 tools/test_codegraph_defects.py   (exit 0 = pass)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CG = os.path.join(REPO, "bench", "codegraph.py")

# (name, source, checks that MUST fire on it)
CASES = [
    ("c23_octal.c",
     "int f(void) { return 0o777; }\n", ["D01"]),
    ("banned.c",
     '#include <string.h>\n'
     'void f(char *d, const char *s) { strcpy(d, s); }\n', ["D02"]),
    ("realloc_self.c",
     '#include <stdlib.h>\n'
     'void f(void) { char *p = malloc(8); p = realloc(p, 16); '
     '(void)p; }\n', ["D03"]),
    ("ignored_errconv.c",
     'int JS_ToInt64(void *c, long long *o, int v);\n'
     'void f(void *c, long long *o, int v) { JS_ToInt64(c, o, v); }\n',
     ["D04"]),
    ("snprintf_accum.c",
     'int snprintf(char *, unsigned, const char *, ...);\n'
     'void f(char *b, unsigned n) { int o = 0; '
     'o += snprintf(b + o, n - o, "x"); }\n', ["D05"]),
    ("assign_in_cond.c",
     'int g(int x) { int y; if (x > 0) y = 1; else y = (x = 2); '
     'return y + x; }\n'
     'int h(int a, int b) { if (a = b) return 1; return 0; }\n',
     ["D06"]),
    ("strlen_repeat.c",
     'unsigned strlen(const char *);\n'
     'int f(const char *s) { return (int)(strlen(s) + strlen(s)); }\n',
     ["D08"]),
    ("memset_dce.c",
     '#include <string.h>\n#include <stdlib.h>\n'
     'void f(void) { char k[32]; memset(k, 0, sizeof k); '
     'free((void *)0); }\n', ["D09"]),
    ("sizeof_param.c",
     '#include <string.h>\n'
     'void f(char *buf) { memset(buf, 0, sizeof(buf)); }\n', ["D10"]),
    ("lock_balance.c",
     'int pthread_mutex_lock(void *);\n'
     'int pthread_mutex_unlock(void *);\n'
     'void f(void *m) { pthread_mutex_lock(m); }\n', ["D11"]),
    ("loop_le.c",
     'int f(int *a, int n) { int i, s = 0; '
     'for (i = 0; i <= n; i++) s += a[i]; return s; }\n', ["D30"]),
    ("memcpy_overlap.c",
     '#include <string.h>\n'
     'void f(char *b) { memcpy(b + 1, b, 4); }\n', ["D31"]),
    ("strlen_loop.c",
     '#include <string.h>\n'
     'int f(const char *s) { int i, n = 0; '
     'for (i = 0; i < (int)strlen(s); i++) n++; return n; }\n',
     ["D32"]),
    ("partial_io.c",
     'int read(int, void *, unsigned);\n'
     'void f(int fd, char *b) { read(fd, b, 10); }\n', ["D33"]),
    ("port_htons.c",
     'struct sockaddr_in { short sin_port; };\n'
     'void f(struct sockaddr_in *a) { a->sin_port = 8080; }\n',
     ["D35"]),
    ("sig_handler.c",
     '#include <stdlib.h>\n#include <stdio.h>\n'
     'void my_sig_handler(int sig) { (void)sig; '
     'char *p = malloc(4); printf("%p\\n", p); }\n', ["D37", "D28"]),
    ("simd_cast.c",
     'typedef long long __m128i __attribute__((vector_size(16)));\n'
     'void _mm_stream_si128(void *, __m128i);\n'
     'void f(char *dst, __m128i v) { '
     '_mm_stream_si128((__m128i *)dst, v); }\n',
     ["D38"]),
    ("simd_store_oob.c",
     'typedef float v4sf __attribute__((vector_size(16)));\n'
     'void _mm_storeu_ps(float *, v4sf);\n'
     'void f(v4sf v) { float small[2]; _mm_storeu_ps(small, v); }\n',
     ["D39"]),
    ("strncpy.c",
     '#include <string.h>\n'
     'void f(char *d, const char *s) { strncpy(d, s, 8); }\n',
     ["D40"]),
    ("sscanf.c",
     'int sscanf(const char *, const char *, ...);\n'
     'void f(const char *line, char *word) { '
     'sscanf(line, "%s", word); }\n', ["D41"]),
    ("strtok.c",
     'char *strtok(char *, const char *);\n'
     'void f(char *s) { strtok(s, " "); }\n', ["D43"]),
    ("double_cast.c",
     'double sqrt(double);\n'
     'int f(double x) { return (int)sqrt(x); }\n', ["D45"]),
    ("use_after_realloc.c",
     '#include <stdlib.h>\n'
     'int f(void) { char *p = malloc(4); char *q = realloc(p, 8); '
     'q[0] = 1; return p[0]; }\n', ["D23"]),
    ("double_free.c",
     '#include <stdlib.h>\n'
     'void f(void) { char *p = malloc(4); free(p); free(p); }\n',
     ["D24"]),
    ("free_stack.c",
     '#include <stdlib.h>\n'
     'void f(void) { char buf[8]; free(buf); }\n', ["D25"]),
    ("ret_local.c",
     'int *f(void) { int x = 1; return &x; }\n', ["D26"]),
    ("stack_escape.c",
     'void f(int **out) { int x = 4; *out = &x; }\n', ["D27"]),
    ("deref_null.c",
     '#include <stdlib.h>\n'
     'int f(unsigned n) { char *p = malloc(n); return p[0]; }\n',
     ["D28", "D29"]),
    ("alloc_overflow.c",
     '#include <stdlib.h>\n'
     'char *f(unsigned a, unsigned b) { return malloc(a * b); }\n',
     ["D21"]),
]

CLEAN = ("clean.c",
         '#include <stdlib.h>\n'
         'int f(unsigned n) {\n'
         '    char *p = malloc(n);\n'
         '    if (!p) return -1;\n'
         '    p[0] = 1;\n'
         '    free(p);\n'
         '    return 0;\n'
         '}\n')

DRIFT = ("drift.c",
         '#include <stdlib.h>\n'
         'void f(char *d, const char *s) { strcpy(d, s); }\n')


def run(args):
    p = subprocess.run([sys.executable, CG] + args,
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    tmp = tempfile.mkdtemp(prefix="cgdefects")
    try:
        for name, src, _ in CASES + [(CLEAN[0], CLEAN[1], [])]:
            with open(os.path.join(tmp, name), "w") as fh:
                fh.write(src)
        rc, out = run([tmp, "--defects", "--defects-all"])
        got = {}
        for line in out.splitlines():
            parts = line.split()
            if parts and parts[0] in ("error", "warn", "info", "NEW"):
                got.setdefault(parts[1].split("-")[0], 0)
                got[parts[1].split("-")[0]] += 1

        fails = []
        for name, _, want in CASES:
            for w in want:
                if got.get(w, 0) == 0:
                    fails.append("%s: expected %s to fire" % (name, w))
        if rc == 0:
            fails.append("exit code should be nonzero (errors planted)")

        # clean file must contribute nothing: rerun without the dirty files
        only = os.path.join(tmp, "only")
        os.makedirs(only)
        with open(os.path.join(only, CLEAN[0]), "w") as fh:
            fh.write(CLEAN[1])
        rc2, out2 = run([only, "--defects"])
        if rc2 != 0 or "0 findings" not in out2:
            fails.append("clean tree: rc=%d out=%r" % (rc2, out2[:80]))

        # baseline gate: pin, drift, expect NEW + exit 1
        bl = os.path.join(tmp, "base.json")
        run([tmp, "--defects", "--update-baseline", bl])
        with open(os.path.join(tmp, DRIFT[0]), "w") as fh:
            fh.write(DRIFT[1])
        rc3, out3 = run([tmp, "--defects", "--defects-baseline", bl])
        if rc3 != 1 or "NEW" not in out3:
            fails.append("gate: rc=%d (want 1), NEW in out=%r"
                         % (rc3, "NEW" in out3))

        for f in fails:
            print("FAIL " + f)
        n = sum(len(w) for _, _, w in CASES)
        print("defects selftest: %d/%d check expectations, gate %s"
              % (n - len([f for f in fails if "expected" in f]), n,
                 "OK" if not any("gate" in f for f in fails) else "BROKEN"))
        return 1 if fails else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
