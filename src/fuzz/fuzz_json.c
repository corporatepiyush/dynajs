#include "dynajs.h"
#include "src/fuzz/fuzz_common.h"

#define FZ_COVER_ENGINE 1

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fz_env_t env;

typedef struct {
    char* b;
    size_t n, cap;
} sb_t;

static void sb_put(sb_t* s, const char* p, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 256;
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

static void sb_str(sb_t* s, const char* p) { sb_put(s, p, strlen(p)); }

static const char* const num_forms[] = { "0", "-0", "1", "-1", "9007199254740993", "1e308", "1.7976931348623157e308", "2e308",
    "5e-324", "2.4703282292062328e-324", "4.9406564584124654e-324", "0.1", "1e-7", "123456789012345678901234567890",
    "1E+2", "1e-0", "0.30000000000000004", "2.2250738585072011e-308", "-1.5e-10", "100e-2" };

static void gen_number(fz_in_t* in, sb_t* s)
{
    unsigned k = fz_below(in, 4), i, d;
    char t[8];
    if (k == 0) {
        sb_str(s, num_forms[fz_below(in, sizeof(num_forms) / sizeof(num_forms[0]))]);
        return;
    }
    if (fz_u8(in) & 1)
        sb_str(s, "-");
    d = 1 + fz_below(in, 24);
    t[1] = 0;
    for (i = 0; i < d; i++) {
        t[0] = (char)('0' + fz_below(in, 10));
        if (i == 0 && d > 1 && t[0] == '0')
            t[0] = '1';
        sb_str(s, t);
    }
    if (k >= 2) {
        sb_str(s, ".");
        d = 1 + fz_below(in, 20);
        for (i = 0; i < d; i++) {
            t[0] = (char)('0' + fz_below(in, 10));
            sb_str(s, t);
        }
    }
    if (k == 3 || (fz_u8(in) & 3) == 0) {
        static const char* const ex[] = { "e", "E", "e+", "e-", "E-" };
        sb_str(s, ex[fz_below(in, 5)]);
        snprintf(t, sizeof(t), "%u", fz_below(in, 400));
        sb_str(s, t);
    }
}

static void gen_string(fz_in_t* in, sb_t* s)
{
    static const char* const esc[] = { "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t", "\\u0000", "\\u00e9", "\\ud83d\\ude00",
        "\\ud800", "\\udfff", "\\uFFFF", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "__proto__", "constructor", "0", "4294967295", " ", "a" };
    unsigned n = fz_below(in, 12), i;
    sb_str(s, "\"");
    for (i = 0; i < n; i++) {
        unsigned c = fz_u8(in);
        if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
            sb_str(s, esc[c % (sizeof(esc) / sizeof(esc[0]))]);
        } else {
            char t = (char)c;
            sb_put(s, &t, 1);
        }
    }
    sb_str(s, "\"");
}

static void gen_value(fz_in_t* in, sb_t* s, int depth)
{
    unsigned k = fz_below(in, depth > 40 ? 5 : 8), i, n;
    static const char* const ws[] = { "", " ", "\n", "\t", "\r\n  " };
    sb_str(s, ws[fz_below(in, 5)]);
    switch (k) {
    case 0:
        sb_str(s, "null");
        break;
    case 1:
        sb_str(s, fz_u8(in) & 1 ? "true" : "false");
        break;
    case 2:
    case 3:
        gen_number(in, s);
        break;
    case 4:
        gen_string(in, s);
        break;
    case 5:
    case 6:
        n = fz_below(in, 6);
        sb_str(s, "[");
        for (i = 0; i < n && in->n; i++) {
            if (i)
                sb_str(s, ",");
            gen_value(in, s, depth + 1);
        }
        sb_str(s, "]");
        break;
    default:
        n = fz_below(in, 6);
        sb_str(s, "{");
        for (i = 0; i < n && in->n; i++) {
            if (i)
                sb_str(s, ",");
            gen_string(in, s);
            sb_str(s, ws[fz_below(in, 5)]);
            sb_str(s, ":");
            gen_value(in, s, depth + 1);
        }
        sb_str(s, "}");
        break;
    }
}

static JSValue stringify(JSContext* ctx, JSValueConst v, int indent)
{
    JSValue sp = indent ? JS_NewInt32(ctx, indent) : JS_UNDEFINED;
    JSValue r = JS_JSONStringify(ctx, v, JS_UNDEFINED, sp);
    JS_FreeValue(ctx, sp);
    return r;
}

