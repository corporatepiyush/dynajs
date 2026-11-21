// tools/js/test_defects.js (the port of tools/test_defects.py, deleted): the selftest for tools/js/defects.js. Writes one fixture per
// defect class into a temp tree, runs the scanner as a subprocess, and asserts every expected
// check fires, a clean tree reports 0 findings, and a planted drift is reported NEW against a
// pinned baseline; a module-only core lib missing from mod_core_libs is D19, and a D47 near miss stays
// silent. The fixtures were generated verbatim from test_defects.py's CASES table. Also: header vs per-rule table
// severity sums, the tools/defects-baseline.tsv suppressions (hidden, excess, stale, malformed), every noise stage
// (STAGE_CASES: its noise shape dropped, the real bug beside it kept), and the D18 include-guard exemption.
// Usage: dynajs --std tools/js/test_defects.js   (exit 0 = pass; DYNAJS_BIN overrides the binary)
import * as std from "std";
import * as os from "os";
import { join, abspath, dirname } from "./pyshim.js";

const HERE = dirname(abspath(scriptArgs[0]));
const REPO = dirname(dirname(HERE));
const CG = join(HERE, "defects.js");
const BIN = std.getenv("DYNAJS_BIN") || join(REPO, "dynajs");

const CASES = [
    ["sweep_hdr.h", "int AddSweep(void (*f)(void *), void *o);\nvoid RemoveSweep(void (*f)(void *), void *o);\nstatic inline void hold_sweep(void *o) { (void)o; }\nstatic inline void hold_on(void *o) { AddSweep(hold_sweep, o); }\nstatic inline void hold_off(void *o) { RemoveSweep(hold_sweep, o); }\n", ["D46"]],
    ["c23_octal.c", "int f(void) { return 0o777; }\n", ["D01"]],
    ["promise_settle.c", "typedef struct C C; typedef long V;\nint JS_PromiseSettle(C *c, V p, V v, int r);\nvoid f(C *c, V p) { JS_PromiseSettle(c, p, p, 0); }\nvoid g(C *c, V p, int k) { if (k)\n        (void)JS_PromiseSettle(c, p, p, 1); }\n", ["D51"]],
    ["banned.c", "#include <string.h>\nvoid f(char *d, const char *s) { strcpy(d, s); }\n", ["D02"]],
    ["realloc_self.c", "#include <stdlib.h>\nvoid f(void) { char *p = malloc(8); p = realloc(p, 16); (void)p; }\n", ["D03"]],
    ["ignored_errconv.c", "int JS_ToInt64(void *c, long long *o, int v);\nvoid f(void *c, long long *o, int v) { JS_ToInt64(c, o, v); }\n", ["D04"]],
    ["snprintf_accum.c", "int snprintf(char *, unsigned, const char *, ...);\nvoid f(char *b, unsigned n) { int o = 0; o += snprintf(b + o, n - o, \"x\"); }\n", ["D05"]],
    ["assign_in_cond.c", "int g(int x) { int y; if (x > 0) y = 1; else y = (x = 2); return y + x; }\nint h(int a, int b) { if (a = b) return 1; return 0; }\n", ["D06"]],
    ["strlen_repeat.c", "unsigned strlen(const char *);\nint f(const char *s) { return (int)(strlen(s) + strlen(s)); }\n", ["D08"]],
    ["memset_dce.c", "#include <string.h>\n#include <stdlib.h>\nvoid f(void) { char k[32]; memset(k, 0, sizeof k); free((void *)0); }\n", ["D09"]],
    ["sizeof_param.c", "#include <string.h>\nvoid f(char *buf) { memset(buf, 0, sizeof(buf)); }\n", ["D10"]],
    ["lock_balance.c", "int pthread_mutex_lock(void *);\nint pthread_mutex_unlock(void *);\nvoid f(void *m) { pthread_mutex_lock(m); }\n", ["D11"]],
    ["loop_le.c", "int f(int *a, int n) { int i, s = 0; for (i = 0; i <= n; i++) s += a[i]; return s; }\n", ["D30"]],
    ["memcpy_overlap.c", "#include <string.h>\nvoid f(char *b) { memcpy(b + 1, b, 4); }\n", ["D31"]],
    ["strlen_loop.c", "#include <string.h>\nint f(const char *s) { int i, n = 0; for (i = 0; i < (int)strlen(s); i++) n++; return n; }\n", ["D32"]],
    ["partial_io.c", "int read(int, void *, unsigned);\nvoid f(int fd, char *b) { read(fd, b, 10); }\n", ["D33"]],
    ["port_htons.c", "struct sockaddr_in { short sin_port; };\nvoid f(struct sockaddr_in *a) { a->sin_port = 8080; }\n", ["D35"]],
    ["sig_handler.c", "#include <stdlib.h>\n#include <stdio.h>\nvoid my_sig_handler(int sig) { (void)sig; char *p = malloc(4); printf(\"%p\\n\", p); }\n", ["D37", "D28"]],
    ["simd_cast.c", "typedef long long __m128i __attribute__((vector_size(16)));\nvoid _mm_stream_si128(void *, __m128i);\nvoid f(char *dst, __m128i v) { _mm_stream_si128((__m128i *)dst, v); }\n", ["D38"]],
    ["simd_store_oob.c", "typedef float v4sf __attribute__((vector_size(16)));\nvoid _mm_storeu_ps(float *, v4sf);\nvoid f(v4sf v) { float small[2]; _mm_storeu_ps(small, v); }\n", ["D39"]],
    ["strncpy.c", "#include <string.h>\nvoid f(char *d, const char *s) { strncpy(d, s, 8); }\n", ["D40"]],
    ["sscanf.c", "int sscanf(const char *, const char *, ...);\nvoid f(const char *line, char *word) { sscanf(line, \"%s\", word); }\n", ["D41"]],
    ["strtok.c", "char *strtok(char *, const char *);\nvoid f(char *s) { strtok(s, \" \"); }\n", ["D43"]],
    ["double_cast.c", "double sqrt(double);\nint f(double x) { return (int)sqrt(x); }\n", ["D45"]],
    ["use_after_realloc.c", "#include <stdlib.h>\nint f(void) { char *p = malloc(4); char *q = realloc(p, 8); q[0] = 1; return p[0]; }\n", ["D23"]],
    ["double_free.c", "#include <stdlib.h>\nvoid f(void) { char *p = malloc(4); free(p); free(p); }\n", ["D24"]],
    ["free_stack.c", "#include <stdlib.h>\nvoid f(void) { char buf[8]; free(buf); }\n", ["D25"]],
    ["ret_local.c", "int *f(void) { int x = 1; return &x; }\n", ["D26"]],
    ["stack_escape.c", "void f(int **out) { int x = 4; *out = &x; }\n", ["D27"]],
    ["deref_null.c", "#include <stdlib.h>\nint f(unsigned n) { char *p = malloc(n); return p[0]; }\n", ["D28", "D29"]],
    ["outparam_leak.c", "#include <stdlib.h>\nint rd(const int *v, unsigned n, unsigned **out) {\n    unsigned i;\n    *out = (unsigned *)malloc(n * sizeof(unsigned) + 1);\n    if (!*out)\n        return -1;\n    for (i = 0; i < n; i++) {\n        if (v[i] < 0)\n            return -1;\n        (*out)[i] = (unsigned)v[i];\n    }\n    return 0;\n}\n", ["D47"]],
    ["alloc_overflow.c", "#include <stdlib.h>\nchar *f(unsigned a, unsigned b) { return malloc(a * b); }\n", ["D21"]],
];
const NEAR_MISS_D51 = ["settle_ok.c", "typedef struct C C; typedef long V;\nint JS_PromiseSettle(C *c, V p, V v, int r);\nint f(C *c, V p) { if (JS_PromiseSettle(c, p, p, 0) < 0) return -1; return JS_PromiseSettle(c, p, p, 1); }\nint g(C *c, V p) { int rc = JS_PromiseSettle(c, p, p, 0); return rc || (JS_PromiseSettle(c, p, p, 1) == 0); }\n"];
const CLEAN = ["clean.c", "#include <stdlib.h>\nint f(unsigned n) {\n    char *p = malloc(n);\n    if (!p) return -1;\n    p[0] = 1;\n    free(p);\n    return 0;\n}\n"];
const NEAR_MISS_D47 = ["outparam_ok.c", "#include <stdlib.h>\nint rd(const int *v, unsigned n, unsigned **out) {\n    unsigned i;\n    *out = (unsigned *)malloc(n * sizeof(unsigned) + 1);\n    if (!*out)\n        return -1;\n    for (i = 0; i < n; i++) {\n        if (v[i] < 0) {\n            free(*out);\n            *out = NULL;\n            return -1;\n        }\n        (*out)[i] = (unsigned)v[i];\n    }\n    return 0;\n}\nint pick(int typed, unsigned n, double **out) {\n    if (typed) {\n        *out = (double *)malloc(n * sizeof(double) + 8);\n        if (!*out)\n            return -1;\n        return 0;\n    }\n    if (n == 0)\n        return -1;\n    return 1;\n}\n"];
const D50H = "typedef struct C C; typedef long V;\nV JS_GetException(C *c);\nvoid JS_FreeValue(C *c, V v);\nint JS_IsException(V v);\nint JS_IsString(V v);\nV JS_Call(C *c, V f, V t, int n, V *a);\nV JS_Throw(C *c, V e);\nV JS_ThrowTypeError(C *c, const char *m);\nV JS_NewObject(C *c);\nconst char *JS_ToCString(C *c, V v);\nint JS_HasUncatchableException(C *c);\nvoid g(void);\n";
const D50_CALL_DROP = "void f(C *c, V fn) { V r = JS_Call(c, fn, fn, 0, 0); if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c)); }\n";

