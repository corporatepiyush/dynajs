#include "vserialize-internal.h"
#include "vserialize-bson.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#define VS_TA_KINDS 12

static void vb_init(vb_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

static void vb_free(vb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static int vs_bytes_arg(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *hold = JS_UNDEFINED;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "decode(bytes): expected a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        return -1;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 0;
}

static JSValue vs_new_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, r;

    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return r;
}

static const char* const vs_opt_keys[] = { "bigint" };

static int vs_bigint_opt(JSContext* ctx, int argc, JSValueConst* argv, const char* fn, int* out)
{
    *out = 0;
    if (dyn_opts_bag(ctx, argc, argv, 1, vs_opt_keys, countof(vs_opt_keys), fn) < 0)
        return -1;
    return argc > 1 ? dyn_opt_bool(ctx, argv[1], "bigint", fn, out) : 0;
}

static JSValue vs_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    venc_t e;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encode(value): a value is required");
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.fmt = (magic & 1) ? FMT_CBOR : FMT_MSGPACK;
    e.sorted = (magic & 2) != 0;
    if (vs_bigint_opt(ctx, argc, argv, magic == 0 ? "MsgPackEncode" : magic == 1 ? "CBOREncode" : "CBORCanonical", &e.bigint) < 0)
        return JS_EXCEPTION;
    vb_init(&e.b);
    if (venc_value(&e, argv[0]) < 0 || e.b.oom) {
        if (e.b.oom)
            JS_ThrowOutOfMemory(ctx);
        vb_free(&e.b);
        free(e.seen);
        return JS_EXCEPTION;
    }
    free(e.seen);
    out = vs_new_bytes(ctx, e.b.p, e.b.n);
    vb_free(&e.b);
    return out;
}

static JSValue vs_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    vdec_t d;
    JSValue hold = JS_UNDEFINED, out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode(bytes): bytes are required");
    memset(&d, 0, sizeof d);
    d.ctx = ctx;
    d.fmt = magic ? FMT_CBOR : FMT_MSGPACK;
    if (vs_bigint_opt(ctx, argc, argv, magic ? "CBORDecode" : "MsgPackDecode", &d.bigint) < 0)
        return JS_EXCEPTION;
    if (vs_bytes_arg(ctx, argv[0], &d.p, &d.n, &hold) < 0)
        return JS_EXCEPTION;
    out = vdec_value(&d);
    if (!JS_IsException(out) && d.i != d.n) {
        JS_FreeValue(ctx, out);
        out = JS_ThrowSyntaxError(ctx, "decode: %u trailing bytes after the "
                                       "value",
            (unsigned)(d.n - d.i));
    }
    {
        int t;
        for (t = 0; t < VDEC_TPL_DEPTH; t++)
            JS_FreeShapeCache(JS_GetRuntime(ctx), &d.tpl[t]);
    }
    JS_FreeValue(ctx, hold);
    return out;
}

static JSValue vs_value_hash(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    venc_t e;
    char hex[17];
    uint64_t h;
    int k;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ValueHash(value): a value is required");
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.fmt = FMT_CBOR;
    e.sorted = 1;
    if (vs_bigint_opt(ctx, argc, argv, "ValueHash", &e.bigint) < 0)
        return JS_EXCEPTION;
    vb_init(&e.b);
    if (venc_value(&e, argv[0]) < 0 || e.b.oom) {
        if (e.b.oom)
            JS_ThrowOutOfMemory(ctx);
        vb_free(&e.b);
        free(e.seen);
        return JS_EXCEPTION;
    }
    free(e.seen);
    h = dyn_xxh64(e.b.p, e.b.n, 0);
    vb_free(&e.b);
    for (k = 15; k >= 0; k--) {
        hex[k] = "0123456789abcdef"[h & 15];
        h >>= 4;
    }
    hex[16] = 0;
    return JS_NewString(ctx, hex);
}

static struct {
    int built;
    JSClassID ta[VS_TA_KINDS];
    JSClassID dv;
} vs_kinds;