static int same_str(JSContext* ctx, JSValueConst a, JSValueConst b)
{
    size_t la, lb;
    const char *sa = JS_ToCStringLen(ctx, &la, a), *sb = JS_ToCStringLen(ctx, &lb, b);
    int r = sa && sb && la == lb && !memcmp(sa, sb, la);
    if (!sa || !sb)
        r = -1;
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return r;
}

static int budget(JSContext* ctx)
{
    JSValue x = JS_GetException(ctx);
    int b = fz_is_budget_error(ctx, x);
    JS_FreeValue(ctx, x);
    if (b)
        env.dirty = 1;
    return b;
}

static int is_json_number(const char* t, size_t n)
{
    size_t i = 0;
    if (i < n && t[i] == '-')
        i++;
    if (i >= n || t[i] < '0' || t[i] > '9')
        return 0;
    if (t[i] == '0')
        i++;
    else
        while (i < n && t[i] >= '0' && t[i] <= '9')
            i++;
    if (i < n && t[i] == '.') {
        if (++i >= n || t[i] < '0' || t[i] > '9')
            return 0;
        while (i < n && t[i] >= '0' && t[i] <= '9')
            i++;
    }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        i++;
        if (i < n && (t[i] == '+' || t[i] == '-'))
            i++;
        if (i >= n || t[i] < '0' || t[i] > '9')
            return 0;
        while (i < n && t[i] >= '0' && t[i] <= '9')
            i++;
    }
    return i == n;
}

