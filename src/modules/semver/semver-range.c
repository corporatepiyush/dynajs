#include "semver-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

typedef struct {
    Range rg;
    char* source;
} dyn_range_t;

static void dyn_range_dispose(void* native)
{
    dyn_range_t* r = (dyn_range_t*)native;
    if (!r)
        return;
    sv_range_release(&r->rg);
    free(r->source);
    free(r);
}

void dyn_range_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_range_dispose(JS_GetOpaque(val, dyn_range_class_id));
}

JSValue dyn_range_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    dyn_range_t* r;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Range(rangeString) requires a range");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    r = (dyn_range_t*)malloc(sizeof(*r));
    if (!r) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->source = (char*)malloc(n + 1);
    if (!r->source) {
        free(r);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(r->source, s, n);
    r->source[n] = '\0';
    JS_FreeCString(ctx, s);

    if (semverk_parse_range(r->source, n, &r->rg)) {
        JSValue e = JS_ThrowTypeError(ctx, "dyna:semver: invalid range \"%.*s\"",
            (int)(n > 64 ? 64 : n), r->source);
        free(r->source);
        free(r);
        return e;
    }

    return dyn_plain_wrap(ctx, new_target, dyn_range_class_id, r, dyn_range_dispose);
}

JSValue dyn_range_test(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* vs;
    size_t vn;
    SemVer v;
    dyn_range_t* r;
    int ok;

    (void)argc;
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }
    ok = semver_parse(vs, vn, &v) == 0 && semverk_range_test(&v, &r->rg);
    JS_FreeCString(ctx, vs);
    return JS_NewBool(ctx, ok);
}

JSValue dyn_range_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max)
{
    JSValue *elems = NULL, out;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    dyn_range_t* r;
    uint32_t count, i;
    long best = -1;

    (void)argc;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) {
        if (!semverk_range_test(&svs[i], &r->rg))
            continue;
        if (best < 0) {
            best = (long)i;
            continue;
        }
        {
            int c = semver_compare(&svs[i], &svs[best]);
            if (want_max ? (c > 0) : (c < 0))
                best = (long)i;
        }
    }
    out = (best < 0) ? JS_NULL : JS_DupValue(ctx, elems[best]);
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

JSValue dyn_range_filter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue *elems = NULL, out;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    dyn_range_t* r;
    uint32_t count, i, o = 0;

    (void)argc;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) {
        if (!semverk_range_test(&svs[i], &r->rg))
            continue;
        if (JS_DefinePropertyValueUint32(ctx, out, o++,
                JS_DupValue(ctx, elems[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
    }
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

JSValue dyn_range_get_source(JSContext* ctx, JSValueConst this_val)
{
    dyn_range_t* r = (dyn_range_t*)dyn_plain_get(ctx, this_val,
        dyn_range_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewString(ctx, r->source);
}

int sv_range_arg(JSContext* ctx, JSValueConst v, const Range** out,
    const char** powned, int* powned_flag, Range** pheap)
{
    const char* s;
    size_t n;
    dyn_range_t* r;

    *pheap = NULL;
    if (JS_IsString(v)) {
        Range* tmp;
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        tmp = (Range*)malloc(sizeof(*tmp));
        if (!tmp) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (semverk_parse_range(s, n, tmp)) {
            JS_FreeCString(ctx, s);
            free(tmp);
            JS_ThrowTypeError(ctx, "dyna:semver: invalid range");
            return -1;
        }
        *out = tmp;
        *pheap = tmp;
        *powned = s;
        *powned_flag = 1;
        return 0;
    }
    r = (dyn_range_t*)JS_GetOpaque(v, dyn_range_class_id);
    if (r) {
        *out = &r->rg;
        *powned = NULL;
        *powned_flag = 0;
        return 0;
    }
    JS_ThrowTypeError(ctx,
        "dyna:semver: expected a range string or a Range");
    return -1;
}

JSValue dyn_range_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const Range* rb;
    Range* hb;
    const char* own_b;
    int flag_b, res;
    dyn_range_t* r;

    if (sv_range_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            &rb, &own_b, &flag_b, &hb))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    res = r && sv_range_intersects(&r->rg, rb);
    if (flag_b)
        JS_FreeCString(ctx, own_b);
    sv_range_free(hb);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, res);
}

JSValue dyn_range_get_set_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_range_t* r = (dyn_range_t*)dyn_plain_get(ctx, this_val,
        dyn_range_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, r->rg.n);
}

#endif