static int vs_view_kinds_build(JSContext* ctx)
{
    int i;

    if (vs_kinds.built)
        return 0;
    for (i = 0; i < VS_TA_KINDS; i++) {
        JSValue zero = JS_NewInt32(ctx, 0), probe;
        vs_kinds.ta[i] = JS_INVALID_CLASS_ID;
        if (JS_IsException(zero))
            return -1;
        probe = JS_NewTypedArray(ctx, 1, (JSValueConst*)&zero,
            (JSTypedArrayEnum)i);
        JS_FreeValue(ctx, zero);
        if (JS_IsException(probe)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        vs_kinds.ta[i] = JS_GetClassID(probe);
        JS_FreeValue(ctx, probe);
    }
    {
        static const uint8_t zero = 0;
        JSValue ab = JS_NewArrayBufferCopy(ctx, &zero, 0);
        vs_kinds.dv = JS_INVALID_CLASS_ID;
        if (!JS_IsException(ab)) {
            JSValue probe = JS_NewDataView(ctx, ab, 0, 0);
            JS_FreeValue(ctx, ab);
            if (JS_IsException(probe)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            } else {
                vs_kinds.dv = JS_GetClassID(probe);
                JS_FreeValue(ctx, probe);
            }
        }
    }
    vs_kinds.built = 1;
    return 0;
}

JSValue vs_clone_bytes(vclone_t* c, JSValueConst v)
{
    JSContext* ctx = c->ctx;
    size_t off, len, bpe, ab;
    JSValue buf, copy, r;
    JSClassID cid;
    int i, kind = -1, is_dv = 0;

    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE)
        return JS_UNDEFINED;
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        uint8_t* base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_UNDEFINED;
        }
        return JS_NewArrayBufferCopy(ctx, base, ab);
    }
    {
        uint8_t* base = JS_GetArrayBuffer(ctx, &ab, buf);
        if (!base || off > ab || len > ab - off) {
            JS_FreeValue(ctx, buf);
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_UNDEFINED;
        }
        copy = JS_NewArrayBufferCopy(ctx, base + off, len);
    }
    JS_FreeValue(ctx, buf);
    if (JS_IsException(copy))
        return copy;

    if (vs_view_kinds_build(ctx) < 0) {
        JS_FreeValue(ctx, copy);
        return JS_EXCEPTION;
    }
    cid = JS_GetClassID(v);
    if (vs_kinds.dv != JS_INVALID_CLASS_ID && cid == vs_kinds.dv) {
        is_dv = 1;
    } else {
        for (i = 0; i < VS_TA_KINDS && kind < 0; i++)
            if (vs_kinds.ta[i] != JS_INVALID_CLASS_ID
                && cid == vs_kinds.ta[i])
                kind = i;
    }

    if (is_dv) {
        r = JS_NewDataView(ctx, copy, 0, len);
    } else {
        JSValueConst ta[3];
        ta[0] = copy;
        ta[1] = JS_UNDEFINED;
        ta[2] = JS_UNDEFINED;
        r = JS_NewTypedArray(ctx, 3, ta,
            kind >= 0 ? (JSTypedArrayEnum)kind
                      : JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, copy);
    return r;
}

static JSValue vs_transfer_list(JSContext* ctx, JSValueConst opts)
{
    JSValue t, g, a, from, r;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "structuredClone: options must be an object");
    t = JS_GetPropertyStr(ctx, opts, "transfer");
    if (JS_IsException(t) || JS_IsUndefined(t))
        return t;
    if (!JS_IsObject(t)) {
        JS_FreeValue(ctx, t);
        return JS_ThrowTypeError(ctx, "structuredClone: options.transfer must be an iterable of objects");
    }
    g = JS_GetGlobalObject(ctx);
    a = JS_GetPropertyStr(ctx, g, "Array");
    JS_FreeValue(ctx, g);
    from = JS_IsException(a) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, a, "from");
    r = JS_IsException(from) ? JS_EXCEPTION : JS_Call(ctx, from, a, 1, (JSValueConst*)&t);
    JS_FreeValue(ctx, from);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, t);
    return r;
}

static JSValue vs_xfer_fin(JSContext* ctx, JSValueConst pre, int ok)
{
    JSValue f = JS_GetPropertyUint32(ctx, pre, ok ? 3 : 2), r;
    if (JS_IsException(f))
        return f;
    r = JS_Call(ctx, f, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, f);
    return r;
}