static int deq(JSContext* ctx, JSValueConst a, JSValueConst b, int depth)
{
    int ta = JS_VALUE_GET_NORM_TAG(a), tb = JS_VALUE_GET_NORM_TAG(b);
    if (depth > 64)
        return 1;
    if (JS_IsNumber(a) && JS_IsNull(b)) {
        double x;
        JS_ToFloat64(ctx, &x, a);
        return !isfinite(x);
    }
    if (JS_IsNumber(a) && JS_IsNumber(b)) {
        double x, y;
        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        return x == y || (x != x && y != y);
    }
    if (ta != tb)
        return 0;
    if (JS_IsString(a))
        return same_str(ctx, a, b) != 0;
    if (!JS_IsObject(a))
        return JS_StrictEq(ctx, a, b);
    {
        JSPropertyEnum *pa, *pb;
        uint32_t na, nb, i;
        int r = 1;
        if (JS_IsArray(ctx, a) != JS_IsArray(ctx, b))
            return 0;
        if (JS_GetOwnPropertyNames(ctx, &pa, &na, a, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
        if (JS_GetOwnPropertyNames(ctx, &pb, &nb, b, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreePropertyEnum(ctx, pa, na);
            return -1;
        }
        r = na == nb;
        for (i = 0; r == 1 && i < na; i++) {
            JSValue va, vb;
            if (pa[i].atom != pb[i].atom) {
                r = 0;
                break;
            }
            va = JS_GetProperty(ctx, a, pa[i].atom);
            vb = JS_GetProperty(ctx, b, pb[i].atom);
            r = JS_IsException(va) || JS_IsException(vb) ? -1 : deq(ctx, va, vb, depth + 1);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
        }
        JS_FreePropertyEnum(ctx, pa, na);
        JS_FreePropertyEnum(ctx, pb, nb);
        return r;
    }
}

static void check_text(JSContext* ctx, const char* t, size_t n, int must_parse)
{
    JSValue a, s1, b, s2, e, se, p;
    double d, ref;
    a = JS_ParseJSON(ctx, t, n, "<json>");
    if (JS_IsException(a)) {
        JSValue x = JS_GetException(ctx);
        int bud = fz_is_budget_error(ctx, x);
        JSValue name = bud ? JS_UNDEFINED : JS_GetPropertyStr(ctx, x, "name");
        const char* nm = bud ? NULL : JS_ToCString(ctx, name);
        if (!bud && (!nm || strcmp(nm, "SyntaxError")))
            fz_fail("strict JSON.parse failed with an error other than SyntaxError");
        if (must_parse && !bud)
            fz_fail("strict JSON.parse rejected generated valid JSON");
        if (bud)
            env.dirty = 1;
        JS_FreeCString(ctx, nm);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, x);
        return;
    }
    if (is_json_number(t, n)) {
        char* end;
        ref = strtod(t, &end);
        if (JS_ToFloat64(ctx, &d, a) < 0 || memcmp(&d, &ref, sizeof(d)))
            fz_fail("JSON number differs from strtod");
    }
    s1 = stringify(ctx, a, 0);
    if (JS_IsException(s1)) {
        if (!budget(ctx))
            fz_fail("JSON.stringify threw on a parsed value");
        JS_FreeValue(ctx, a);
        return;
    }
    {
        size_t l1;
        const char* c1 = JS_ToCStringLen(ctx, &l1, s1);
        if (!c1)
            fz_fail("ToCString of stringify result");
        for (size_t k = 0; k < l1; k++) {
            uint8_t ch = (uint8_t)c1[k];
            if (ch < 0x20)
                fz_fail("JSON.stringify output holds a raw control character");
            if (ch == 0xed && k + 1 < l1 && (uint8_t)c1[k + 1] >= 0xa0)
                fz_fail("JSON.stringify output holds a lone surrogate");
        }
        b = JS_ParseJSON(ctx, c1, l1, "<json2>");
        JS_FreeCString(ctx, c1);
    }
    if (JS_IsException(b)) {
        if (!budget(ctx))
            fz_fail("JSON.parse rejected the output of JSON.stringify");
    } else {
        int dq = deq(ctx, a, b, 0);
        if (dq == 0)
            fz_fail("parse(stringify(v)) is not deep-equal to v");
        if (dq < 0)
            budget(ctx);
        s2 = stringify(ctx, b, 0);
        if (!JS_IsException(s2) && same_str(ctx, s1, s2) == 0)
            fz_fail("parse(stringify(parse(t))) is not equal to parse(t)");
        if (JS_IsException(s2))
            budget(ctx);
        JS_FreeValue(ctx, s2);
        p = stringify(ctx, b, 1 + (int)(n % 4));
        if (!JS_IsException(p)) {
            size_t lp;
            const char* cp = JS_ToCStringLen(ctx, &lp, p);
            if (!cp) {
                if (!budget(ctx))
                    fz_fail("ToCString of indented stringify result");
            } else {
                JSValue c = JS_ParseJSON(ctx, cp, lp, "<json3>");
                JSValue sc = JS_IsException(c) ? JS_EXCEPTION : stringify(ctx, c, 0);
                if (JS_IsException(c) || JS_IsException(sc)) {
                    if (!budget(ctx))
                        fz_fail("indented JSON.stringify output does not parse");
                } else if (same_str(ctx, s1, sc) == 0)
                    fz_fail("indented stringify round trip differs");
                JS_FreeValue(ctx, sc);
                JS_FreeValue(ctx, c);
                JS_FreeCString(ctx, cp);
            }
        } else
            budget(ctx);
        JS_FreeValue(ctx, p);
    }
    JS_FreeValue(ctx, b);
    e = JS_ParseJSON2(ctx, t, n, "<json-ext>", JS_PARSE_JSON_EXT);
    if (JS_IsException(e)) {
        if (!budget(ctx))
            fz_fail("JSON_EXT rejected text that strict JSON accepts");
    } else {
        int dqe = deq(ctx, a, e, 0);
        if (dqe == 0)
            fz_fail("JSON_EXT value is not deep-equal to the strict JSON value");
        if (dqe < 0)
            budget(ctx);
        se = stringify(ctx, e, 0);
        if (!JS_IsException(se) && same_str(ctx, s1, se) == 0)
            fz_fail("JSON_EXT and strict JSON parse the same text differently");
        if (JS_IsException(se))
            budget(ctx);
        JS_FreeValue(ctx, se);
    }
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, s1);
    JS_FreeValue(ctx, a);
}

static void run(fz_env_t* e, const uint8_t* data, size_t size)
{
    fz_in_t in = { data, size };
    sb_t s = { 0 };
    JSContext* ctx = e->ctx[0];
    unsigned mode;
    fz_arm(500000000u, 0);
    mode = fz_u8(&in) % 4;
    if (mode == 0) {
        sb_put(&s, (const char*)in.p, in.n);
        check_text(ctx, s.b, s.n, 0);
        JSValue e = JS_ParseJSON2(ctx, s.b, s.n, "<json-ext>", JS_PARSE_JSON_EXT);
        if (JS_IsException(e))
            budget(ctx);
        JS_FreeValue(ctx, e);
    } else if (mode == 1) {
        gen_number(&in, &s);
        check_text(ctx, s.b, s.n, 1);
    } else {
        gen_value(&in, &s, 0);
        check_text(ctx, s.b, s.n, 1);
    }
    free(s.b);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size && size <= 65536)
        fz_one(&env, data, size);
    return 0;
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    fz_args(*argc, *argv);
    fz_env_open(&env, 1, 256, NULL, run);
    return 0;
}
