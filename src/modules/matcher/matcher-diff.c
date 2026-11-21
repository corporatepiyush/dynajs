#include "matcher-internal.h"
#include "cutils.h"
#include "core/dyn-textdiff.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

static JSValue dyn_hunk_value(JSContext* ctx, const dyn_td_hunk_t* h,
    const dyn_td_toks_t* ta, const uint8_t* sa,
    const dyn_td_toks_t* tb, const uint8_t* sb)
{
    JSValue o = JS_NewObject(ctx);
    const uint8_t* src;
    uint32_t lo, hi, start, end;
    if (JS_IsException(o))
        return o;
    if (h->op == DYN_TD_INS) {
        src = sb;
        lo = h->b0;
        hi = h->b1;
    } else {
        src = sa;
        lo = h->a0;
        hi = h->a1;
    }
    {
        const dyn_td_toks_t* t = (h->op == DYN_TD_INS) ? tb : ta;
        start = t->off[lo];
        end = t->off[hi - 1] + t->len[hi - 1];
    }
    if (JS_DefinePropertyValueStr(ctx, o, "op", JS_NewInt32(ctx, h->op),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, o, "text",
               JS_NewStringLen(ctx, (const char*)src + start,
                   end - start),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int dyn_td_throw(JSContext* ctx, const char* name, int rc)
{
    if (rc == DYN_TD_ETOKENS)
        JS_ThrowRangeError(ctx, "%s(a, b): more than %u tokens per side",
            name, DYN_TD_MAX_TOKENS);
    else if (rc == DYN_TD_ECOSTLY)
        JS_ThrowRangeError(ctx,
            "%s(a, b): inputs too dissimilar (edit distance exceeds cap); "
            "shorten or align the input",
            name);
    else
        JS_ThrowOutOfMemory(ctx);
    return -1;
}

static int dyn_diff_meta(JSContext* ctx, JSValueConst o, const char* name, uint64_t* ceil)
{
    static const char* const meta_keys[] = { "maxSteps" };
    JSValue mv, v;
    double d;
    *ceil = 0;
    mv = JS_GetPropertyStr(ctx, o, "dyna");
    if (JS_IsException(mv))
        return -1;
    if (JS_IsUndefined(mv))
        return 0;
    if (!JS_IsObject(mv) || JS_IsFunction(ctx, mv)) {
        JS_FreeValue(ctx, mv);
        JS_ThrowTypeError(ctx, "%s: dyna must be an object", name);
        return -1;
    }
    if (dyn_opts_strict(ctx, mv, meta_keys, 1)) {
        JS_FreeValue(ctx, mv);
        return -1;
    }
    v = JS_GetPropertyStr(ctx, mv, "maxSteps");
    JS_FreeValue(ctx, mv);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: dyna.maxSteps must be a number", name);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    if (isinf(d) && d > 0) {
        *ceil = UINT64_MAX;
        return 0;
    }
    if (!(d >= 2147483648.0 && d <= 9007199254740991.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s: dyna.maxSteps only raises the step budget: an integer in [2^31, 2^53-1] or Infinity", name);
        return -1;
    }
    *ceil = (uint64_t)d;
    return 0;
}

static int dyn_diff_opts(JSContext* ctx, int argc, JSValueConst* argv, const char* name, uint64_t* ceil)
{
    static const char* const keys[] = { "dyna" };
    *ceil = 0;
    if (argc < 3 || JS_IsUndefined(argv[2]))
        return 0;
    if (!JS_IsObject(argv[2]) || JS_IsFunction(ctx, argv[2])) {
        JS_ThrowTypeError(ctx, "%s(a, b, opts): opts must be an object", name);
        return -1;
    }
    if (dyn_opts_strict(ctx, argv[2], keys, 1))
        return -1;
    return dyn_diff_meta(ctx, argv[2], name, ceil);
}

static int dyn_diff_compute(JSContext* ctx, const char* name, const uint8_t* a,
    uint32_t na, const uint8_t* b, uint32_t nb, int mode, uint64_t ceil, dyn_td_res_t* r)
{
    int rc = dyn_td_diff_ex(a, na, b, nb, mode, ceil, r);
    return rc == DYN_TD_OK ? 0 : dyn_td_throw(ctx, name, rc);
}

JSValue dyn_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_operand_t A, B;
    dyn_td_res_t res;
    JSValue ret = JS_EXCEPTION, arr;
    uint32_t i;
    uint64_t ceil;
    static const char* const NAMES[3] = { "DiffLines", "DiffWords", "DiffChars" };

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    memset(&res, 0, sizeof res);

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(a, b): two strings required", NAMES[magic]);
    if (dyn_diff_opts(ctx, argc, argv, NAMES[magic], &ceil) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;

#ifndef DYN_NO_BYPASS
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            goto done;
        if (A.bytes) {
            JSValue h = JS_NewObject(ctx);
            if (JS_IsException(h)) {
                JS_FreeValue(ctx, arr);
                goto done;
            }
            if (JS_DefinePropertyValueStr(ctx, h, "op", JS_NewInt32(ctx, 0),
                    JS_PROP_C_W_E)
                    < 0
                || JS_DefinePropertyValueStr(ctx, h, "text",
                       JS_NewStringLen(ctx, A.cs, A.bytes),
                       JS_PROP_C_W_E)
                    < 0) {
                JS_FreeValue(ctx, h);
                JS_FreeValue(ctx, arr);
                goto done;
            }
            if (JS_DefinePropertyValueUint32(ctx, arr, 0, h, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                goto done;
            }
        }
        ret = arr;
        goto done;
    }
#endif
    if (dyn_diff_compute(ctx, NAMES[magic], (const uint8_t*)A.cs, (uint32_t)A.bytes,
            (const uint8_t*)B.cs, (uint32_t)B.bytes, magic, ceil, &res)
        < 0)
        goto done;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto done;
    for (i = 0; i < res.sc.n; i++) {
        JSValue h = dyn_hunk_value(ctx, &res.sc.v[i], &res.ta, (const uint8_t*)A.cs,
            &res.tb, (const uint8_t*)B.cs);
        if (JS_IsException(h)
            || JS_DefinePropertyValueUint32(ctx, arr, i, h, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            goto done;
        }
    }
    ret = arr;
done:
    dyn_td_res_free(&res);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}

static int dyn_ud_opt_name(JSContext* ctx, JSValueConst opts, const char* key,
    const char* def, const char** out, int* owned)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    size_t n;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        *out = def;
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "UnifiedDiff: %s must be a string", key);
        return -1;
    }
    *out = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!*out)
        return -1;
    *owned = 1;
    if (memchr(*out, '\n', n) || memchr(*out, '\r', n) || strlen(*out) != n) {
        JS_FreeCString(ctx, *out);
        *out = NULL;
        *owned = 0;
        JS_ThrowRangeError(ctx, "UnifiedDiff: %s must not contain a line break or NUL", key);
        return -1;
    }
    return 0;
}

