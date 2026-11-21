#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#define DYN_SC_MAX_EQUAL_DEPTH 128
#define DYN_SC_MAX_EVAL 4096
#define DYN_SC_CACHE_ATOM_NAME "__dyna_compiled_schema"
#define DYN_SC_CACHE_HASH_NAME "__dyna_compiled_hash"

int dyn_sc_equal(JSContext* ctx, JSValueConst a, JSValueConst b,
    int depth, dyn_sc_pair_t* seen, int nseen)
{
    int i;

    if (depth > DYN_SC_MAX_EQUAL_DEPTH)
        return JS_StrictEq(ctx, a, b);

    if (JS_IsObject(a) && JS_IsObject(b) && !JS_IsFunction(ctx, a) && !JS_IsFunction(ctx, b)) {
        for (i = 0; i < nseen; i++) {
            if (JS_VALUE_GET_PTR(seen[i].a) == JS_VALUE_GET_PTR(a) && JS_VALUE_GET_PTR(seen[i].b) == JS_VALUE_GET_PTR(b))
                return 1;
        }
        if (nseen < DYN_SC_MAX_EQUAL_DEPTH) {
            seen[nseen].a = a;
            seen[nseen].b = b;
            nseen++;
        }
    }

    if (dyn_sc_is_array(ctx, a) || dyn_sc_is_array(ctx, b)) {
        uint64_t la, lb, i2;
        if (dyn_sc_is_array(ctx, a) != 1 || dyn_sc_is_array(ctx, b) != 1)
            return 0;
        la = dyn_sc_arr_len(ctx, a);
        lb = dyn_sc_arr_len(ctx, b);
        if (la == (uint64_t)-1 || lb == (uint64_t)-1)
            return -1;
        if (la != lb)
            return 0;
        for (i2 = 0; i2 < la; i2++) {
            JSValue ea = JS_GetPropertyUint32(ctx, a, (uint32_t)i2);
            JSValue eb = JS_GetPropertyUint32(ctx, b, (uint32_t)i2);
            int r;
            if (JS_IsException(ea) || JS_IsException(eb)) {
                JS_FreeValue(ctx, ea);
                JS_FreeValue(ctx, eb);
                return -1;
            }
            r = dyn_sc_equal(ctx, ea, eb, depth + 1, seen, nseen);
            JS_FreeValue(ctx, ea);
            JS_FreeValue(ctx, eb);
            if (r < 0)
                return -1;
            if (r == 0)
                return 0;
        }
        return 1;
    }
    if (JS_IsObject(a) || JS_IsObject(b)) {
        JSPropertyEnum *pa = NULL, *pb = NULL;
        uint32_t na, nb, k;
        if (!JS_IsObject(a) || !JS_IsObject(b))
            return 0;
        if (JS_IsFunction(ctx, a) || JS_IsFunction(ctx, b))
            return JS_StrictEq(ctx, a, b);
        if (JS_GetOwnPropertyNames(ctx, &pa, &na, a,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0)
            return -1;
        if (JS_GetOwnPropertyNames(ctx, &pb, &nb, b,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0) {
            JS_FreePropertyEnum(ctx, pa, na);
            return -1;
        }
        if (na != nb) {
            JS_FreePropertyEnum(ctx, pa, na);
            JS_FreePropertyEnum(ctx, pb, nb);
            return 0;
        }
        for (k = 0; k < na; k++) {
            JSValue va, vb;
            int found, r;
            found = dyn_sc_get_own(ctx, a, pa[k].atom, &va);
            if (found < 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (found == 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
            found = dyn_sc_get_own(ctx, b, pa[k].atom, &vb);
            if (found < 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (found == 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
            r = dyn_sc_equal(ctx, va, vb, depth + 1, seen, nseen);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
            if (r < 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (r == 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
        }
        JS_FreePropertyEnum(ctx, pa, na);
        JS_FreePropertyEnum(ctx, pb, nb);
        return 1;
    }
    return JS_StrictEq(ctx, a, b);
}

void dyn_sc_eval_free(JSContext* ctx, dyn_sceval_t* e)
{
    int i;
    for (i = 0; i < e->nprops; i++)
        js_free(ctx, e->props[i]);
    js_free(ctx, e->props);
    js_free(ctx, e->plens);
    js_free(ctx, e->items);
}

int dyn_sc_eval_add_prop(JSContext* ctx, dyn_sceval_t* e,
    const char* name, size_t len)
{
    char* p;
    if (!e)
        return 0;
    if (e->nprops >= DYN_SC_MAX_EVAL) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: over %d evaluated properties tracked by "
            "unevaluated* validation",
            DYN_SC_MAX_EVAL);
        return -1;
    }
    if (e->nprops >= e->cprops) {
        int ncap = e->cprops ? e->cprops * 2 : 8;
        char** np;
        int* nl;
        np = js_realloc(ctx, e->props, (size_t)ncap * sizeof(*np));
        if (!np)
            return -1;
        e->props = np;
        nl = js_realloc(ctx, e->plens, (size_t)ncap * sizeof(*nl));
        if (!nl)
            return -1;
        e->plens = nl;
        e->cprops = ncap;
    }
    p = js_malloc(ctx, len + 1);
    if (!p)
        return -1;
    memcpy(p, name, len);
    p[len] = '\0';
    e->props[e->nprops] = p;
    e->plens[e->nprops] = (int)len;
    e->nprops++;
    return 0;
}

int dyn_sc_eval_has_prop(const dyn_sceval_t* e, const char* name,
    size_t len)
{
    int i;
    if (!e)
        return 0;
    for (i = 0; i < e->nprops; i++)
        if ((size_t)e->plens[i] == len && memcmp(e->props[i], name, len) == 0)
            return 1;
    return 0;
}

int dyn_sc_eval_add_item(JSContext* ctx, dyn_sceval_t* e, uint64_t idx)
{
    int i;
    if (!e)
        return 0;
    for (i = 0; i < e->nitems; i++)
        if (e->items[i] == idx)
            return 0;
    if (e->nitems >= DYN_SC_MAX_EVAL) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: over %d evaluated items tracked by unevaluated* "
            "validation",
            DYN_SC_MAX_EVAL);
        return -1;
    }
    if (e->nitems >= e->citems) {
        int ncap = e->citems ? e->citems * 2 : 8;
        uint64_t* ni = js_realloc(ctx, e->items, (size_t)ncap * sizeof(*ni));
        if (!ni)
            return -1;
        e->items = ni;
        e->citems = ncap;
    }
    e->items[e->nitems++] = idx;
    return 0;
}

int dyn_sc_eval_has_item(const dyn_sceval_t* e, uint64_t idx)
{
    int i;
    if (!e)
        return 0;
    for (i = 0; i < e->nitems; i++)
        if (e->items[i] == idx)
            return 1;
    return 0;
}

int dyn_sc_eval_merge(JSContext* ctx, dyn_sceval_t* dst,
    dyn_sceval_t* src)
{
    if (src->nprops) {
        if (dst->nprops + src->nprops > DYN_SC_MAX_EVAL || src->nprops > DYN_SC_MAX_EVAL - dst->nprops) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: over %d evaluated properties tracked by "
                "unevaluated* validation",
                DYN_SC_MAX_EVAL);
            return -1;
        }
        if (dst->nprops + src->nprops > dst->cprops) {
            int ncap = dst->cprops ? dst->cprops : 8;
            char** np;
            int* nl;
            while (ncap < dst->nprops + src->nprops)
                ncap *= 2;
            np = js_realloc(ctx, dst->props, (size_t)ncap * sizeof(*np));
            if (!np)
                return -1;
            dst->props = np;
            nl = js_realloc(ctx, dst->plens, (size_t)ncap * sizeof(*nl));
            if (!nl)
                return -1;
            dst->plens = nl;
            dst->cprops = ncap;
        }
        memcpy(dst->props + dst->nprops, src->props,
            (size_t)src->nprops * sizeof(char*));
        memcpy(dst->plens + dst->nprops, src->plens,
            (size_t)src->nprops * sizeof(int));
        dst->nprops += src->nprops;
        src->nprops = 0;
    }
    if (src->nitems) {
        if (src->nitems > DYN_SC_MAX_EVAL - dst->nitems) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: over %d evaluated items tracked by "
                "unevaluated* validation",
                DYN_SC_MAX_EVAL);
            return -1;
        }
        if (dst->nitems + src->nitems > dst->citems) {
            int ncap = dst->citems ? dst->citems : 8;
            uint64_t* ni;
            while (ncap < dst->nitems + src->nitems)
                ncap *= 2;
            ni = js_realloc(ctx, dst->items, (size_t)ncap * sizeof(*ni));
            if (!ni)
                return -1;
            dst->items = ni;
            dst->citems = ncap;
        }
        memcpy(dst->items + dst->nitems, src->items,
            (size_t)src->nitems * sizeof(uint64_t));
        dst->nitems += src->nitems;
        src->nitems = 0;
    }
    return 0;
}

