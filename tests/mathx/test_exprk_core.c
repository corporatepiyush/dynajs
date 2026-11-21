/*
 * test_exprk_core.c -- src/core/exprk (the dyna:mathx Expression compiler
 * and stack machine) as a standalone C library.
 *
 * The oracle is a recursive-descent evaluator written here from the
 * documented grammar, not a shunting-yard like the code: sum := product
 * (('+'|'-') product)*, product := unary (('*'|'/'|'%') unary)*, unary :=
 * ('-'|'+') unary | power, power := primary ('^' unary)? (so ^ is right
 * associative and binds tighter than a leading minus: -2^2 = -4, 2^-1 =
 * 0.5), primary := number | pi | e | name | fn(args) | (sum). Numbers are
 * the JS decimal literal subset (digits, one dot, exponent) read with
 * strtod; anything else made of those characters is "bad number". 40k
 * random well-formed expressions over x, y, z must compile, list their
 * variables in first-use order, and evaluate bit-identically (NaN == NaN)
 * on random inputs, with the stack sized by max_depth under ASan; 60k
 * mutated strings must be accepted exactly when the oracle accepts them.
 * Limits: 4095 instructions compile and 4097 do not; 200 nested
 * parentheses compile and 300 do not. The sign of a zero result is not
 * compared: min/max pick b on a tie, so max(0, -0) is -0 where JS Math.max
 * gives +0; the d.ts does not pin it (reported as an owner question).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-exprk.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static unsigned long long rs = 0x5deece66dull;
static unsigned rnd(unsigned n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

static int js_num(void* ud, const char* s, size_t n, double* out)
{
    size_t i = 0, a;
    int digits = 0;
    char buf[128];
    (void)ud;
    while (i < n && s[i] >= '0' && s[i] <= '9')
        i++, digits++;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++, digits++;
    }
    if (!digits)
        return EXPRK_NUM_BAD;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        a = i;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if (i == a)
            return EXPRK_NUM_BAD;
    }
    if (i != n || n >= sizeof(buf))
        return EXPRK_NUM_BAD;
    memcpy(buf, s, n);
    buf[n] = 0;
    *out = strtod(buf, NULL);
    return EXPRK_NUM_OK;
}

typedef struct {
    const char* s;
    size_t i, n;
    int err, unk;
    const double* vars;
    char names[3];
    int nnames;
} O;

static void sp(O* o)
{
    while (o->i < o->n && (o->s[o->i] == ' ' || o->s[o->i] == '\t'))
        o->i++;
}

static double o_sum(O* o);

static int isid(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static double o_call(O* o, const char* f, size_t fl)
{
    static const char* const F1[] = { "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "sqrt", "cbrt", "exp", "log", "log2",
        "log10", "abs", "floor", "ceil", "round", "trunc", "sign", "expm1", "log1p" };
    static const char* const F2[] = { "atan2", "pow", "hypot", "min", "max", "fmod" };
    double a[3];
    int argc = 0, k, want = 0, which = -1;
    for (k = 0; k < 23; k++)
        if (strlen(F1[k]) == fl && !memcmp(F1[k], f, fl))
            want = 1, which = k;
    for (k = 0; k < 6; k++)
        if (strlen(F2[k]) == fl && !memcmp(F2[k], f, fl))
            want = 2, which = k;
    if (which < 0) {
        o->err = 1;
        return 0;
    }
    for (;;) {
        double v = o_sum(o);
        if (o->err)
            return 0;
        if (argc < 3)
            a[argc] = v;
        argc++;
        sp(o);
        if (o->i < o->n && o->s[o->i] == ',') {
            o->i++;
            continue;
        }
        if (o->i < o->n && o->s[o->i] == ')') {
            o->i++;
            break;
        }
        o->err = 1;
        return 0;
    }
    if (argc != want) {
        o->err = 1;
        return 0;
    }
    if (want == 1) {
        double x = a[0];
        switch (which) {
        case 0: return sin(x);
        case 1: return cos(x);
        case 2: return tan(x);
        case 3: return asin(x);
        case 4: return acos(x);
        case 5: return atan(x);
        case 6: return sinh(x);
        case 7: return cosh(x);
        case 8: return tanh(x);
        case 9: return sqrt(x);
        case 10: {
            double r = cbrt(x), w = nearbyint(r);
            return w * w * w == x ? w : r;
        }
        case 11: return exp(x);
        case 12: return log(x);
        case 13: return log2(x);
        case 14: return log10(x);
        case 15: return fabs(x);
        case 16: return floor(x);
        case 17: return ceil(x);
        case 18: return round(x);
        case 19: return trunc(x);
        case 20: return x > 0 ? 1 : x < 0 ? -1 : x;
        case 21: return expm1(x);
        default: return log1p(x);
        }
    }
    switch (which) {
    case 0: return atan2(a[0], a[1]);
    case 1: return pow(a[0], a[1]);
    case 2: return hypot(a[0], a[1]);
    case 3: return isnan(a[0]) || isnan(a[1]) ? NAN : fmin(a[0], a[1]);
    case 4: return isnan(a[0]) || isnan(a[1]) ? NAN : fmax(a[0], a[1]);
    default: return fmod(a[0], a[1]);
    }
}

static double o_unary(O* o);

static double o_primary(O* o)
{
    char c;
    sp(o);
    if (o->i >= o->n) {
        o->err = 1;
        return 0;
    }
    c = o->s[o->i];
    if ((c >= '0' && c <= '9') || c == '.') {
        size_t a = o->i;
        double d;
        while (o->i < o->n && (strchr("0123456789.eE", o->s[o->i]) || ((o->s[o->i] == '+' || o->s[o->i] == '-') && (o->s[o->i - 1] | 32) == 'e')))
            o->i++;
        if (js_num(NULL, o->s + a, o->i - a, &d) != EXPRK_NUM_OK)
            o->err = 1;
        return d;
    }
    if (isid(c)) {
        size_t a = o->i;
        while (o->i < o->n && isid(o->s[o->i]))
            o->i++;
        {
            size_t l = o->i - a;
            sp(o);
            if (o->i < o->n && o->s[o->i] == '(') {
                o->i++;
                return o_call(o, o->s + a, l);
            }
            if (l == 2 && !memcmp(o->s + a, "pi", 2))
                return 3.14159265358979323846;
            if (l == 1 && o->s[a] == 'e')
                return 2.71828182845904523536;
            if (l == 1 && o->s[a] >= 'x' && o->s[a] <= 'z') {
                if (!memchr(o->names, o->s[a], (size_t)o->nnames))
                    o->names[o->nnames++] = o->s[a];
                return o->vars[o->s[a] - 'x'];
            }
            o->unk = 1;
            return 0;
        }
    }
    if (c == '(') {
        double v;
        o->i++;
        v = o_sum(o);
        sp(o);
        if (o->err || o->i >= o->n || o->s[o->i] != ')') {
            o->err = 1;
            return 0;
        }
        o->i++;
        return v;
    }
    o->err = 1;
    return 0;
}

static double o_power(O* o)
{
    double b = o_primary(o);
    if (o->err)
        return 0;
    sp(o);
    if (o->i < o->n && o->s[o->i] == '^') {
        double e;
        o->i++;
        e = o_unary(o);
        return pow(b, e);
    }
    return b;
}

static double o_unary(O* o)
{
    sp(o);
    if (o->i < o->n && o->s[o->i] == '-') {
        o->i++;
        return -o_unary(o);
    }
    if (o->i < o->n && o->s[o->i] == '+') {
        o->i++;
        return o_unary(o);
    }
    return o_power(o);
}

static double o_product(O* o)
{
    double v = o_unary(o);
    for (;;) {
        char c;
        sp(o);
        if (o->err || o->i >= o->n)
            return v;
        c = o->s[o->i];
        if (c != '*' && c != '/' && c != '%')
            return v;
        o->i++;
        {
            double r = o_unary(o);
            v = c == '*' ? v * r : c == '/' ? v / r : fmod(v, r);
        }
    }
}

static double o_sum(O* o)
{
    double v = o_product(o);
    for (;;) {
        char c;
        sp(o);
        if (o->err || o->i >= o->n)
            return v;
        c = o->s[o->i];
        if (c != '+' && c != '-')
            return v;
        o->i++;
        {
            double r = o_product(o);
            v = c == '+' ? v + r : v - r;
        }
    }
}

static int oracle(const char* s, const double* vars, double* out, char* names, int* nn)
{
    O o;
    memset(&o, 0, sizeof(o));
    o.s = s;
    o.n = strlen(s);
    o.vars = vars;
    *out = o_sum(&o);
    sp(&o);
    if (o.err || o.i != o.n)
        return -1;
    memcpy(names, o.names, 3);
    *nn = o.unk ? -1 : o.nnames;
    return 0;
}

static int compile(const char* s, expr_t** pe)
{
    ex_parse_t p;
    expr_t* e = (expr_t*)calloc(1, sizeof(*e));
    memset(&p, 0, sizeof(p));
    p.num = js_num;
    p.s = s;
    p.n = strlen(s);
    p.e = e;
    if (exprk_compile(&p) < 0) {
        exprk_free(e);
        *pe = NULL;
        return -1;
    }
    *pe = e;
    return 0;
}

static int run(const expr_t* e, const double* xyz, double* out)
{
    double vals[8], *stk;
    uint32_t i;
    int r;
    for (i = 0; i < e->nname; i++)
        vals[i] = xyz[e->names[i][0] - 'x'];
    stk = (double*)malloc((e->max_depth ? e->max_depth : 1) * sizeof(double));
    r = exprk_run(e, vals, stk, out);
    free(stk);
    return r;
}

static void gen(char* o, int depth)
{
    static const char* const F1[] = { "sin", "sqrt", "cbrt", "abs", "floor", "sign", "log1p", "exp", "round", "tanh" };
    static const char* const F2[] = { "atan2", "pow", "hypot", "min", "max", "fmod" };
    static const char* const OPS[] = { "+", "-", "*", "/", "%", "^" };
    unsigned r = depth > 4 ? rnd(3) : rnd(9);
    const char* ws = rnd(3) ? "" : " ";
    switch (r) {
    case 0:
        sprintf(o, "%s%u%s", ws, rnd(20), ws);
        break;
    case 1:
        sprintf(o, "%s", (const char* const[]) { "x", "y", "z", "pi", "e", "2.5", ".5", "1e2", "3E-1", "0.25e+1", "7." }[rnd(11)]);
        break;
    case 2:
        sprintf(o, "%s%s", rnd(2) ? "-" : "+", (const char* const[]) { "x", "y", "3", "z" }[rnd(4)]);
        break;
    case 3:
    case 4:
    case 5: {
        char a[2000], b[2000];
        gen(a, depth + 1);
        gen(b, depth + 1);
        sprintf(o, "%s%s%s%s%s", a, ws, OPS[rnd(6)], rnd(4) ? "" : "-", b);
        break;
    }
    case 6: {
        char a[2000];
        gen(a, depth + 1);
        sprintf(o, "(%s)", a);
        break;
    }
    case 7: {
        char a[2000];
        gen(a, depth + 1);
        sprintf(o, "%s(%s)", F1[rnd(10)], a);
        break;
    }
    default: {
        char a[2000], b[2000];
        gen(a, depth + 1);
        gen(b, depth + 1);
        sprintf(o, "%s( %s ,%s)", F2[rnd(6)], a, b);
        break;
    }
    }
}

static int same(double a, double b) { return (isnan(a) && isnan(b)) || memcmp(&a, &b, sizeof(a)) == 0 || (a == 0 && b == 0); }

int main(void)
{
    static char s[20000];
    int k;
    for (k = 0; k < 40000; k++) {
        double xyz[3] = { (double)(int)rnd(2001) / 100.0 - 10.0, (double)rnd(7), -(double)rnd(5) / 4.0 }, got, want;
        char names[3];
        int nn, w;
        expr_t* e;
        gen(s, 0);
        w = oracle(s, xyz, &want, names, &nn);
        CHECK(w == 0, "oracle rejects generated '%s'", s);
        CHECK(compile(s, &e) == 0, "compile '%s'", s);
        if (!e || w)
            continue;
        CHECK((int)e->nname == nn, "var count '%s'", s);
        for (int i = 0; i < nn && i < (int)e->nname; i++)
            CHECK(e->names[i][0] == names[i] && e->names[i][1] == 0, "var order '%s'", s);
        CHECK(run(e, xyz, &got) == 0 && same(got, want), "eval '%s' x=%g y=%g z=%g got %.17g want %.17g", s, xyz[0], xyz[1], xyz[2], got, want);
        exprk_free(e);
    }
    for (k = 0; k < 60000; k++) {
        static const char AL[] = "0123456789.eE+-*/%^() ,xyzpisqrtw";
        double xyz[3] = { 1.5, 2, -3 }, want, got;
        char names[3];
        int nn, w, c, m = (int)rnd(3) + 1, j;
        expr_t* e;
        size_t n;
        gen(s, 2);
        n = strlen(s);
        for (j = 0; j < m; j++) {
            size_t at = rnd((unsigned)n + 1);
            switch (rnd(3)) {
            case 0:
                if (at < n)
                    s[at] = AL[rnd(sizeof(AL) - 1)];
                break;
            case 1:
                memmove(s + at + 1, s + at, n - at + 1);
                s[at] = AL[rnd(sizeof(AL) - 1)];
                n++;
                break;
            default:
                if (at < n) {
                    memmove(s + at, s + at + 1, n - at);
                    n--;
                }
            }
        }
        w = oracle(s, xyz, &want, names, &nn);
        c = compile(s, &e);
        CHECK((w == 0) == (c == 0), "accept '%s' got %d want %d", s, c, w);
        if (c == 0 && w == 0 && nn == (int)e->nname)
            CHECK(run(e, xyz, &got) == 0 && same(got, want), "eval mutated '%s'", s);
        if (e)
            exprk_free(e);
    }
    {
        static const char* const OK[] = { "-2^2", "2^-1", "2^3^2", "--2", "+-+2", "-2*3", "2*-3^2", "max(1,2)", "1e3", "x", " pi ", "e", "e^2" };
        static const double V[] = { -4, 0.5, 512, 2, -2, -6, -18, 2, 1000, 1.5, 3.14159265358979323846, 2.71828182845904523536, 7.3890560989306495 };
        static const char* const BAD[] = { "", "1 2", "(", ")", "1+", "*1", "f(1)", "sin(1,2)", "atan2(1)", "1e", "1.2.3", "2pi", "()", "max(,1)", "1,2", "sin 1", "(1)(2)", "e(1)", "1e+", "#" };
        size_t i;
        double xyz[3] = { 1.5, 0, 0 }, got;
        for (i = 0; i < sizeof(OK) / sizeof(OK[0]); i++) {
            expr_t* e;
            CHECK(compile(OK[i], &e) == 0 && run(e, xyz, &got) == 0 && fabs(got - V[i]) <= 1e-15 * fabs(V[i]), "fixed '%s' got %.17g", OK[i], got);
            if (e)
                exprk_free(e);
        }
        for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
            expr_t* e;
            CHECK(compile(BAD[i], &e) != 0, "reject '%s'", BAD[i]);
            if (e)
                exprk_free(e);
        }
    }
    {
        expr_t* e;
        char* p = s;
        int i;
        for (i = 0; i < 2048; i++)
            p += sprintf(p, i ? "+1" : "1");
        CHECK(compile(s, &e) == 0, "4095 instructions compile");
        if (e)
            exprk_free(e);
        sprintf(p, "+1");
        CHECK(compile(s, &e) != 0, "4097 instructions refused");
        if (e)
            exprk_free(e);
        p = s;
        for (i = 0; i < 200; i++)
            *p++ = '(';
        *p++ = 'x';
        for (i = 0; i < 200; i++)
            *p++ = ')';
        *p = 0;
        CHECK(compile(s, &e) == 0, "200 nested parentheses");
        if (e)
            exprk_free(e);
        p = s;
        for (i = 0; i < 300; i++)
            *p++ = '(';
        *p++ = '1';
        for (i = 0; i < 300; i++)
            *p++ = ')';
        *p = 0;
        CHECK(compile(s, &e) != 0, "300 nested parentheses refused");
        if (e)
            exprk_free(e);
    }
    {
        expr_t* e;
        int k3;
        CHECK(compile("cbrt(x)", &e) == 0, "cbrt compile");
        for (k3 = 1; e && k3 <= 2000; k3++) {
            double xyz[3] = { (double)k3 * k3 * k3, 0, 0 }, got;
            CHECK(run(e, xyz, &got) == 0 && got == (double)k3, "cbrt(%d^3) exact got %.17g", k3, got);
        }
        if (e)
            exprk_free(e);
    }
    printf("%s test_exprk_core: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
