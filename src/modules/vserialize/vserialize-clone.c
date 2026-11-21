#include "vserialize-internal.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#define VCLONE_HASH_MIN 64

static uint32_t vclone_hash(const void* p)
{
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    return (uint32_t)h;
}

static void vclone_reindex(vclone_t* c)
{
    uint32_t size = 128, i;

    while (size < (uint32_t)c->n * 2)
        size *= 2;
    free(c->idx);
    c->idx = (uint32_t*)calloc(size, sizeof *c->idx);
    if (!c->idx) {
        c->imask = 0;
        return;
    }
    c->imask = size - 1;
    for (i = 0; i < (uint32_t)c->n; i++) {
        uint32_t sl = vclone_hash(JS_VALUE_GET_PTR(c->src[i])) & c->imask;
        while (c->idx[sl])
            sl = (sl + 1) & c->imask;
        c->idx[sl] = i + 1;
    }
}

static int vclone_find(vclone_t* c, JSValueConst v)
{
    void* p = JS_VALUE_GET_PTR(v);
    int i;

    if (c->imask) {
        uint32_t sl = vclone_hash(p) & c->imask;
        while (c->idx[sl]) {
            if (JS_VALUE_GET_PTR(c->src[c->idx[sl] - 1]) == p)
                return (int)c->idx[sl] - 1;
            sl = (sl + 1) & c->imask;
        }
        return -1;
    }
    for (i = 0; i < c->n; i++)
        if (JS_VALUE_GET_PTR(c->src[i]) == p)
            return i;
    return -1;
}

static int vclone_memo(vclone_t* c, JSValueConst s, JSValueConst d)
{
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        JSValue* a = (JSValue*)realloc(c->src, (size_t)nc * sizeof *a);
        JSValue* b = (JSValue*)realloc(c->dst, (size_t)nc * sizeof *b);
        if (a)
            c->src = a;
        if (b)
            c->dst = b;
        if (!a || !b) {
            JS_ThrowOutOfMemory(c->ctx);
            return -1;
        }
        c->cap = nc;
    }
    c->src[c->n] = (JSValue)s;
    c->dst[c->n] = (JSValue)d;
    c->n++;
    if (c->imask && (uint32_t)c->n * 2 <= c->imask + 1) {
        uint32_t sl = vclone_hash(JS_VALUE_GET_PTR(s)) & c->imask;
        while (c->idx[sl])
            sl = (sl + 1) & c->imask;
        c->idx[sl] = (uint32_t)c->n;
    } else if (c->n >= VCLONE_HASH_MIN) {
        vclone_reindex(c);
    }
    return 0;
}

static int vclone_props(vclone_t* c, JSValueConst v, JSValue out)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (JS_GetOwnPropertyNames(c->ctx, &tab, &len, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        JSPropertyDescriptor desc;
        JSValue cl;
        int r = JS_GetOwnProperty(c->ctx, &desc, v, tab[k].atom);
        if (r < 0) {
            rc = -1;
            break;
        }
        if (r == 0)
            continue;
        if (desc.flags & JS_PROP_GETSET) {
            JS_FreeValue(c->ctx, desc.getter);
            JS_FreeValue(c->ctx, desc.setter);
            desc.value = JS_GetProperty(c->ctx, v, tab[k].atom);
            if (JS_IsException(desc.value)) {
                rc = -1;
                break;
            }
        }
        cl = vclone(c, desc.value);
        JS_FreeValue(c->ctx, desc.value);
        if (JS_IsException(cl)
            || JS_DefinePropertyValue(c->ctx, out, tab[k].atom, cl,
                   JS_PROP_C_W_E)
                < 0)
            rc = -1;
    }
    JS_FreePropertyEnum(c->ctx, tab, len);
    return rc;
}

static int vclone_cache_class(vclone_t* c, const char* name, JSValue* ctor,
    JSValue* proto)
{
    JSValue g, k;

    *ctor = JS_UNDEFINED;
    *proto = JS_UNDEFINED;
    g = JS_GetGlobalObject(c->ctx);
    if (JS_IsException(g))
        return -1;
    k = JS_GetPropertyStr(c->ctx, g, name);
    JS_FreeValue(c->ctx, g);
    if (JS_IsException(k))
        return -1;
    if (!JS_IsFunction(c->ctx, k)) {
        JS_FreeValue(c->ctx, k);
        return 0;
    }
    *proto = JS_GetPropertyStr(c->ctx, k, "prototype");
    if (JS_IsException(*proto)) {
        JS_FreeValue(c->ctx, k);
        return -1;
    }
    *ctor = k;
    return 0;
}

