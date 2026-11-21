#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fz_sb {
    char* b;
    size_t n, cap;
} fz_sb_t;
#define NA(a) (sizeof(a) / sizeof((a)[0]))

static void fz_sb_put(fz_sb_t* s, const char* p, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1024;
        while (c < s->n + n + 1)
            c *= 2;
        s->b = realloc(s->b, c);
        if (!s->b)
            fz_fail("realloc");
        s->cap = c;
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
    s->b[s->n] = 0;
}

static void fz_sb_str(fz_sb_t* s, const char* p) { fz_sb_put(s, p, strlen(p)); }

static const char* const lits[] = { "0", "-0", "1", "-1", "1.5", "NaN", "Infinity", "-Infinity", "2**53", "2**31-1", "-(2**31)", "0x7fffffff",
    "1e21", "5e-324", "1n", "-1n", "2n**64n", "0n", "''", "'a'", "'\\u{1F600}'", "'\\uD800'", "'x'.repeat(17)", "'0'", "'-0'", "'__proto__'",
    "true", "false", "null", "undefined", "[]", "{}", "[1,,3]", "[1,2,3]", "Symbol.iterator", "Symbol('s')", "/a+b|c/g", "/(?<n>\\d)\\k<n>/u",
    "new Date(0)", "function(){}", "()=>{}", "class {}", "Object.prototype", "Array.prototype", "globalThis", "this", "arguments", "new.target" };
static const char* const bins[] = { "+", "-", "*", "/", "%", "**", "&", "|", "^", "<<", ">>", ">>>", "==", "===", "!=", "!==", "<", "<=", ">",
    ">=", "&&", "||", "??", "in", "instanceof", "," };
static const char* const unas[] = { "typeof ", "-", "+", "~", "!", "void ", "...", "await " };
static const char* const meths[] = { "push", "pop", "shift", "unshift", "slice", "splice", "concat", "join", "reverse", "sort", "map", "filter",
    "reduce", "reduceRight", "indexOf", "lastIndexOf", "includes", "find", "findLast", "flat", "flatMap", "fill", "copyWithin", "at", "keys",
    "entries", "values", "toString", "valueOf", "toSorted", "toSpliced", "with", "charAt", "charCodeAt", "codePointAt", "split", "replace",
    "replaceAll", "match", "matchAll", "search", "padStart", "padEnd", "trim", "normalize", "localeCompare", "substring", "toUpperCase",
    "toFixed", "toPrecision", "toExponential", "then", "catch", "finally", "next", "return", "throw", "get", "set", "has", "add", "delete",
    "forEach", "every", "some", "exec", "test", "call", "apply", "bind", "subarray", "byteLength", "getUint32", "setFloat64", "groupBy" };
static const char* const ctors[] = { "new Array(%s)", "new Map([[%s, 1]])", "new Set(%s)", "new WeakMap()", "new WeakRef({})",
    "new Proxy(%s, { get(t, k, r) { return Reflect.get(t, k, r); }, has() { return true; }, ownKeys() { return []; } })",
    "new Uint8Array(%s)", "new Float64Array([%s])", "new BigInt64Array(4)", "new Int32Array(new ArrayBuffer(16, { maxByteLength: 64 }))",
    "new DataView(new ArrayBuffer(8))", "new RegExp(String(%s), 'gimsuy'.slice(0, 3))", "Object.create(%s)",
    "Object.defineProperty({}, 'x', { get() { return %s; }, configurable: true })", "Reflect.ownKeys(Object(%s))",
    "JSON.parse(JSON.stringify(%s) ?? 'null')", "Array.from(%s ?? [])", "String.fromCodePoint(65, %s & 0xffff)", "Symbol(String(%s))",
    "BigInt(Math.trunc(Number(%s)) || 0)", "Math.max(%s, -0)", "Promise.resolve(%s)", "Object.freeze([%s])", "Object.entries(Object(%s))",
    "structuredClone === undefined ? 0 : 0", "Array.prototype.slice.call(%s ?? {})", "[%s].sort((a, b) => (a > b) - (a < b))",
    "new Error(String(%s), { cause: 1 })", "Object.groupBy([1, 2, 3], () => String(%s))", "new Intl.NumberFormat ? 0 : 0",
    "String.raw`a${%s}b`", "Atomics.add(new Int32Array(new SharedArrayBuffer(8)), 0, 1)", "Number.parseFloat(String(%s))",
    "Iterator.from([1, 2, 3]).map(x => x).toArray()", "Array.prototype.concat.apply([], [%s])" };