JSValue dyn_unified_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const KEYS[4] = { "context", "aName", "bName", "dyna" };
    uint64_t ceil = 0;
    dyn_operand_t A, B;
    dyn_td_res_t res;
    const char *an = NULL, *bn = NULL;
    uint32_t ctxl = 3;
    int an_owned = 0, bn_owned = 0;
    JSValue ret = JS_EXCEPTION, opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    dyn_td_buf_t o;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    memset(&res, 0, sizeof res);
    memset(&o, 0, sizeof o);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "UnifiedDiff(a, b, opts?): two strings required");
    if (!JS_IsUndefined(opts)) {
        JSPropertyEnum* props;
        uint32_t np, k;
        if (!JS_IsObject(opts))
            return JS_ThrowTypeError(ctx, "UnifiedDiff: options must be an object");
        if (JS_GetOwnPropertyNames(ctx, &props, &np, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return JS_EXCEPTION;
        for (k = 0; k < np; k++) {
            const char* nm = JS_AtomToCString(ctx, props[k].atom);
            int known = 0, j;
            if (!nm) {
                JS_FreePropertyEnum(ctx, props, np);
                return JS_EXCEPTION;
            }
            for (j = 0; j < 4; j++)
                known |= !strcmp(nm, KEYS[j]);
            if (!known) {
                JS_ThrowTypeError(ctx, "UnifiedDiff: unknown option \"%s\" (valid: context, aName, bName, dyna)", nm);
                JS_FreeCString(ctx, nm);
                JS_FreePropertyEnum(ctx, props, np);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, nm);
        }
        JS_FreePropertyEnum(ctx, props, np);
        {
            JSValue c = JS_GetPropertyStr(ctx, opts, "context");
            double d;
            if (JS_IsException(c))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(c)) {
                if (!JS_IsNumber(c)) {
                    JS_FreeValue(ctx, c);
                    return JS_ThrowTypeError(ctx, "UnifiedDiff: context must be a number");
                }
                JS_ToFloat64(ctx, &d, c);
                if (!(d >= 0 && d <= 1073741824.0) || d != floor(d))
                    return JS_ThrowRangeError(ctx, "UnifiedDiff: context must be an integer in [0, 2^30]");
                ctxl = (uint32_t)d;
            }
        }
        if (dyn_diff_meta(ctx, opts, "UnifiedDiff", &ceil) < 0)
            goto done;
        if (dyn_ud_opt_name(ctx, opts, "aName", "a", &an, &an_owned) < 0)
            goto done;
        if (dyn_ud_opt_name(ctx, opts, "bName", "b", &bn, &bn_owned) < 0)
            goto done;
    } else {
        an = "a";
        bn = "b";
    }
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        goto done;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        ret = JS_NewString(ctx, "");
        goto done;
    }
    if (dyn_diff_compute(ctx, "UnifiedDiff", (const uint8_t*)A.cs, (uint32_t)A.bytes,
            (const uint8_t*)B.cs, (uint32_t)B.bytes, DYN_TD_LINES, ceil, &res)
        < 0)
        goto done;
    if (dyn_td_unified((const uint8_t*)A.cs, (const uint8_t*)B.cs, &res, ctxl, an, bn, &o) < 0) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = JS_NewStringLen(ctx, (const char*)o.buf, o.size);