void vclone_free_cache(vclone_t* c)
{
    JS_FreeValue(c->ctx, c->date_ctor);
    JS_FreeValue(c->ctx, c->date_proto);
    JS_FreeValue(c->ctx, c->date_gettime);
    JS_FreeValue(c->ctx, c->regexp_ctor);
    JS_FreeValue(c->ctx, c->regexp_proto);
    JS_FreeValue(c->ctx, c->map_ctor);
    JS_FreeValue(c->ctx, c->map_proto);
    JS_FreeValue(c->ctx, c->map_set);
    JS_FreeValue(c->ctx, c->map_entries);
    JS_FreeValue(c->ctx, c->set_ctor);
    JS_FreeValue(c->ctx, c->set_proto);
    JS_FreeValue(c->ctx, c->set_add);
    JS_FreeValue(c->ctx, c->set_values);
    JS_FreeValue(c->ctx, c->object_proto);
    JS_FreeValue(c->ctx, c->array_proto);
}

int vclone_cache_protos(vclone_t* c)
{
    if (vclone_cache_class(c, "Date", &c->date_ctor, &c->date_proto) < 0 || vclone_cache_class(c, "RegExp", &c->regexp_ctor, &c->regexp_proto) < 0 || vclone_cache_class(c, "Map", &c->map_ctor, &c->map_proto) < 0 || vclone_cache_class(c, "Set", &c->set_ctor, &c->set_proto) < 0)
        return -1;
    if (JS_IsObject(c->date_proto)) {
        c->date_gettime = JS_GetPropertyStr(c->ctx, c->date_proto, "getTime");
        if (JS_IsException(c->date_gettime))
            return -1;
    }
    if (JS_IsObject(c->map_proto)) {
        c->map_set = JS_GetPropertyStr(c->ctx, c->map_proto, "set");
        c->map_entries = JS_GetPropertyStr(c->ctx, c->map_proto, "entries");
        if (JS_IsException(c->map_set) || JS_IsException(c->map_entries))
            return -1;
    }
    if (JS_IsObject(c->set_proto)) {
        c->set_add = JS_GetPropertyStr(c->ctx, c->set_proto, "add");
        c->set_values = JS_GetPropertyStr(c->ctx, c->set_proto, "values");
        if (JS_IsException(c->set_add) || JS_IsException(c->set_values))
            return -1;
    }
    {
        JSValue o = JS_NewObject(c->ctx), a = JS_NewArray(c->ctx), ap;
        if (JS_IsException(o) || JS_IsException(a)) {
            JS_FreeValue(c->ctx, o);
            JS_FreeValue(c->ctx, a);
            return -1;
        }
        c->object_proto = JS_GetPrototype(c->ctx, o);
        c->array_proto = JS_GetPrototype(c->ctx, a);
        JS_FreeValue(c->ctx, o);
        JS_FreeValue(c->ctx, a);
        if (JS_IsException(c->object_proto) || JS_IsException(c->array_proto))
            return -1;
        ap = JS_GetPrototype(c->ctx, c->array_proto);
        if (JS_IsException(ap))
            return -1;
        c->array_plain = JS_IsObject(ap) && JS_VALUE_GET_PTR(ap) == JS_VALUE_GET_PTR(c->object_proto);
        JS_FreeValue(c->ctx, ap);
    }
    return 0;
}