static JSValue vs_structured_clone(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    vclone_t c;
    JSValue out, list, streams = JS_UNDEFINED, pre = JS_UNDEFINED, src;
    int64_t n = 0, i, j, ns = 0;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "structuredClone(value): a value is required");
    list = vs_transfer_list(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(list))
        return list;
    if (!JS_IsUndefined(list)) {
        JSValue lv = JS_GetPropertyStr(ctx, list, "length");
        int le = JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0;
        JS_FreeValue(ctx, lv);
        if (le)
            goto fail;
    }
    for (i = 0; i < n; i++) {
        JSValue x = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
        int bad = 0;
        if (!JS_IsObject(x)) {
            JS_FreeValue(ctx, x);
            JS_ThrowTypeError(ctx, "structuredClone: transfer[%d] is not an object", (int)i);
            goto fail;
        }
        for (j = 0; j < i && !bad; j++) {
            JSValue y = JS_GetPropertyUint32(ctx, list, (uint32_t)j);
            bad = JS_VALUE_GET_PTR(y) == JS_VALUE_GET_PTR(x);
            JS_FreeValue(ctx, y);
        }
        if (bad) {
            JS_FreeValue(ctx, x);
            js_throw_dom_exception(ctx, "DataCloneError", "structuredClone: transfer[%d] is listed twice", (int)i);
            goto fail;
        }
        if (JS_GetBufferKind(x) == JS_BUFFER_KIND_BUFFER) {
            bad = JS_IsSharedArrayBuffer(x) ? 1 : JS_IsArrayBufferDetached(x) == 1 ? 2 : 0;
            JS_FreeValue(ctx, x);
            if (bad) {
                js_throw_dom_exception(ctx, "DataCloneError", bad == 1 ? "structuredClone: a SharedArrayBuffer cannot be transferred" : "structuredClone: transfer[%d] is a detached ArrayBuffer", (int)i);
                goto fail;
            }
            continue;
        }
        if (!js_streams_kind(x)) {
            JS_FreeValue(ctx, x);
            js_throw_dom_exception(ctx, "DataCloneError", "structuredClone: transfer[%d] is not transferable", (int)i);
            goto fail;
        }
        if (JS_IsUndefined(streams) && JS_IsException(streams = JS_NewArray(ctx))) {
            JS_FreeValue(ctx, x);
            goto fail;
        }
        JS_SetPropertyInt64(ctx, streams, ns++, x);
    }
    src = JS_DupValue(ctx, argv[0]);
    if (ns) {
        JSValue env;
        pre = js_streams_transfer_prepare(ctx, argv[0], streams);
        if (JS_IsException(pre)) {
            JS_FreeValue(ctx, src);
            goto fail;
        }
        env = JS_GetPropertyUint32(ctx, pre, 0);
        JS_FreeValue(ctx, src);
        src = env;
    }
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    if (vclone_cache_protos(&c) < 0)
        out = JS_EXCEPTION;
    else
        out = vclone(&c, src);
    JS_FreeValue(ctx, src);
    free(c.src);
    free(c.dst);
    free(c.idx);
    vclone_free_cache(&c);
    if (ns) {
        JSValue r = vs_xfer_fin(ctx, pre, !JS_IsException(out));
        if (JS_IsException(r) && !JS_IsException(out)) {
            JS_FreeValue(ctx, out);
            out = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
        if (!JS_IsException(out))
            out = js_streams_transfer_revive(ctx, out);
    }
    if (!JS_IsException(out)) {
        for (i = 0; i < n; i++) {
            JSValue x = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
            if (JS_GetBufferKind(x) == JS_BUFFER_KIND_BUFFER)
                JS_DetachArrayBuffer(ctx, x);
            JS_FreeValue(ctx, x);
        }
    }
    JS_FreeValue(ctx, pre);
    JS_FreeValue(ctx, streams);
    JS_FreeValue(ctx, list);
    return out;
fail:
    JS_FreeValue(ctx, pre);
    JS_FreeValue(ctx, streams);
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_vs_funcs[] = {
    JS_CFUNC_MAGIC_DEF("MsgPackEncode", 1, vs_encode, 0),
    JS_CFUNC_MAGIC_DEF("MsgPackDecode", 1, vs_decode, 0),
    JS_CFUNC_MAGIC_DEF("CBOREncode", 1, vs_encode, 1),
    JS_CFUNC_MAGIC_DEF("CBORDecode", 1, vs_decode, 1),
    JS_CFUNC_MAGIC_DEF("CBORCanonical", 1, vs_encode, 3),
    JS_CFUNC_DEF("BSONEncode", 1, dyn_bsonjs_encode_fn),
    JS_CFUNC_DEF("BSONDecode", 1, dyn_bsonjs_decode_fn),
    JS_CFUNC_DEF("ValueHash", 1, vs_value_hash),
    JS_CFUNC_DEF("structuredClone", 1, vs_structured_clone),
};

static int dyn_vs_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_vs_funcs, countof(dyn_vs_funcs)) < 0)
        return -1;
    return dyn_bsonjs_register(ctx, m);
}

int js_nat_init_vserialize(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:serialize", dyn_vs_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_vs_funcs, countof(dyn_vs_funcs)) < 0)
        return -1;
    return dyn_bsonjs_add_exports(ctx, m);
}

#endif