done:
    dyn_td_buf_free(&o);
    dyn_td_res_free(&res);
    if (an_owned)
        JS_FreeCString(ctx, an);
    if (bn_owned)
        JS_FreeCString(ctx, bn);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}

static JSValue dyn_patch_plain_error(JSContext* ctx, const char* msg)
{
    JSValue err = JS_NewError(ctx);
    if (JS_IsException(err))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return JS_Throw(ctx, err);
}

JSValue dyn_apply_patch(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t T, P;
    dyn_td_buf_t o;
    JSValue ret = JS_EXCEPTION;
    char msg[DYN_TD_MSG_CAP];
    int rc;

    T.cs = NULL;
    T.cp = NULL;
    P.cs = NULL;
    P.cp = NULL;
    memset(&o, 0, sizeof o);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ApplyPatch(text, patch): two strings required");
    if (dyn_operand_get(ctx, argv[0], "text", &T) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "patch", &P) < 0)
        goto done;
    rc = dyn_td_apply((const uint8_t*)T.cs, (uint32_t)T.bytes, (const uint8_t*)P.cs, P.bytes, &o, msg);
    if (rc == DYN_TD_ETOKENS)
        JS_ThrowRangeError(ctx, "ApplyPatch: text has more than %u lines", DYN_TD_MAX_TOKENS);
    else if (rc == DYN_TD_ESYNTAX)
        JS_ThrowSyntaxError(ctx, "%s", msg);
    else if (rc == DYN_TD_ECONFLICT)
        dyn_patch_plain_error(ctx, msg);
    else if (rc < 0)
        JS_ThrowOutOfMemory(ctx);
    else
        ret = JS_NewStringLen(ctx, (const char*)o.buf, o.size);
done:
    dyn_td_buf_free(&o);
    dyn_operand_free(ctx, &T);
    dyn_operand_free(ctx, &P);
    return ret;
}

#endif