static int vclone_fill_container(vclone_t* c, JSValueConst v, JSValue out,
    JSValueConst iter_method,
    JSValueConst put_method, int want_key)
{
    JSValue next, iter;
    int rc = -1;

    if (!JS_IsFunction(c->ctx, iter_method) || !JS_IsFunction(c->ctx, put_method)) {
        JS_ThrowTypeError(c->ctx, "structuredClone: the %s prototype methods "
                                  "are missing",
            want_key ? "Map" : "Set");
        return -1;
    }
    iter = JS_Call(c->ctx, iter_method, v, 0, NULL);
    if (JS_IsException(iter))
        return -1;
    next = JS_GetPropertyStr(c->ctx, iter, "next");
    if (JS_IsException(next)) {
        JS_FreeValue(c->ctx, iter);
        return -1;
    }
    for (;;) {
        JSValue res, dv, ck = JS_UNDEFINED, cv = JS_UNDEFINED;
        int done;

        res = JS_Call(c->ctx, next, iter, 0, NULL);
        if (JS_IsException(res))
            goto out;
        dv = JS_GetPropertyStr(c->ctx, res, "done");
        if (JS_IsException(dv)) {
            JS_FreeValue(c->ctx, res);
            goto out;
        }
        done = JS_ToBool(c->ctx, dv);
        JS_FreeValue(c->ctx, dv);
        if (done == -1) {
            JS_FreeValue(c->ctx, res);
            goto out;
        }
        if (done) {
            JS_FreeValue(c->ctx, res);
            break;
        }
        dv = JS_GetPropertyStr(c->ctx, res, "value");
        JS_FreeValue(c->ctx, res);
        if (JS_IsException(dv))
            goto out;
        if (want_key) {
            JSValue entry = dv;
            JSValue ckk, cvv;
            ck = JS_GetPropertyUint32(c->ctx, entry, 0);
            cv = JS_GetPropertyUint32(c->ctx, entry, 1);
            JS_FreeValue(c->ctx, entry);
            if (JS_IsException(ck) || JS_IsException(cv)) {
                JS_FreeValue(c->ctx, ck);
                JS_FreeValue(c->ctx, cv);
                goto out;
            }
            ckk = vclone(c, ck);
            JS_FreeValue(c->ctx, ck);
            cvv = vclone(c, cv);
            JS_FreeValue(c->ctx, cv);
            ck = ckk;
            cv = cvv;
        } else {
            cv = vclone(c, dv);
            JS_FreeValue(c->ctx, dv);
        }
        if (JS_IsException(ck) || JS_IsException(cv)) {
            JS_FreeValue(c->ctx, ck);
            JS_FreeValue(c->ctx, cv);
            goto out;
        }
        {
            JSValueConst args[2];
            int nargs = want_key ? 2 : 1;
            JSValue r;
            if (want_key) {
                args[0] = ck;
                args[1] = cv;
            } else {
                args[0] = cv;
            }
            r = JS_Call(c->ctx, put_method, out, nargs, args);
            JS_FreeValue(c->ctx, r);
            JS_FreeValue(c->ctx, ck);
            JS_FreeValue(c->ctx, cv);
            if (JS_IsException(r))
                goto out;
        }
    }
    rc = 0;
out:
    JS_FreeValue(c->ctx, next);
    JS_FreeValue(c->ctx, iter);
    return rc;
}

static JSValue vclone_date(vclone_t* c, JSValueConst v)
{
    JSValue ms, args[1], out;

    if (!JS_IsFunction(c->ctx, c->date_gettime) || !JS_IsFunction(c->ctx, c->date_ctor))
        return JS_ThrowTypeError(c->ctx, "structuredClone: Date is missing");
    ms = JS_Call(c->ctx, c->date_gettime, v, 0, NULL);
    if (JS_IsException(ms))
        return JS_EXCEPTION;
    if (!JS_IsNumber(ms)) {
        JS_FreeValue(c->ctx, ms);
        return JS_ThrowTypeError(c->ctx, "structuredClone: getTime did not "
                                         "return a number");
    }
    args[0] = ms;
    out = JS_CallConstructor(c->ctx, c->date_ctor, 1, args);
    JS_FreeValue(c->ctx, ms);
    return out;
}

static JSValue vclone_regexp(vclone_t* c, JSValueConst v)
{
    JSValue src, fl, args[2], out;

    src = JS_GetPropertyStr(c->ctx, v, "source");
    if (JS_IsException(src))
        return JS_EXCEPTION;
    fl = JS_GetPropertyStr(c->ctx, v, "flags");
    if (JS_IsException(fl)) {
        JS_FreeValue(c->ctx, src);
        return JS_EXCEPTION;
    }
    if (!JS_IsString(src) || !JS_IsString(fl)) {
        JS_FreeValue(c->ctx, src);
        JS_FreeValue(c->ctx, fl);
        return JS_ThrowTypeError(c->ctx, "structuredClone: RegExp source/flags "
                                         "are not strings");
    }
    args[0] = src;
    args[1] = fl;
    out = JS_CallConstructor(c->ctx, c->regexp_ctor, 2, args);
    JS_FreeValue(c->ctx, src);
    JS_FreeValue(c->ctx, fl);
    return out;
}

static int vclone_matches(vclone_t* c, JSValueConst v, JSValueConst proto,
    JSValueConst with_proto, JSValueConst with_ctor)
{
    int same;

    if (JS_IsObject(proto)) {
        same = JS_SameValue(c->ctx, proto, with_proto);
        if (same < 0)
            return -1;
        if (same)
            return 1;
    }
    if (JS_IsFunction(c->ctx, with_ctor)) {
        same = JS_IsInstanceOf(c->ctx, v, with_ctor);
        if (same != 0)
            return same;
    }
    return 0;
}