typedef struct {
    fz_in_t* in;
    fz_sb_t* s;
    int depth;
    int nfun;
} gen_t;

static void g_expr(gen_t* g);
static void g_stmts(gen_t* g, unsigned max);

static void g_var(gen_t* g)
{
    char t[16];
    snprintf(t, sizeof(t), "v%u", fz_below(g->in, 6));
    fz_sb_str(g->s, t);
}

static void g_expr(gen_t* g)
{
    unsigned k;
    char t[256];
    if (!g->in->n || g->depth > 8) {
        fz_sb_str(g->s, lits[fz_below(g->in, NA(lits))]);
        return;
    }
    g->depth++;
    k = fz_below(g->in, 16);
    switch (k) {
    case 0:
    case 1:
        fz_sb_str(g->s, lits[fz_below(g->in, NA(lits))]);
        break;
    case 2:
        g_var(g);
        break;
    case 3:
        fz_sb_str(g->s, "(");
        g_expr(g);
        fz_sb_str(g->s, " ");
        fz_sb_str(g->s, bins[fz_below(g->in, NA(bins))]);
        fz_sb_str(g->s, " ");
        g_expr(g);
        fz_sb_str(g->s, ")");
        break;
    case 4: {
        unsigned u = fz_below(g->in, NA(unas));
        fz_sb_str(g->s, u == 6 ? "[" : "(");
        fz_sb_str(g->s, unas[u]);
        g_expr(g);
        fz_sb_str(g->s, u == 6 ? "]" : ")");
        break;
    }
    case 5:
    case 6:
        g_expr(g);
        fz_sb_str(g->s, fz_u8(g->in) & 1 ? "?." : ".");
        fz_sb_str(g->s, meths[fz_below(g->in, NA(meths))]);
        fz_sb_str(g->s, "(");
        g_expr(g);
        fz_sb_str(g->s, ", ");
        g_expr(g);
        fz_sb_str(g->s, ")");
        break;
    case 7: {
        fz_sb_t inner = { 0 };
        fz_sb_t* save = g->s;
        g->s = &inner;
        g_expr(g);
        g->s = save;
        snprintf(t, sizeof(t), ctors[fz_below(g->in, NA(ctors))], inner.b ? inner.b : "0");
        if (inner.n < 200)
            fz_sb_str(g->s, t);
        else
            fz_sb_str(g->s, "0");
        free(inner.b);
        break;
    }
    case 8:
        fz_sb_str(g->s, "[");
        g_expr(g);
        fz_sb_str(g->s, ", ");
        g_expr(g);
        fz_sb_str(g->s, "]");
        break;
    case 9:
        fz_sb_str(g->s, "({ a: ");
        g_expr(g);
        fz_sb_str(g->s, ", [Symbol.toPrimitive]() { return ");
        g_expr(g);
        fz_sb_str(g->s, "; }, get b() { return ");
        g_var(g);
        fz_sb_str(g->s, "; }, __proto__: ");
        g_expr(g);
        fz_sb_str(g->s, " })");
        break;
    case 10:
        g_var(g);
        fz_sb_str(g->s, "[");
        g_expr(g);
        fz_sb_str(g->s, "]");
        break;
    case 11:
        fz_sb_str(g->s, "(");
        g_var(g);
        fz_sb_str(g->s, fz_u8(g->in) & 1 ? ".p = " : "[0] = ");
        g_expr(g);
        fz_sb_str(g->s, ")");
        break;
    case 12:
        fz_sb_str(g->s, "((a, b) => ");
        g_expr(g);
        fz_sb_str(g->s, ")(");
        g_expr(g);
        fz_sb_str(g->s, ")");
        break;
    case 13:
        if (g->nfun) {
            snprintf(t, sizeof(t), "f%u(", fz_below(g->in, (unsigned)g->nfun));
            fz_sb_str(g->s, t);
            g_expr(g);
            fz_sb_str(g->s, ")");
        } else
            g_var(g);
        break;
    case 14:
        fz_sb_str(g->s, "`${");
        g_expr(g);
        fz_sb_str(g->s, "}x`");
        break;
    default:
        fz_sb_str(g->s, "(");
        g_expr(g);
        fz_sb_str(g->s, " ? ");
        g_expr(g);
        fz_sb_str(g->s, " : ");
        g_expr(g);
        fz_sb_str(g->s, ")");
        break;
    }
    g->depth--;
}