const STAGE_CASES = [
    ["D50", "own-throw",
        D50H + "void f(C *c) { V e = JS_ThrowTypeError(c, \"x\"); (void)e; JS_FreeValue(c, JS_GetException(c)); }\nvoid h(C *c, V v) { if (!JS_IsString(v)) { JS_ThrowTypeError(c, \"y\"); JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + D50_CALL_DROP],
    ["D50", "allocation-only",
        D50H + "void f(C *c) { V o = JS_NewObject(c); if (JS_IsException(o)) { g(); JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + D50_CALL_DROP],
    ["D50", "known-string",
        D50H + "void f(C *c, V v) { if (JS_IsString(v)) { const char *s = JS_ToCString(c, v); if (!s) JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + "void f(C *c, V v, V w) { if (JS_IsString(w)) { const char *s = JS_ToCString(c, v); if (!s) JS_FreeValue(c, JS_GetException(c)); } }\n"],
    ["D24", "reassigned-via-addr",
        "#include <stdlib.h>\nint get(char **o);\nvoid f(void) { char *p = 0; get(&p); free(p); get(&p); free(p); }\n",
        "#include <stdlib.h>\nint get(char **o);\nvoid f(void) { char *p = 0; get(&p); free(p); free(p); }\n"],
    ["D24", "fd-reopened-by-dup2",
        "int close(int);\nint dup2(int, int);\nvoid f(int a, int fd) { close(fd); dup2(a, fd); close(fd); }\n",
        "int close(int);\nint dup2(int, int);\nvoid f(int a, int fd) { close(fd); dup2(fd, a); close(fd); }\n"],
    ["D27", "static-storage",
        "void f(const int **o) { static const int z = 0; *o = &z; }\n",
        "void f(const int **o) { const int z = 0; *o = &z; }\n"],
    ["D27", "param-rebind",
        "int g(const int *p);\nint f(const int *o) { int d = 1; if (!o) o = &d; return g(o); }\n",
        "struct S { int *p; };\nvoid f(struct S *o) { int d = 1; o->p = &d; }\n"],
    ["D27", "declarator-init",
        "int g(int **ip);\nint f(void) { int x = 0, *ip = &x; return g(&ip); }\n",
        "int *gp;\nvoid f(void) { int x = 0; gp = &x; }\n"],
    ["D27", "member-of-local",
        "struct E { const int *val; };\nint use(struct E *e);\nint f(void) { int v = 1; struct E e[2]; e[0].val = &v; return use(e); }\n",
        "struct E { const int *val; };\nvoid f(struct E *e) { int v = 1; e[0].val = &v; }\n"],
    ["D27", "restored-before-exit",
        "struct V { int *errs; };\nint chk(struct V *v);\nint f(struct V *v) { int *saved = v->errs; int s = 0; v->errs = &s; int r = chk(v); v->errs = saved; return r; }\n",
        "struct V { int *errs; };\nint chk(struct V *v);\nint f(struct V *v) { int *saved = v->errs; int s = 0; v->errs = &s; if (chk(v)) return 1; v->errs = saved; return 0; }\n"],
    ["D28", "sizeof-operand",
        "#include <stdlib.h>\nint f(unsigned n) { int *p = malloc(sizeof(p[0]) * n); if (!p) return -1; p[0] = 1; free(p); return 0; }\n",
        "#include <stdlib.h>\nint f(unsigned n) { int *p = malloc(sizeof(p[0]) * n); p[0] = 1; free(p); return 0; }\n"],
    ["D28", "assign-in-condition",
        "#include <stdlib.h>\nchar *f(unsigned n) { char *p; if (n && (p = malloc(n))) { p[0] = 1; return p; } return 0; }\n",
        "#include <stdlib.h>\nchar *f(unsigned n) { char *p; if (n) { p = malloc(n); p[0] = 1; return p; } return 0; }\n"],
    ["D28", "member-lvalue",
        "#include <stdlib.h>\nstruct S { int *iv; int n; };\nint f(struct S *s, struct S *iv) { s->iv = calloc(iv->n, sizeof *s->iv); if (!s->iv) return -1; s->iv[0] = 1; return 0; }\n",
        "#include <stdlib.h>\nstruct S { int *iv; int n; };\nint f(struct S *s, struct S *iv) { s->iv = calloc(iv->n, 4); s->iv[0] = 1; return 0; }\n"],
    ["D28", "balanced-nullcheck",
        "#include <stdlib.h>\nint likely(int);\nint f(int a, unsigned n) { char *p = malloc(n); if (likely(a) && NULL == p) return -1; p[0] = 1; free(p); return 0; }\n",
        "#include <stdlib.h>\nint likely(int);\nint f(int a, unsigned n) { char *p = malloc(n); if (likely(a)) p[0] = 1; if (NULL == p) return -1; free(p); return 0; }\n"],
    ["D31", "distinct-field",
        "#include <string.h>\nstruct S { char a[8]; char b[8]; };\nvoid f(struct S *s) { memcpy(s->a, s->b, 8); }\nstruct V { char *p; };\nvoid g(struct V *c, int k) { memcpy(c[k].p, c[k + 1].p, 8); }\n",
        "#include <string.h>\nstruct S { char a[8]; char b[8]; };\nvoid f(struct S *s) { memcpy(s->a + 1, s->a, 4); }\n"],
    ["D31", "distinct-call",
        "#include <string.h>\nchar *at(char *b, int i);\nvoid f(char *b, int i, int j) { memcpy(at(b, i), at(b, j), 4); }\n",
        "#include <string.h>\nchar *at(char *b, int i);\nvoid f(char *b, int i) { memcpy(at(b, i), at(b, i) + 1, 4); }\n"],
    ["D31", "disjoint-span",
        "#include <string.h>\nvoid f(char *p, int j, int w, int e, int s) { memcpy(p + (size_t)j * w, p + (size_t)(j - 1) * w, w); memcpy(p + w, p, w); memcpy(p + e, p + s, e - s); }\n",
        "#include <string.h>\nvoid f(char *p, int j, int w) { memcpy(p + j * w, p + (j - 1) * w, 2 * w); }\n"],
    ["D31", "overlap-guard",
        "#include <string.h>\nvoid f(char *b, unsigned len, unsigned off, unsigned n) { if (off >= n) { memcpy(b + len, b + len - off, n); } }\nvoid g(char *r, unsigned i, unsigned j, unsigned w) { if (i == j) return; memcpy(r + i * w, r + j * w, w); }\n",
        "#include <string.h>\nvoid f(char *b, unsigned len, unsigned off, unsigned n) { if (off > 0) { memcpy(b + len, b + len - off, n); } }\nvoid g(char *r, unsigned i, unsigned j, unsigned w) { if (i != j) memcpy(r + i * w, r + j * w, w); }\n"],
    ["D21", "constant-size",
        "#include <stdlib.h>\n#include <stdint.h>\n#define UI_W 512\nvoid *f(void) { return malloc(4096 * 24); }\nvoid *g(void) { return calloc((size_t)UI_W * 128, 1); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(size_t n) { return malloc(n * 24); }\n"],
    ["D21", "bounded-operand",
        "#include <stdlib.h>\n#include <stdint.h>\nint dyn_size_mul(size_t a, size_t b, size_t *o);\nvoid *f(size_t n) { if (n > 1024) return 0; return malloc(n * 8); }\nvoid *g(size_t a, size_t b) { if (b && a > SIZE_MAX / b) return 0; return malloc(a * b); }\nvoid *h(size_t n) { if (n > 2048) n = 2048; return malloc(3 * n + 1); }\nvoid *k(size_t a, size_t b) { size_t t; if (dyn_size_mul(a, b, &t)) return 0; return malloc(a * b); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(size_t n) { if (n < 16) return 0; return malloc(n * 8); }\n"],
    ["D21", "constant-assigned",
        "#include <stdlib.h>\n#include <stdint.h>\n#define CAP 4096\nstruct U { size_t k; };\nvoid *f(struct U *u) { size_t n = 64; u->k = CAP; return malloc(u->k * n); }\nvoid *g(size_t vn) { size_t pn = vn > CAP ? CAP : vn; return malloc(4 * pn + 1); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\n#define CAP 4096\nvoid *f(const char *s) { size_t n = 0; while (s[n]) n++; return malloc(n * 8); }\n"],
    ["D21", "widened-32bit",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(uint32_t n, uint32_t m) { return malloc((size_t)n * m); }\nvoid *g(int span) { return calloc((size_t)span * 2 + 1, 4); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *g(uint32_t n, uint32_t m) { return malloc(n * m); }\n"],
    ["D30", "inclusive-bound",
        "struct T { unsigned mask; int *s; };\nint f(struct T *t) { unsigned i; int c = 0; for (i = 0; i <= t->mask; i++) c += t->s[i]; return c; }\nint g(int *a) { int i, c = 0; for (i = 0; i <= CHARCODE_MAX; i++) c += a[i]; return c; }\n",
        "struct T { unsigned len; int *s; };\nint f(struct T *t) { unsigned i; int c = 0; for (i = 0; i <= t->len; i++) c += t->s[i]; return c; }\n"],
    ["D30", "sentinel-test",
        "int f(const char *s, int n) { int i, k = 0; for (i = 0; i <= n; i++) { if (i == n || s[i] == '.') k++; } return k; }\n",
        "int f(const char *s, int n) { int i, k = 0; for (i = 0; i <= n; i++) { if (s[i] == '.') k++; } return k; }\n"],
    ["D30", "window-difference",
        "int f(const char *h, unsigned hl, unsigned nl) { unsigned i; if (hl < nl) return -1; for (i = 0; i <= hl - nl; i++) if (h[i] == 'x') return (int)i; return -1; }\n",
        "int f(const char *h, unsigned hl) { unsigned i; for (i = 0; i <= hl; i++) if (h[i] == 'x') return (int)i; return -1; }\n"],
    ["D30", "child-array",
        "struct N { int n; struct N *kid[9]; };\nvoid g(struct N *);\nint *cbt_cnt(struct N *x);\nvoid f(struct N *x) { int i; for (i = 0; i <= x->n; i++) g(x->kid[i]); }\nint h(struct N *x) { int i, s = 0; for (i = 0; i <= x->n; i++) s += cbt_cnt(x)[i]; return s; }\n",
        "struct N { int n; struct N *key[9]; struct N *kid[9]; };\nvoid g(struct N *);\nvoid f(struct N *x) { int i; for (i = 0; i <= x->n; i++) { g(x->kid[i]); g(x->key[i]); } }\n"],
    ["D30", "literal-in-decl",
        "int f(void) { char s[8]; int k, c = 0; for (k = 0; k <= 4; k++) { s[k] = 0; c += s[k]; } return c; }\n",
        "int f(void) { char s[4]; int k, c = 0; for (k = 0; k <= 4; k++) { s[k] = 0; c += s[k]; } return c; }\n"],
    ["D30", "sized-plus-one",
        "#include <stdlib.h>\nint f(int n) { int i, *a = malloc((n + 1) * sizeof *a); if (!a) return 0; for (i = 0; i <= n; i++) a[i] = i; free(a); return 1; }\n",
        "#include <stdlib.h>\nint f(int n) { int i, *a = malloc(n * sizeof *a); if (!a) return 0; for (i = 0; i <= n; i++) a[i] = i; free(a); return 1; }\n"],
    ["D04", "known-number",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (JS_IsNumber(v)) { JS_ToFloat64(c, &d, v); } return d; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (!JS_IsNumber(v)) { JS_ToFloat64(c, &d, v); } return d; }\n"],
    ["D04", "closure-data",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nV mk(C *c, unsigned g) { V d[2]; d[1] = JS_NewUint32(c, g); return d[0]; }\nint f(C *c, V *data) { unsigned g = 0; JS_ToUint32(c, &g, data[1]); return (int)g; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nint f(C *c, V *data) { unsigned g = 0; JS_ToUint32(c, &g, data[1]); return (int)g; }\n"],
    ["D04", "numeric-origin",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nint f(C *c, V v) { int i = 0; v = JS_ToNumericFree(c, v); JS_ToInt32(c, &i, v); return i; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nint f(C *c, V o) { int i = 0; V v; v = JS_GetPropertyStr(c, o, \"x\"); JS_ToInt32(c, &i, v); return i; }\n"],
    ["D23", "same-lvalue",
        "#include <stdlib.h>\nstruct R { int *k; };\nvoid f(struct R *r, unsigned n) { r->k = realloc(r->k, n); r->k[0] = 1; }\n",
        "#include <stdlib.h>\nstruct R { int *k; int *j; };\nvoid f(struct R *r, unsigned n) { r->j = realloc(r->k, n); r->k[0] = 1; }\n"],
    ["D23", "member-path",
        "#include <stdlib.h>\nstruct B { char *p; int oom; };\nint f(struct B *b, unsigned n) { char *np = realloc(b->p, n); b->oom = !np; free(np); return b->oom; }\n",
        "#include <stdlib.h>\nstruct B { char *p; int oom; };\nint f(struct B *b, unsigned n) { char *np = realloc(b->p, n); b->oom = b->p[0]; free(np); return b->oom; }\n"],
    ["D23", "reassigned-before-use",
        "#include <stdlib.h>\nstruct B { char *p; };\nvoid f(struct B *b, unsigned n) { char *np = realloc(b->p, n); if (!np) return; b->p = np; b->p[0] = 1; }\n",
        "#include <stdlib.h>\nstruct B { char *p; };\nvoid f(struct B *b, unsigned n) { char *np = realloc(b->p, n); if (!np) return; b->p[0] = 1; b->p = np; }\n"],
    ["D23", "failure-branch",
        "#include <stdlib.h>\nstruct B { char *p; };\nvoid f(struct B *b, unsigned n) { char *np = realloc(b->p, n); if (!np) { free(b->p); b->p = 0; return; } b->p = np; }\n",
        "#include <stdlib.h>\nstruct B { char *p; };\nvoid f(struct B *b, unsigned n) { char *np = realloc(b->p, n); if (!np) { return; } b->p[0] = 1; b->p = np; }\n"],
    ["D22", "declared-element-type",
        "#include <stdlib.h>\n#include <string.h>\ntypedef struct { int a; } P;\nvoid f(P *d, const P *s, int n) { memcpy(d, s, n * sizeof(P)); }\nvoid g(double *cent, const double *x, int c, int cols) { memcpy(cent + c * cols, &x[c], cols * sizeof(double)); }\n",
        "#include <stdlib.h>\n#include <string.h>\ntypedef struct { int a; } P;\nvoid f(char *d, const char *s, int n) { memcpy(d, s, n * sizeof(P)); }\n"],
    ["D22", "allocated-as-type",
        "#include <stdlib.h>\n#include <string.h>\nvoid *f(const void *s, int n) { void *d = malloc(n * sizeof(short)); if (d) memcpy(d, s, n * sizeof(short)); return d; }\n",
        "#include <stdlib.h>\n#include <string.h>\nvoid *f(const void *s, int n) { void *d = malloc(n * sizeof(int)); if (d) memcpy(d, s, n * sizeof(short)); return d; }\n"],
    ["D22", "past-embedded-header",
        "#include <stdlib.h>\n#include <string.h>\ntypedef struct { int h; } H;\ntypedef struct { H header; int x, y; } S;\nvoid f(S *a, S *b) { memcpy(&a->header + 1, &b->header + 1, sizeof(S) - sizeof(H)); }\n",
        "#include <stdlib.h>\n#include <string.h>\ntypedef struct { int h; } H;\ntypedef struct { H header; int x, y; } S;\ntypedef struct { long q[9]; } Q;\nvoid f(S *a, S *b) { memcpy(&a->header + 1, &b->header + 1, sizeof(Q)); }\n"],
    ["D03", "allocator-shim",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nvoid *f(void *ptr, size_t n) { ptr = realloc(ptr, n); return ptr; }\n",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nvoid *f(void *q, size_t n) { void *ptr = q; ptr = realloc(ptr, n); return ptr; }\n"],
    ["D03", "fails-hard",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nstruct S { char *b; };\nvoid f(struct S *s, size_t n) { s->b = realloc(s->b, n); if (!s->b) fz_fail(\"realloc\"); }\nvoid g(char *a, size_t n) { char *p = a; p = realloc(p, n); if (!p) abort(); }\n",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nstruct S { char *b; };\nvoid f(struct S *s, size_t n) { s->b = realloc(s->b, n); if (!s->b) return; }\n"],
    ["D48", "known-string",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { int r = 0; if (JS_IsString(v)) { const char *s = JS_ToCString(c, v); if (s) { r = g(s); JS_FreeCString(c, s); } } return r; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v, V w) { int r = 0; if (JS_IsString(w)) { const char *s = JS_ToCString(c, v); if (s) { r = g(s); JS_FreeCString(c, s); } } return r; }\n"],
    ["D48", "exception-settled",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nV JS_Throw(C *c, V e);\nvoid js_drop_catchable_exception(C *c);\nV stream_take_exc(C *c);\nvoid fwd(C *c, V e);\nint f(C *c, V *a, int n) { int i, r = 0; for (i = 0; i < n; i++) { const char *s = JS_ToCString(c, a[i]); if (s) { r += g(s); JS_FreeCString(c, s); } if (JS_HasException(c)) return -1; } return r; }\nint h(C *c, V v) { const char *s = JS_ToCString(c, v); if (s) { g(s); JS_FreeCString(c, s); } JS_Throw(c, v); return -1; }\nvoid k(C *c, V v) { const char *s = JS_ToCString(c, v); if (!s) js_drop_catchable_exception(c); else { g(s); JS_FreeCString(c, s); } }\nvoid m(C *c, V v) { const char *s = JS_ToCString(c, v); if (s) { g(s); JS_FreeCString(c, s); } else fwd(c, stream_take_exc(c)); }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V *a, int n, int k) { int i, r = 0; if (k) { for (i = 0; i < n; i++) { if (n) { const char *s = JS_ToCString(c, a[i]); if (s) { r += g(s); JS_FreeCString(c, s); } } } } else if (JS_HasException(c)) return -1; return r; }\n"],
    ["D48", "later-exit-test",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V a, V b) { const char *x = JS_ToCString(c, a), *y = JS_ToCString(c, b); int r = x && y && g(x) == g(y); if (!x || !y) r = -1; JS_FreeCString(c, x); JS_FreeCString(c, y); return r; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V a, V b) { const char *x = JS_ToCString(c, a), *y = JS_ToCString(c, b); int r = x && y && g(x) == g(y); if (!x || !y) g(\"none\"); JS_FreeCString(c, x); JS_FreeCString(c, y); return r; }\n"],
    ["D49", "absent-gate-then-convert",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { int n = 0; V v = JS_GetPropertyStr(c, o, \"n\"); if (!JS_IsUndefined(v) && !JS_IsNull(v)) { if (JS_ToInt32(c, &n, v)) { JS_FreeValue(c, v); return -1; } } JS_FreeValue(c, v); return n; }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { int n = 0; V v = JS_GetPropertyStr(c, o, \"n\"); if (!JS_IsUndefined(v)) n = JS_ToBool(c, v); JS_FreeValue(c, v); return n; }\n"],
    ["D49", "tobool-sign-tested",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { V v = JS_GetPropertyStr(c, o, \"b\"); int b = JS_ToBool(c, v); JS_FreeValue(c, v); if (b < 0) return -1; return b; }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { V v = JS_GetPropertyStr(c, o, \"b\"); int b = JS_ToBool(c, v); JS_FreeValue(c, v); return b; }\n"],
    ["D49", "helper-then-has-exception",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { V v = JS_GetPropertyStr(c, o, \"d\"); int d = dtype(c, v); JS_FreeValue(c, v); if (d == -1 && JS_HasException(c)) return -1; return d; }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o) { V v = JS_GetPropertyStr(c, o, \"d\"); int d = dtype(c, v); JS_FreeValue(c, v); if (d < 0) return 0; return d; }\n"],
];
const RULE_CASES = [
    ["D50", "a call inside the guard condition is the failing call",
        D50H + "void g2(C *c, V v);\nvoid f(C *c, V fn) { if (JS_IsException(JS_NewObject(c))) { g2(c, fn); JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + "void f(C *c, V fn) { if (JS_IsException(JS_Call(c, fn, fn, 0, 0))) JS_FreeValue(c, JS_GetException(c)); }\n"],
    ["D50", "an uncatchable test before the take exempts it; a rethrow without one is a swallow",
        D50H + "void f(C *c, V fn) { V r = JS_Call(c, fn, fn, 0, 0); if (JS_IsException(r)) { if (JS_HasUncatchableException(c)) return; JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + "void f(C *c, V fn) { V r = JS_Call(c, fn, fn, 0, 0); if (JS_IsException(r)) { V e = JS_GetException(c); JS_Throw(c, e); } }\n"],
    ["D50", "the failing call comes from the guard, so an own throw after a JS call is still a swallow",
        D50H + "void f(C *c, V fn) { V o = JS_NewObject(c); if (JS_IsException(o)) { JS_Call(c, fn, fn, 0, 0); JS_FreeValue(c, JS_GetException(c)); } }\n",
        D50H + "void f(C *c, V fn) { V r = JS_Call(c, fn, fn, 0, 0); if (JS_IsException(r)) { JS_ThrowTypeError(c, \"wrapped\"); JS_FreeValue(c, JS_GetException(c)); } }\n"],
    ["D49", "a check before any use, a returned result and an absent gate that re-reads then checks are clean; a type test that throws its own error is not",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint w(C *c, V o, const char *k, const char *alt) { V v = JS_GetPropertyStr(c, o, k); if (alt && JS_IsUndefined(v)) v = JS_GetPropertyStr(c, o, alt); if (JS_IsException(v)) return -1; JS_FreeValue(c, v); return 0; }\nV g(C *c, V o) { if (o) return JS_GetPropertyStr(c, o, \"x\"); return o; }\nint m(C *c, V o) {\n    V v;\n#define G(n) do { \\\n        v = JS_GetPropertyStr(c, o, n); \\\n        if (JS_IsException(v)) return -1; \\\n    } while (0)\n    G(\"a\");\n    return JS_IsString(v);\n}\nint k(C *c, V o) { V v = JS_GetPropertyStr(c, o, \"x\"); if (!JS_IsString(v) || JS_IsException(v)) { JS_FreeValue(c, v); return -1; } JS_FreeValue(c, v); return 0; }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint s(C *c, V v, unsigned i) { V e = JS_GetPropertyUint32(c, v, i); if (!JS_IsNumber(e)) { JS_FreeValue(c, e); JS_ThrowTypeError(c, \"shape\"); return -1; } return 0; }\n"],
    ["D49", "an out-parameter deref is tracked to its exception check",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint h(C *c, V o, V *out) { *out = JS_GetPropertyStr(c, o, \"x\"); return JS_IsException(*out) ? -1 : 0; }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint h(C *c, V o, V *out) { *out = JS_GetPropertyStr(c, o, \"x\"); return JS_IsString(*out); }\n"],
    ["D49", "a getter result handed to a setter stores JS_EXCEPTION as a value",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o, V d) { V v = JS_GetPropertyStr(c, o, \"k\"); if (JS_IsException(v)) return -1; return JS_SetPropertyStr(c, d, \"k\", v); }\n",
        "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nint f(C *c, V o, V d) { return JS_SetPropertyStr(c, d, \"k\", JS_GetPropertyStr(c, o, \"k\")); }\n"],
    ["D48", "a new TypeError replaces the pending exception instead of settling it",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nV JS_Throw(C *c, V e);\nint h(C *c, V v, V e) { const char *s = JS_ToCString(c, v); int ok = s && g(s); if (s) JS_FreeCString(c, s); if (!ok) { JS_Throw(c, e); return -1; } return 0; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nV JS_ThrowTypeError(C *c, const char *m);\nint h(C *c, V v) { const char *s = JS_ToCString(c, v); int ok = s && g(s); if (s) JS_FreeCString(c, s); if (!ok) { JS_ThrowTypeError(c, \"bad\"); return -1; } return 0; }\n"],
    ["D48", "a settle check outside the two enclosing blocks does not count",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V *a, int n) { int i, r = 0; for (i = 0; i < n; i++) { if (n) { const char *s = JS_ToCString(c, a[i]); if (s) { r += g(s); JS_FreeCString(c, s); } } if (JS_HasException(c)) return -1; } return r; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V *a, int n, int k) { int i, r = 0; if (k) { for (i = 0; i < n; i++) { if (n) { const char *s = JS_ToCString(c, a[i]); if (s) { r += g(s); JS_FreeCString(c, s); } } } } else if (JS_HasException(c)) return -1; return r; }\n"],
    ["D48", "a later test only counts when it is the negated result with an exit",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint h(C *c, V a) { const char *x = JS_ToCString(c, a); int r = x && g(x); if (!x) return -1; JS_FreeCString(c, x); return r; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint h(C *c, V a) { const char *x = JS_ToCString(c, a); int r = x && g(x); if (x) { JS_FreeCString(c, x); return r; } g(\"none\"); return 0; }\n"],
    ["D48", "a noreturn failure call is an exit and returning the result hands it to the caller",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nvoid abort(void);\nvoid fz_fail(const char *m);\nint f(C *c, V v) { const char *s = JS_ToCString(c, v); if (!s) abort(); g(s); JS_FreeCString(c, s); return 0; }\nint h(C *c, V v) { const char *s = JS_ToCString(c, v); if (!s) fz_fail(\"x\"); g(s); JS_FreeCString(c, s); return 0; }\nconst char *k(C *c, V v) { const char *s; s = JS_ToCString(c, v); return s; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nvoid note(const char *m);\nint f(C *c, V v) { const char *s = JS_ToCString(c, v); if (!s) note(\"x\"); g(s); JS_FreeCString(c, s); return 0; }\n"],
    ["D48", "a NULL tolerated by if (s) carries the pending exception",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { const char *s = JS_ToCString(c, v); if (!s) return -1; int r = g(s); JS_FreeCString(c, s); return r; }\nint h(C *c, V v) { const char *s = JS_ToCString(c, v); if (s == NULL) return -1; int r = g(s); JS_FreeCString(c, s); return r; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { int r = 0; const char *s = JS_ToCString(c, v); if (s) { r = g(s); JS_FreeCString(c, s); } return r; }\n"],
    ["D48", "member and deref lvalues are tracked to their NULL test",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nstruct O { const char *cstr; };\nint f(C *c, V v, struct O *o, const char **p) { o->cstr = JS_ToCString(c, v); if (!o->cstr) return -1; *p = JS_ToCString(c, v); if (!*p) return -1; return 0; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nstruct O { const char *cstr; };\nint f(C *c, V v, struct O *o) { o->cstr = JS_ToCString(c, v); return o->cstr ? 0 : 1; }\n"],
    ["D48", "an error flag is an exit, a bare continue is not",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V *a, int n) { int i, bad = 0; for (i = 0; i < n; i++) { const char *s = JS_ToCString(c, a[i]); if (!s) { bad = 1; break; } g(s); JS_FreeCString(c, s); } return bad ? -1 : 0; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V *a, int n) { int i; for (i = 0; i < n; i++) { const char *s = JS_ToCString(c, a[i]); if (!s) continue; g(s); JS_FreeCString(c, s); } return 0; }\n"],
    ["D48", "a negated string guard counts only when it exits",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { const char *s; int eq; if (!JS_IsString(v)) return 0; s = JS_ToCString(c, v); eq = s && g(s); JS_FreeCString(c, s); return eq; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { const char *s; int eq; if (!JS_IsString(v)) g(\"x\"); s = JS_ToCString(c, v); eq = s && g(s); JS_FreeCString(c, s); return eq; }\n"],
    ["D48", "returning the result as a negative status hands it to the caller",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v, const char **s) { *s = JS_ToCString(c, v); return *s ? 0 : -1; }\nstruct O { const char *p; };\nint h(C *c, V v, struct O *o) { o->p = JS_ToCString(c, v); return o->p ? 1 : -1; }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v, const char **s) { *s = JS_ToCString(c, v); return *s ? 0 : 1; }\n"],
    ["D48", "a result used before any NULL test",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { const char *s; if (!(s = JS_ToCString(c, v))) return -1; return g(s); }\n",
        "typedef struct C C; typedef long V;\nconst char *JS_ToCString(C *c, V v);\nvoid JS_FreeCString(C *c, const char *s);\nint JS_IsString(V v);\nint JS_HasException(C *c);\nint g(const char *s);\nint f(C *c, V v) { const char *s = JS_ToCString(c, v); int r = g(s); JS_FreeCString(c, s); return r; }\n"],
    ["D22", "declared type comes from the operand's own declarator, not an earlier parameter or an initializer",
        "#include <stdlib.h>\n#include <string.h>\nvoid g(double *y, double *d, const double *s, int n) { double *e = d; y[0] = e[0]; memcpy(e, s, n * sizeof(double)); }\n",
        "#include <stdlib.h>\n#include <string.h>\nvoid g(double *y, char *d, const char *s, int n) { char *e = d; double z = e[0]; y[0] = z; memcpy(e, s, n * sizeof(double)); }\n"],
    ["D22", "operand root is the base pointer of the arithmetic, past casts and &",
        "#include <stdlib.h>\n#include <string.h>\nvoid f(double *cent, const char *x, int c, int cols) { memcpy((void *)(cent + c * cols), &x[c * cols], cols * sizeof(double)); }\n",
        "#include <stdlib.h>\n#include <string.h>\nvoid f(char *cent, const char *x, int c, int cols) { memcpy(cent + c * cols, x + c * cols, cols * sizeof(double)); }\n"],
    ["D04", "result used by an operator, ternary or assignment",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nint f(C *c, V v, int k) { const char *s = k ? 0 : JS_ToCString(c, v); int b = k && JS_ToBool(c, v); int n = !JS_ToBool(c, v); return (s != 0) + b + n; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\nint f(C *c, V v, int k) { int i = 0; if (k) JS_ToInt32(c, &i, v); return i; }\n"],
    ["D04", "number check whose branch falls through",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (!JS_IsNumber(v)) return -1; JS_ToFloat64(c, &d, v); return d; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v, int k) { double d = 0; if (!JS_IsNumber(v)) d = 1; if (k) return 0; JS_ToFloat64(c, &d, v); return d; }\n"],
    ["D04", "number check with the conversion in its else branch",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (JS_IsNumber(v)) JS_ToFloat64(c, &d, v); return d; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (JS_IsNumber(v)) d = 1; else JS_ToFloat64(c, &d, v); return d; }\n"],
    ["D04", "a BigInt check does not make the conversion infallible",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (JS_IsNumber(v)) { JS_ToFloat64(c, &d, v); } return d; }\n",
        "typedef struct C C; typedef long V;\nint JS_ToInt32(C *c, int *o, V v);\nint JS_ToFloat64(C *c, double *o, V v);\nint JS_ToBool(C *c, V v);\nconst char *JS_ToCString(C *c, V v);\nint JS_IsNumber(V v);\nint JS_IsBigInt(C *c, V v);\nV JS_NewUint32(C *c, unsigned u);\nV JS_ToNumericFree(C *c, V v);\nV JS_GetPropertyStr(C *c, V o, const char *k);\nint JS_ToUint32(C *c, unsigned *o, V v);\ndouble f(C *c, V v) { double d = 0; if (JS_IsBigInt(c, v)) { JS_ToFloat64(c, &d, v); } return d; }\n"],
    ["D33", "result captured by a ternary assignment",
        "long write(int, const void *, unsigned long);\nlong f(int fd, const char *b, int n, int h) { long r; r = h ? write(fd, b, 1) : write(fd, b, n); return r; }\n",
        "long write(int, const void *, unsigned long);\nvoid f(int fd, const char *b, int n, int h) { h ? write(fd, b, 1) : write(fd, b, n); }\n"],
    ["D33", "member callback named like libc",
        "struct T { void (*send)(void *, const char *, int); void *ud; };\nvoid f(struct T *t, const char *b, int n) { t->send(t->ud, b, n); }\n",
        "long send(int, const void *, unsigned long, int);\nvoid f(int fd, const char *b, int n) { send(fd, b, n, 0); }\n"],
    ["D30", "index used only in a later loop",
        "void g(int);\nint f(const int *a, int n) { int i, s = 0; for (i = 0; i <= 4; i++) g(i); for (i = 0; i < n; i++) s += a[i]; return s; }\n",
        "void g(int);\nint f(const int *a, int n) { int i, s = 0; for (i = 0; i <= 4; i++) g(a[i]); for (i = 0; i < n; i++) s += a[i]; return s; }\n"],
    ["D21", "clamp ternary picking the unbounded side",
        "#include <stdlib.h>\n#include <stdint.h>\n#define CAP 4096\nvoid *g(size_t vn) { size_t pn = vn > CAP ? CAP : vn; return malloc(4 * pn + 1); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\n#define CAP 4096\nvoid *g(size_t vn) { size_t pn = vn > CAP ? vn : CAP; return malloc(4 * pn + 1); }\n"],
    ["D21", "widened product of a 64-bit operand",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(uint32_t n, uint32_t m) { return malloc((size_t)n * m); }\n",
        "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(size_t n, uint32_t m) { return malloc((size_t)n * m); }\n"],
    ["D03", "a NULL test that returns or tests another variable still leaks the old block",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nvoid g(char *a, size_t n) { char *p = a; p = realloc(p, n); if (!p) exit(1); }\n",
        "#include <stdlib.h>\nvoid fz_fail(const char *m);\nint g(char *a, char *b, size_t n) { char *p = a; p = realloc(p, n); if (!p) return -1; if (!b) abort(); return 0; }\n"],
    ["D03", "member lvalue realloc'd into itself",
        "#include <stdlib.h>\nstruct S { int *f; int *g; };\nvoid f(struct S *s, unsigned n) { s->g = realloc(s->f, n); }\n",
        "#include <stdlib.h>\nstruct S { int *f; };\nvoid f(struct S *s, unsigned n) { s->f = realloc(s->f, n); }\n"],
];
const INC_FRAG = "static inline int frag_one(void) { return 1; }\n";
const DRIFT = ["drift.c", "#include <stdlib.h>\nvoid f(char *d, const char *s) { strcpy(d, s); }\n"];

function writeFile(p, s) {
    const f = std.open(p, "w");
    f.puts(s);
    f.close();
}

function mkdirs(p) {
    let cur = "";
    for (const part of p.split("/")) {
        cur = cur === "" ? (part === "" ? "/" : part) : join(cur, part);
        if (part !== "") os.mkdir(cur);
    }
}

let seq = 0;
function run(tmp, args) {
    const outp = join(tmp, "run" + seq++ + ".out");
    const fd = os.open(outp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
    const st = os.exec([BIN, "--std", CG, ...args], { block: true, stdout: fd, stderr: fd });
    os.close(fd);
    const out = std.loadFile(outp);
    os.remove(outp);
    return [st, out];
}

function main() {
    const base = std.getenv("TMPDIR") || "/tmp";
    const tmp = join(base, "cgdefects" + os.getpid() + "_" + Math.floor(Math.random() * 1e9));
    try {
        mkdirs(join(tmp, "src"));
        for (const [name, src] of [...CASES, [CLEAN[0], CLEAN[1], []]]) writeFile(join(tmp, "src", name), src);
        mkdirs(join(tmp, "tests"));
        writeFile(join(tmp, "tests", "t_banned.c"), CASES.find((c) => c[0] === "banned.c")[1]);
        writeFile(join(tmp, "tests", "t_deref.c"), CASES.find((c) => c[0] === "deref_null.c")[1]);
        const [rc, out] = run(tmp, [tmp, "--defects", "--defects-all"]);
        const got = new Map();
        for (const line of out.split("\n")) {
            const parts = line.split(/\s+/).filter((x) => x);
            if (parts.length && ["error", "warn", "info", "NEW"].includes(parts[0])) {
                const k = parts[1].split("-")[0];
                got.set(k, (got.get(k) || 0) + 1);
            }
        }
        const fails = [];
        for (const [name, , want] of CASES)
            for (const w of want) if (!got.get(w)) fails.push(name + ": expected " + w + " to fire");
        if (rc === 0) fails.push("exit code should be nonzero (errors planted)");

        const only = join(tmp, "only");
        mkdirs(join(only, "src"));
        writeFile(join(only, "src", CLEAN[0]), CLEAN[1]);
        for (const outside of [".obj-x", join(".claude", "worktrees", "w", "src")]) {
            mkdirs(join(only, outside));
            writeFile(join(only, outside, CASES[0][0]), CASES[0][1]);
        }
        const [rc2, out2] = run(tmp, [only, "--defects"]);
        if (rc2 !== 0 || !out2.includes("0 findings")) fails.push("clean tree: rc=" + rc2 + " out=" + JSON.stringify(out2.slice(0, 80)));

        const modlib = join(tmp, "modlib");
        for (const lib of ["listed", "unlisted", "englib"]) {
            mkdirs(join(modlib, "src", "core", lib));
            writeFile(join(modlib, "src", "core", lib, lib + "-a.c"), CLEAN[1]);
        }
        writeFile(join(modlib, "build.sh"), 'ENG_CORE_LIBS="englib"\n' +
            "mod_core_libs(){\n" +
            '  case "$1" in\n' +
            "    demo) printf '%s' \"listed\" ;;\n" +
            "  esac\n" +
            "}\n" +
            "mod_objs(){\n" +
            '  for l in $(mod_core_libs "$1"); do _dir_objs "core/$l"; done\n' +
            "}\n");
        const [, out4] = run(tmp, [modlib, "--defects", "--defects-all"]);
        const fields = (l) => l.split(/\s+/).filter((x) => x);
        const d19 = out4.split("\n").filter((l) => { const f = fields(l); return f[0] === "error" && f[1] === "D19"; });
        if (d19.length !== 1 || !d19[0].includes("unlisted-a.c")) fails.push("expected D19 on the unlisted module-only lib only, got " + JSON.stringify(d19));

        const disc = join(tmp, "disc");
        mkdirs(join(disc, "src", "fuzz"));
        for (const f of ["forgot.c", "opt.c", "ok.c", "main.c", "fuzz/f.c"]) writeFile(join(disc, "src", f), CLEAN[1]);
        writeFile(join(disc, "build.sh"), "sources(){\n" +
            "  local x=(! -name opt.c)\n" +
            '  [ "${OPT:-0}" = 1 ] && x=("${x[@]/opt.c/-}")\n' +
            "  find src -name '*.c' ! -name '*.inc.c' ! -path 'src/fuzz/*' ! -name main.c ! -name forgot.c \"${x[@]}\" | sort\n" +
            "}\n" +
            "cc -c src/main.c\n");
        const [, out6] = run(tmp, [disc, "--defects", "--defects-all"]);
        const d19d = out6.split("\n").filter((l) => { const f = fields(l); return f[0] === "error" && f[1] === "D19"; });
        if (d19d.length !== 1 || !d19d[0].includes("forgot.c")) fails.push("expected D19 on the discovery-excluded forgot.c only, got " + JSON.stringify(d19d));

        const near = join(tmp, "near");
        mkdirs(join(near, "src"));
        writeFile(join(near, "src", NEAR_MISS_D47[0]), NEAR_MISS_D47[1]);
        const [, out5] = run(tmp, [near, "--defects", "--defects-all"]);
        if (out5.split("\n").some((l) => { const f = fields(l); return f[0] === "error" && f[1] === "D47"; }))
            fails.push("D47 fired on the near miss (free(*out) before the error return, or an error return on a path that never allocated): " + JSON.stringify(out5.slice(-200)));

        const near51 = join(tmp, "near51");
        mkdirs(join(near51, "src"));
        writeFile(join(near51, "src", NEAR_MISS_D51[0]), NEAR_MISS_D51[1]);
        const [, out51] = run(tmp, [near51, "--defects", "--defects-all"]);
        if (out51.split("\n").some((l) => fields(l)[1] === "D51"))
            fails.push("D51 fired on a checked JS_PromiseSettle result: " + JSON.stringify(out51.slice(-200)));

        const hdr = /^defect scanner: (\d+) findings \((\d+) error, (\d+) warn, (\d+) info\)/m.exec(out);
        const sums = { error: 0, warn: 0, info: 0 };
        let d02 = "";
        for (const line of out.split("\n")) {
            const m = /^  (D\d\d)-\S+\s+((?:(?:error|warn|info) \d+ ?)+)\s+\+\d+ baselined/.exec(line);
            if (!m) continue;
            for (const p of m[2].matchAll(/(error|warn|info) (\d+)/g)) sums[p[1]] += Number(p[2]);
            if (m[1] === "D02") d02 = m[2].trim();
        }
        if (!hdr || Number(hdr[2]) !== sums.error || Number(hdr[3]) !== sums.warn || Number(hdr[4]) !== sums.info ||
            Number(hdr[1]) !== sums.error + sums.warn + sums.info)
            fails.push("header vs table: header=" + (hdr ? hdr[0] : "missing") + " table=" + JSON.stringify(sums));
        if (!out.split("\n").some((l) => /^warn\s+D28\s+\S*tests\/t_deref\.c:/.test(l))) fails.push("D28 in a test harness must stay warn");
        if (d02 !== "error 1 info 1") fails.push("D02 row must split by actual severity (src error, tests info), got '" + d02 + "'");

        const sup = join(tmp, "sup");
        mkdirs(join(sup, "src"));
        mkdirs(join(sup, "tools"));
        writeFile(join(sup, "src", "a.c"), "#include <stdlib.h>\nvoid f(void) { char *p = malloc(4); free(p); free(p); }\nvoid g(void) { char *q = malloc(4); free(q); free(q); }\n");
        writeFile(join(sup, "src", "b.c"), "#include <stdlib.h>\nvoid h(void) { char *p = malloc(4); free(p); free(p); free(p); }\n");
        const head = "file\trule\tfunction\tcount\treason\n";
        writeFile(join(sup, "tools", "defects-baseline.tsv"), head +
            "src/a.c\tD24\tf\t1\tfixture: safe by construction\n" +
            "src/b.c\tD24\th\t1\tfixture: one allowed, two present\n" +
            "src/gone.c\tD24\tz\t1\tfixture: nothing left to match\n");
        const [, outS] = run(tmp, [sup, "--defects-all"]);
        const d24 = outS.split("\n").filter((l) => { const f = fields(l); return f[0] === "warn" && f[1] === "D24"; });
        const at = (f, ln) => d24.some((l) => l.includes(f + ":" + ln + " "));
        if (d24.length !== 3 || at("src/a.c", 2) || !at("src/a.c", 3) || !at("src/b.c", 2))
            fails.push("suppress: want D24 hidden in f only, g and both h findings shown; got " + JSON.stringify(d24));
        if (!/; 1 baselined, 1 stale baseline rows/.test(outS)) fails.push("suppress: header must say '1 baselined, 1 stale': " + JSON.stringify(outS.split("\n")[0]));
        const stale = outS.split("\n").filter((l) => l.startsWith("STALE "));
        if (stale.length !== 1 || !stale[0].includes("src/gone.c")) fails.push("suppress: want one STALE row (src/gone.c), got " + JSON.stringify(stale));
        const [, outN] = run(tmp, [sup, "--defects-all", "--no-suppress"]);
        if (outN.split("\n").filter((l) => { const f = fields(l); return f[0] === "warn" && f[1] === "D24"; }).length !== 4)
            fails.push("--no-suppress must show all 4 D24 findings");
        const [, outT] = run(tmp, [sup, "--emit-tsv"]);
        if (!outT.split("\n").includes("TSV\tsrc/a.c\tD24\tg\t1\t") || !outT.split("\n").includes("TSV\tsrc/b.c\tD24\th\t2\t"))
            fails.push("--emit-tsv rows missing: " + JSON.stringify(outT.split("\n").filter((l) => l.startsWith("TSV"))));
        for (const [bad, why] of [["src/a.c\tD24\tf\t1\n", "no header"], [head + "src/a.c\tD24\tf\t0\tfixture reason\n", "count 0"],
            [head + "src/a.c\tD24\tf\t1\tshort\n", "short reason"], [head + "src/a.c\tD24\tf\t1\tfixture reason\nsrc/a.c\tD24\tf\t1\tfixture reason\n", "duplicate"]]) {
            writeFile(join(sup, "tools", "defects-baseline.tsv"), bad);
            const [rcB] = run(tmp, [sup]);
            if (rcB !== 2) fails.push("malformed suppression file (" + why + ") must exit 2, got " + rcB);
        }

        const stg = join(tmp, "stg");
        mkdirs(join(stg, "src"));
        const fname = (rule, st, kind) => rule + "_" + st.replaceAll("-", "_") + "_" + kind + ".c";
        for (const [rule, st, neg, pos] of STAGE_CASES) {
            writeFile(join(stg, "src", fname(rule, st, "neg")), neg);
            writeFile(join(stg, "src", fname(rule, st, "pos")), pos);
        }
        const [, outG] = run(tmp, [stg, "--defects-all", "--no-suppress"]);
        const hits = (rule, file) => outG.split("\n").filter((l) => { const f = fields(l); return f[1] === rule && f[2] && f[2].startsWith("src/" + file + ":"); }).length;
        for (const [rule, st] of STAGE_CASES) {
            if (hits(rule, fname(rule, st, "neg"))) fails.push("stage " + rule + "/" + st + ": the noise shape is still reported");
            if (!hits(rule, fname(rule, st, "pos"))) fails.push("stage " + rule + "/" + st + ": the real bug next to it is no longer reported");
            if (!new RegExp("stages " + rule + ": raw \\d+.* -> " + st + " \\d+").test(outG)) fails.push("stage " + rule + "/" + st + ": missing from the stage report");
        }

        const rul = join(tmp, "rul");
        mkdirs(join(rul, "src"));
        RULE_CASES.forEach(([, , neg, pos], i) => {
            writeFile(join(rul, "src", "r" + i + "_neg.c"), neg);
            writeFile(join(rul, "src", "r" + i + "_pos.c"), pos);
        });
        writeFile(join(rul, "src", "d21_once.c"), "#include <stdlib.h>\n#include <stdint.h>\nvoid *f(size_t a, size_t b) { return malloc(a * b); }\nvoid *g(size_t a, size_t b) { return calloc(a * b, 1); }\n");
        const [, outR] = run(tmp, [rul, "--defects-all", "--no-suppress"]);
        const once = outR.split("\n").filter((l) => { const f = fields(l); return f[1] === "D21" && f[2] && f[2].startsWith("src/d21_once.c:"); }).length;
        if (once !== 2) fails.push("D21: one finding per allocation expected, got " + once + " for 2 allocations");
        {
            const d49 = join(tmp, "d49sev");
            mkdirs(join(d49, "src"));
            writeFile(join(d49, "src", "cont.c"), "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\nV fin(C *c, V data, int magic, int is_reject, V arg) { V t = JS_GetPropertyUint32(c, data, 0); (void)magic; (void)is_reject; return JS_SetPropertyStr(c, t, \"v\", arg); }\nint opt(C *c, V o) { V t = JS_GetPropertyStr(c, o, \"t\"); return JS_SetPropertyStr(c, t, \"v\", o); }\n");
            const [, outS49] = run(tmp, [d49, "--defects-all", "--no-suppress"]);
            const sev = (ln) => outS49.split("\n").filter((l) => { const f = fields(l); return f[1] === "D49" && f[2] === "src/cont.c:" + ln; }).map((l) => fields(l)[0]).join(",");
            if (sev(16) !== "info") fails.push("D49: a continuation's own data array read must be info (internal receiver), got " + JSON.stringify(sev(16)));
            if (sev(17) !== "warn") fails.push("D49: a caller option read must stay warn, got " + JSON.stringify(sev(17)));
        }
        {
            const g = join(tmp, "gate");
            mkdirs(join(g, "src"));
            mkdirs(join(g, "tools"));
            mkdirs(join(g, "tests"));
            const hit = (fn) => "int " + fn + "(C *c, V o, V d) { return JS_SetPropertyStr(c, d, \"k\", JS_GetPropertyStr(c, o, \"k\")); }\n";
            const head = "file\trule\tfunction\tcount\treason\n";
            const row = (fn, n) => "src/a.c\tD49\t" + fn + "\t" + n + "\tD49 backlog fixture\n";
            const ref = join(tmp, "gate-ref.tsv");
            writeFile(join(g, "tests", "t.c"), "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\n" + hit("t"));
            const gate = (src, tsv, refTsv) => {
                writeFile(join(g, "src", "a.c"), "typedef struct C C; typedef long V;\nV JS_GetPropertyStr(C *c, V o, const char *k);\nV JS_GetPropertyUint32(C *c, V o, unsigned i);\nint JS_IsException(V v);\nint JS_IsUndefined(V v);\nint JS_IsNull(V v);\nint JS_IsString(V v);\nint JS_IsNumber(V v);\nint JS_ToInt32(C *c, int *p, V v);\nint JS_ToBool(C *c, V v);\nvoid JS_FreeValue(C *c, V v);\nint JS_HasException(C *c);\nint JS_SetPropertyStr(C *c, V o, const char *k, V v);\nV JS_ThrowTypeError(C *c, const char *m);\nint dtype(C *c, V v);\n" + src);
                writeFile(join(g, "tools", "defects-baseline.tsv"), head + tsv);
                const extra = refTsv === undefined ? [] : (writeFile(ref, head + refTsv), ["--baseline-ref", "file:" + ref]);
                return run(tmp, [g, "--no-git", "--gate", "D04,D28,D48,D49", ...extra]);
            };
            const f2 = hit("f").replace("{ return", "{ JS_SetPropertyStr(c, d, \"j\", JS_GetPropertyStr(c, o, \"j\")); return");
            const cases = [
                ["a baselined hit passes", gate(hit("f"), row("f", 1)), 0, null],
                ["a hit beyond its row fails", gate(f2, row("f", 1)), 1, /^GATE warn\s+D49\s+src\/a.c:\d+/m],
                ["a stale row (fixed, not lowered) fails", gate(hit("f"), row("f", 2)), 1, /^GATE stale row src\/a.c D49 f: 2 baselined, 1 found/m],
                ["an unchanged baseline vs the ref passes", gate(hit("f"), row("f", 1), row("f", 1)), 0, null],
                ["a lowered count vs the ref passes", gate(f2.replace(/JS_SetPropertyStr\(c, d, "j"[^;]*;/, ""), row("f", 1), row("f", 2)), 0, null],
                ["a count above the ref fails", gate(f2, row("f", 2), row("f", 1)), 1, /^GATE baseline grew vs file:\S+ src\/a.c D49 f 1 -> 2/m],
                ["a new row of an already-gated rule fails", gate(hit("f") + hit("g"), row("f", 1) + row("g", 1), row("f", 1)), 1, /^GATE baseline grew vs file:\S+ src\/a.c D49 g new row/m],
                ["the first rows of a newly gated rule are the backlog import", gate(hit("f") + hit("g"), row("f", 1) + row("g", 1), "src/a.c\tD31\tz\t1\tother rule fixture\n"), 0, null],
            ];
            for (const [what, [rc, out], want, re] of cases) {
                if ((rc ? 1 : 0) !== want) fails.push("gate (" + what + "): want rc " + want + ", got " + rc + " " + JSON.stringify(out.split("\n").filter((l) => /^GATE|^gate/.test(l))));
                if (re && !re.test(out)) fails.push("gate (" + what + "): missing " + re);
            }
            if (/GATE.*tests\/t\.c/.test(cases[1][1][1])) fails.push("gate: an info finding under tests/ must not gate");
        }
        RULE_CASES.forEach(([rule, what], i) => {
            const n = (k) => outR.split("\n").filter((l) => { const f = fields(l); return f[1] === rule && f[2] && f[2].startsWith("src/r" + i + "_" + k + ".c:"); }).length;
            if (n("neg")) fails.push(rule + " (" + what + "): near miss reported");
            if (!n("pos")) fails.push(rule + " (" + what + "): not reported");
        });
        const exc = join(tmp, "d50x");
        mkdirs(join(exc, "src", "fuzz"));
        for (const f of ["fuzz/fz.c", "dyna-cli.c", "dyna-bench.c", "native.c"]) writeFile(join(exc, "src", f), D50H + D50_CALL_DROP);
        const [, outX] = run(tmp, [exc, "--defects-all", "--no-suppress"]);
        const d50At = outX.split("\n").filter((l) => fields(l)[1] === "D50").map((l) => fields(l)[2].split(":")[0]).sort().join(",");
        if (d50At !== "src/native.c") fails.push("D50: fuzz harnesses and the CLI/bench embedders are excluded, natives are not: got " + d50At);

        for (const [kind, guard, wantD18] of [["neg", true, 0], ["pos", false, 1]]) {
            const inc = join(tmp, "inc_" + kind);
            mkdirs(join(inc, "src"));
            writeFile(join(inc, "src", "frag.inc.c"), guard ? "#ifndef FRAG_INC_C\n#define FRAG_INC_C\n" + INC_FRAG + "#endif\n" : INC_FRAG);
            writeFile(join(inc, "src", "a.c"), "#include \"frag.inc.c\"\nint a(void) { return frag_one(); }\n");
            writeFile(join(inc, "src", "b.c"), "#include \"frag.inc.c\"\nint b(void) { return frag_one(); }\n");
            const [, outI] = run(tmp, [inc, "--defects-all"]);
            const n = outI.split("\n").filter((l) => fields(l)[1] === "D18").length;
            if (n !== wantD18) fails.push("D18 " + (guard ? "include-guarded" : "unguarded") + " all-static fragment in 2 files: want " + wantD18 + " finding, got " + n);
        }

        const gt = join(tmp, "gitroot");
        mkdirs(join(gt, "src"));
        mkdirs(join(gt, "bench"));
        writeFile(join(gt, ".gitignore"), "bench/*\n");
        writeFile(join(gt, "src", "seen.c"), CASES.find((c) => c[0] === "double_free.c")[1]);
        writeFile(join(gt, "bench", "hidden.c"), CASES.find((c) => c[0] === "double_free.c")[1]);
        os.exec(["git", "-C", gt, "init", "-q"], { block: true });
        const d24At = (o) => o.split("\n").filter((l) => fields(l)[1] === "D24").map((l) => fields(l)[2].split(":")[0]).sort().join(",");
        const [, outGit] = run(tmp, [gt, "--defects-all"]);
        if (d24At(outGit) !== "src/seen.c") fails.push("git root: want D24 only in the untracked, unignored src/seen.c, got " + d24At(outGit));
        const [, outNoGit] = run(tmp, [gt, "--defects-all", "--no-git"]);
        if (d24At(outNoGit) !== "bench/hidden.c,src/seen.c") fails.push("--no-git: want both files, got " + d24At(outNoGit));

        const bl = join(tmp, "base.json");
        run(tmp, [tmp, "--defects", "--update-baseline", bl]);
        writeFile(join(tmp, "src", DRIFT[0]), DRIFT[1]);
        const [rc3, out3] = run(tmp, [tmp, "--defects", "--defects-baseline", bl]);
        if (rc3 !== 1 || !out3.includes("NEW")) fails.push("gate: rc=" + rc3 + " (want 1), NEW in out=" + (out3.includes("NEW") ? "True" : "False"));

        for (const f of fails) print("FAIL " + f);
        const n = CASES.reduce((a, c) => a + c[2].length, 0);
        print("defects selftest: " + (n - fails.filter((f) => f.includes("expected")).length) + "/" + n +
            " check expectations, gate " + (fails.some((f) => f.includes("gate")) ? "BROKEN" : "OK"));
        return fails.length ? 1 : 0;
    } finally {
        os.exec(["rm", "-rf", tmp], { block: true });
    }
}

std.exit(main());