static JSValue dyn_sc_result(JSContext* ctx, dyn_scerrs_t* errs, int r)
{
    JSValue obj, arr;
    int i;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    for (i = 0; i < errs->n; i++) {
        JSValue eo, ep, em, ek;
        eo = JS_NewObject(ctx);
        ep = JS_NewStringLen(ctx, errs->e[i].path,
            (size_t)errs->e[i].pathlen);
        em = JS_NewString(ctx, errs->e[i].message);
        ek = JS_NewString(ctx, errs->e[i].keyword);
        if (JS_IsException(eo)) {
            JS_FreeValue(ctx, ep);
            JS_FreeValue(ctx, em);
            JS_FreeValue(ctx, ek);
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (JS_IsException(ep) || JS_IsException(em) || JS_IsException(ek)) {
            JS_FreeValue(ctx, eo);
            JS_FreeValue(ctx, ep);
            JS_FreeValue(ctx, em);
            JS_FreeValue(ctx, ek);
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueStr(ctx, eo, "path", ep, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, eo, "message", em, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, eo, "keyword", ek, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, eo, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    JS_DefinePropertyValueStr(ctx, obj, "valid",
        JS_NewBool(ctx, r == 1 && errs->n == 0 && errs->drops == 0),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "errors", arr, JS_PROP_C_W_E);
    return obj;
}

JSValue dyn_sc_validate_impl(JSContext* ctx, dyn_schema_t* sch,
    JSValueConst inst)
{
    dyn_vctx_t v;
    dyn_scerrs_t errs;
    JSValue res;
    int r;

    memset(&v, 0, sizeof v);
    v.ctx = ctx;
    v.sch = sch;
    memset(&errs, 0, sizeof errs);
    v.errs = &errs;
    v.eqseen = js_malloc(ctx,
        (DYN_SC_MAX_EQUAL_DEPTH + 1) * sizeof(dyn_sc_pair_t));
    if (!v.eqseen)
        return JS_EXCEPTION;
    v.dstack = js_malloc(ctx, (DYN_SC_MAX_DEPTH + 2) * sizeof(*v.dstack));
    if (!v.dstack) {
        js_free(ctx, v.eqseen);
        return JS_EXCEPTION;
    }

    r = dyn_sc_check_top(&v, sch->root, inst);
    js_free(ctx, v.eqseen);
    js_free(ctx, v.dstack);
    if (r < 0) {
        dyn_sc_errs_free(ctx, &errs);
        js_free(ctx, v.path.seg);
        return JS_EXCEPTION;
    }
    res = dyn_sc_result(ctx, &errs, r);
    dyn_sc_errs_free(ctx, &errs);
    js_free(ctx, v.path.seg);
    return res;
}

JSValue dyn_sc_schema_validate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue compiled, res;
    dyn_schema_t* sch;
    JSValueConst a1[1];
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx,
            "Schema.validate(schema, instance) requires two arguments");
    }

    sch = (dyn_schema_t*)JS_GetOpaque(argv[0], dyn_schema_class_id);
    if (sch)
        return dyn_sc_validate_impl(ctx, sch, argv[1]);

    if (JS_IsObject(argv[0])) {
        uint64_t now_hash = 0;
        int hres = dyn_value_hash64(ctx, argv[0], &now_hash);
        int have_hash = (hres == 0);
        if (hres < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JSValue cached, hv;
        uint64_t old_hash = 0;
        int have_old = 0;
        JSAtom cache_atom = JS_NewAtom(ctx, DYN_SC_CACHE_ATOM_NAME);
        JSAtom hash_atom = JS_NewAtom(ctx, DYN_SC_CACHE_HASH_NAME);
        if (cache_atom == JS_ATOM_NULL || hash_atom == JS_ATOM_NULL) {
            if (cache_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, cache_atom);
            if (hash_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, hash_atom);
            return JS_EXCEPTION;
        }
        cached = JS_UNDEFINED;
        hv = JS_UNDEFINED;
        {
            int cr = dyn_sc_get_own(ctx, argv[0], cache_atom, &cached);
            int hr = have_hash ? dyn_sc_get_own(ctx, argv[0], hash_atom, &hv)
                               : 0;
            if (cr < 0 || hr < 0) {
                if (cr > 0)
                    JS_FreeValue(ctx, cached);
                if (hr > 0)
                    JS_FreeValue(ctx, hv);
                JS_FreeValue(ctx, JS_GetException(ctx));
                cached = JS_UNDEFINED;
                hv = JS_UNDEFINED;
            }
        }
        if (!JS_IsException(hv) && JS_IsString(hv)) {
            const char* hs = JS_ToCString(ctx, hv);
            if (hs) {
                old_hash = (uint64_t)strtoull(hs, NULL, 16);
                have_old = 1;
                JS_FreeCString(ctx, hs);
            }
        }
        if (!JS_IsException(hv))
            JS_FreeValue(ctx, hv);
        if (!JS_IsException(cached) && !JS_IsUndefined(cached) && have_hash && have_old && old_hash == now_hash) {
            sch = (dyn_schema_t*)JS_GetOpaque(cached, dyn_schema_class_id);
            if (sch) {
                res = dyn_sc_validate_impl(ctx, sch, argv[1]);
                JS_FreeValue(ctx, cached);
                JS_FreeAtom(ctx, hash_atom);
                JS_FreeAtom(ctx, cache_atom);
                return res;
            }
        }
        JS_FreeValue(ctx, cached);
        if (!JS_IsException(cached) && JS_DeleteProperty(ctx, argv[0], cache_atom, 0) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeAtom(ctx, hash_atom);
        JS_FreeAtom(ctx, cache_atom);
    }

    a1[0] = argv[0];
    compiled = dyn_sc_compile_entry(ctx, JS_UNDEFINED, 1, a1);
    if (JS_IsException(compiled))
        return compiled;
    sch = (dyn_schema_t*)JS_GetOpaque(compiled, dyn_schema_class_id);
    res = dyn_sc_validate_impl(ctx, sch, argv[1]);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, compiled);
        return res;
    }

    if (JS_IsObject(argv[0])) {
        uint64_t h2 = 0;
        int hres = dyn_value_hash64(ctx, argv[0], &h2);
        if (hres < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (hres == 0) {
            JSAtom cache_atom = JS_NewAtom(ctx, DYN_SC_CACHE_ATOM_NAME);
            JSAtom hash_atom = cache_atom != JS_ATOM_NULL
                ? JS_NewAtom(ctx, DYN_SC_CACHE_HASH_NAME)
                : JS_ATOM_NULL;
            if (cache_atom != JS_ATOM_NULL && hash_atom != JS_ATOM_NULL) {
                char hex[17];
                for (int k = 15; k >= 0; k--) {
                    hex[k] = "0123456789abcdef"[h2 & 15];
                    h2 >>= 4;
                }
                hex[16] = 0;
                JS_DefinePropertyValue(ctx, argv[0], hash_atom,
                    JS_NewString(ctx, hex),
                    JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
                JS_DefinePropertyValue(ctx, argv[0], cache_atom,
                    JS_DupValue(ctx, compiled),
                    JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            }
            if (hash_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, hash_atom);
            if (cache_atom != JS_ATOM_NULL)
                JS_FreeAtom(ctx, cache_atom);
        }
    }

    JS_FreeValue(ctx, compiled);
    return res;
}

#endif