static JSValue vclone_builtin(vclone_t* c, JSValueConst v, JSValue proto)
{
    int same;

    same = vclone_matches(c, v, proto, c->date_proto, c->date_ctor);
    if (same < 0)
        return JS_EXCEPTION;
    if (same) {
        JSValue out = vclone_date(c, v);
        if (JS_IsException(out))
            return out;
        if (vclone_memo(c, v, out) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
    same = vclone_matches(c, v, proto, c->regexp_proto, c->regexp_ctor);
    if (same < 0)
        return JS_EXCEPTION;
    if (same) {
        JSValue out = vclone_regexp(c, v);
        if (JS_IsException(out))
            return out;
        if (vclone_memo(c, v, out) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
    same = vclone_matches(c, v, proto, c->map_proto, c->map_ctor);
    if (same < 0)
        return JS_EXCEPTION;
    if (same) {
        JSValue out = JS_CallConstructor(c->ctx, c->map_ctor, 0, NULL);
        if (JS_IsException(out))
            return out;
        if (vclone_memo(c, v, out) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        if (vclone_fill_container(c, v, out, c->map_entries, c->map_set, 1) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
    same = vclone_matches(c, v, proto, c->set_proto, c->set_ctor);
    if (same < 0)
        return JS_EXCEPTION;
    if (same) {
        JSValue out = JS_CallConstructor(c->ctx, c->set_ctor, 0, NULL);
        if (JS_IsException(out))
            return out;
        if (vclone_memo(c, v, out) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        if (vclone_fill_container(c, v, out, c->set_values, c->set_add, 0) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
    return JS_UNDEFINED;
}

static JSClassID vc_cid[8];

static void vclone_class_ids(JSContext* ctx)
{
    JSValue v[7], g, o;
    int i;
    if (vc_cid[7])
        return;
    v[0] = JS_NewObject(ctx);
    v[1] = JS_NewArray(ctx);
    v[2] = JS_NewError(ctx);
    v[3] = JS_NewInt32(ctx, 0);
    v[4] = JS_NewStringLen(ctx, "", 0);
    v[5] = JS_FALSE;
    v[6] = JS_NewBigInt64(ctx, 0);
    g = JS_GetGlobalObject(ctx);
    o = JS_GetPropertyStr(ctx, g, "Object");
    JS_FreeValue(ctx, g);
    for (i = 3; i < 7; i++) {
        JSValue b = JS_Call(ctx, o, JS_UNDEFINED, 1, (JSValueConst*)&v[i]);
        JS_FreeValue(ctx, v[i]);
        if (JS_IsException(b)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            b = JS_UNDEFINED;
        }
        v[i] = b;
    }
    JS_FreeValue(ctx, o);
    for (i = 0; i < 7; i++) {
        vc_cid[i] = JS_IsObject(v[i]) ? JS_GetClassID(v[i]) : 0;
        JS_FreeValue(ctx, v[i]);
    }
    vc_cid[7] = 1;
}

static JSValue vclone_global_call(JSContext* ctx, const char* ctor, const char* method, JSValueConst self, int construct, int argc, JSValueConst* argv)
{
    JSValue g = JS_GetGlobalObject(ctx), k = JS_GetPropertyStr(ctx, g, ctor), f, r;
    JS_FreeValue(ctx, g);
    if (JS_IsException(k))
        return k;
    if (construct) {
        r = JS_CallConstructor(ctx, k, argc, argv);
        JS_FreeValue(ctx, k);
        return r;
    }
    f = JS_GetPropertyStr(ctx, k, "prototype");
    JS_FreeValue(ctx, k);
    if (JS_IsException(f))
        return f;
    k = JS_GetPropertyStr(ctx, f, method);
    JS_FreeValue(ctx, f);
    if (JS_IsException(k))
        return k;
    r = JS_Call(ctx, k, self, argc, argv);
    JS_FreeValue(ctx, k);
    return r;
}

static JSValue vclone_error(vclone_t* c, JSValueConst v)
{
    static const char* const names[] = { "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError" };
    JSContext* ctx = c->ctx;
    JSValue nm = JS_GetPropertyStr(ctx, v, "name"), msg = JS_UNDEFINED, out;
    const char* use = "Error";
    const char* s;
    JSPropertyDescriptor d;
    JSAtom am;
    int i, r;
    if (JS_IsException(nm))
        return nm;
    s = JS_ToCString(ctx, nm);
    JS_FreeValue(ctx, nm);
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < (int)countof(names); i++)
        if (!strcmp(s, names[i]))
            use = names[i];
    JS_FreeCString(ctx, s);
    am = JS_NewAtom(ctx, "message");
    r = JS_GetOwnProperty(ctx, &d, v, am);
    JS_FreeAtom(ctx, am);
    if (r < 0)
        return JS_EXCEPTION;
    if (r) {
        if (d.flags & JS_PROP_GETSET) {
            JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter);
        } else {
            msg = JS_ToString(ctx, d.value);
            JS_FreeValue(ctx, d.value);
            if (JS_IsException(msg))
                return msg;
        }
    }
    out = vclone_global_call(ctx, use, NULL, JS_UNDEFINED, 1, JS_IsUndefined(msg) ? 0 : 1, (JSValueConst*)&msg);
    JS_FreeValue(ctx, msg);
    return out;
}

static JSValue vclone_exotic(vclone_t* c, JSValueConst v)
{
    static const char* const boxed[] = { "Number", "String", "Boolean", "BigInt" };
    JSClassID id = JS_GetClassID(v);
    JSValue out, prim;
    int i;
    vclone_class_ids(c->ctx);
    if (id == vc_cid[0] || id == vc_cid[1])
        return JS_UNDEFINED;
    if (id == vc_cid[2])
        out = vclone_error(c, v);
    else if (js_is_dom_exception(v))
        out = js_dom_exception_copy(c->ctx, v);
    else {
        for (i = 0; i < 4 && id != vc_cid[3 + i]; i++)
            ;
        if (i == 4)
            return js_throw_dom_exception(c->ctx, "DataCloneError", "structuredClone: this object cannot be cloned");
        prim = vclone_global_call(c->ctx, boxed[i], "valueOf", v, 0, 0, NULL);
        if (JS_IsException(prim))
            return prim;
        out = vclone_global_call(c->ctx, "Object", NULL, JS_UNDEFINED, 1, 1, (JSValueConst*)&prim);
        JS_FreeValue(c->ctx, prim);
    }
    if (JS_IsException(out))
        return out;
    if (vclone_memo(c, v, out) < 0) {
        JS_FreeValue(c->ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

JSValue vclone(vclone_t* c, JSValueConst v)
{
    int i;
    JSValue out;

    if (!JS_IsObject(v)) {
        if (JS_IsSymbol(v))
            return js_throw_dom_exception(c->ctx, "DataCloneError", "structuredClone: a Symbol cannot be cloned");
        return JS_DupValue(c->ctx, v);
    }
    i = vclone_find(c, v);
    if (i >= 0)
        return JS_DupValue(c->ctx, c->dst[i]);
    if (c->depth >= VS_MAX_DEPTH)
        return JS_ThrowRangeError(c->ctx, "structuredClone: nesting exceeds %d",
            VS_MAX_DEPTH);
    if (JS_IsFunction(c->ctx, v))
        return js_throw_dom_exception(c->ctx, "DataCloneError", "structuredClone: a function cannot be cloned");
    out = vs_clone_bytes(c, v);
    if (JS_IsException(out))
        return out;
    if (!JS_IsUndefined(out)) {
        if (vclone_memo(c, v, out) < 0) {
            JS_FreeValue(c->ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
    {
        JSValue proto = JS_GetPrototype(c->ctx, v);
        if (JS_IsException(proto))
            return proto;
        if (JS_IsObject(proto) && (JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(c->object_proto) || (c->array_plain && JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(c->array_proto))))
            out = JS_UNDEFINED;
        else
            out = vclone_builtin(c, v, proto);
        JS_FreeValue(c->ctx, proto);
        if (JS_IsException(out))
            return out;
        if (!JS_IsUndefined(out))
            return out;
    }
    out = vclone_exotic(c, v);
    if (!JS_IsUndefined(out))
        return out;
    out = JS_IsArray(c->ctx, v) == 1 ? JS_NewArray(c->ctx) : JS_NewObject(c->ctx);
    if (JS_IsException(out))
        return out;
    if (vclone_memo(c, v, out) < 0) {
        JS_FreeValue(c->ctx, out);
        return JS_EXCEPTION;
    }
    c->depth++;
    if (vclone_props(c, v, out) < 0) {
        c->depth--;
        JS_FreeValue(c->ctx, out);
        return JS_EXCEPTION;
    }
    c->depth--;
    return out;
}

#endif