static void g_stmt(gen_t* g)
{
    char t[64];
    unsigned k = fz_below(g->in, 12);
    if (g->depth > 6)
        k = k % 3;
    g->depth++;
    switch (k) {
    case 0:
        g_var(g);
        fz_sb_str(g->s, " = ");
        g_expr(g);
        fz_sb_str(g->s, ";\n");
        break;
    case 1:
    case 2:
        g_expr(g);
        fz_sb_str(g->s, ";\n");
        break;
    case 3:
        fz_sb_str(g->s, "if (");
        g_expr(g);
        fz_sb_str(g->s, ") {\n");
        g_stmts(g, 3);
        fz_sb_str(g->s, "} else {\n");
        g_stmts(g, 2);
        fz_sb_str(g->s, "}\n");
        break;
    case 4:
        snprintf(t, sizeof(t), "for (let i = 0; i < %u; i++) {\n", fz_below(g->in, g->depth > 1 ? 4 : 40));
        fz_sb_str(g->s, t);
        g_stmts(g, 3);
        fz_sb_str(g->s, "}\n");
        break;
    case 5:
        fz_sb_str(g->s, "try {\n");
        g_stmts(g, 3);
        fz_sb_str(g->s, "} catch (e) {\n");
        g_var(g);
        fz_sb_str(g->s, " = e;\n} finally {\n");
        g_stmts(g, 1);
        fz_sb_str(g->s, "}\n");
        break;
    case 6:
        snprintf(t, sizeof(t), "function f%d(a, b) {\n", g->nfun < 6 ? g->nfun++ : 5);
        fz_sb_str(g->s, t);
        g_stmts(g, 3);
        fz_sb_str(g->s, "return ");
        g_expr(g);
        fz_sb_str(g->s, ";\n}\n");
        break;
    case 7:
        fz_sb_str(g->s, "class C extends (");
        fz_sb_str(g->s, fz_u8(g->in) & 1 ? "Array" : "Object");
        fz_sb_str(g->s, ") { #p = 1; constructor(a) { super(a); this.x = ");
        g_expr(g);
        fz_sb_str(g->s, "; } m() { return ");
        g_expr(g);
        fz_sb_str(g->s, "; } static s = 1; get g() { return this.#p; } }\n");
        g_var(g);
        fz_sb_str(g->s, " = new C(");
        g_expr(g);
        fz_sb_str(g->s, ");\n");
        break;
    case 8:
        fz_sb_str(g->s, "(async () => {\n");
        g_stmts(g, 2);
        fz_sb_str(g->s, "await ");
        g_expr(g);
        fz_sb_str(g->s, ";\n})().catch(() => 0);\n");
        break;
    case 9:
        fz_sb_str(g->s, "for (const x of (function* () { yield ");
        g_expr(g);
        fz_sb_str(g->s, "; yield* [1, 2]; })()) {\n");
        g_stmts(g, 2);
        fz_sb_str(g->s, "}\n");
        break;
    case 10:
        fz_sb_str(g->s, "switch (");
        g_expr(g);
        fz_sb_str(g->s, ") { case 0: ");
        g_stmts(g, 1);
        fz_sb_str(g->s, "break; default: ");
        g_stmts(g, 1);
        fz_sb_str(g->s, "}\n");
        break;
    default:
        fz_sb_str(g->s, "{ let [q, ...r] = [");
        g_expr(g);
        fz_sb_str(g->s, "]; const { a = ");
        g_expr(g);
        fz_sb_str(g->s, ", ...o } = Object(q); ");
        g_var(g);
        fz_sb_str(g->s, " = [q, r, a, o]; }\n");
        break;
    }
    g->depth--;
}

static void g_stmts(gen_t* g, unsigned max)
{
    unsigned n = 1 + fz_below(g->in, max), i;
    for (i = 0; i < n && g->in->n; i++)
        g_stmt(g);
}

static void fz_js_program(fz_in_t* in, fz_sb_t* s)
{
    gen_t g = { in, s, 0, 0 };
    unsigned i;
    fz_sb_str(s, "var v0 = 1, v1 = 'a', v2 = [1, 2], v3 = { k: 1 }, v4 = 2n, v5 = null;\n");
    for (i = 0; i < 24 && in->n; i++) {
        fz_sb_str(s, "try {\n");
        g_stmt(&g);
        fz_sb_str(s, "} catch (e) {}\n");
    }
}
