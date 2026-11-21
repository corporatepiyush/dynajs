#include "libc-wst.h"

enum { AK_CALL, AK_TORS, AK_TOWS, AK_FROMRS, AK_BODY, AK_SOCK, AK_FDSRC, AK_FDSINK, AK_XRS, AK_XWS, AK_FAST, AK_XFER };
enum { IO_READ, IO_WRITE, IO_FLUSH, IO_CLOSE, IO_CLOSED, IO_DISPOSE };

typedef struct SAdp {
    JSValue obj;
    JSValue v[6];
    SDef d;
    SRS* rs;
    SWS* ws;
    SReader* rd;
    SWriter* wr;
    double n0, n1, n2;
    int fd;
    int8_t kind, closed, taken, eof, done, listening, pend, flag, read_done, write_done, half_open, sync;
    const char* name;
} SAdp;

static void adp_finalizer(JSRuntime* rt, JSValue val)
{
    SAdp* a = wst_opaque(val, WSTC_ADP);
    int i;
    if (!a)
        return;
    for (i = 0; i < 6; i++)
        JS_FreeValueRT(rt, a->v[i]);
    sdef_free(rt, &a->d);
    js_free_rt(rt, a);
}

static void adp_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SAdp* a = wst_opaque(val, WSTC_ADP);
    int i;
    if (!a)
        return;
    for (i = 0; i < 6; i++)
        JS_MarkValue(rt, a->v[i], mf);
    sdef_mark(rt, &a->d, mf);
}

static SAdp* adp_of(JSValueConst v)
{
    return wst_opaque(v, WSTC_ADP);
}

static JSValue adp_new_proto(JSContext* ctx, int kind, JSValueConst proto, SAdp** out)
{
    SAdp* a;
    JSValue obj = wst_new_obj(ctx, WSTC_ADP, proto, sizeof(SAdp), (void**)&a);
    int i;
    if (JS_IsException(obj))
        return obj;
    a->kind = kind;
    for (i = 0; i < 6; i++)
        a->v[i] = JS_UNDEFINED;
    sdef_init(&a->d);
    *out = a;
    return obj;
}

static JSValue adp_new(JSContext* ctx, int kind, SAdp** out)
{
    return adp_new_proto(ctx, kind, JS_NULL, out);
}

static int adp_is(JSValueConst v, int kind)
{
    SAdp* a = JS_GetClassID(v) == wst_cid[WSTC_ADP] ? adp_of(v) : NULL;
    return a && a->kind == kind;
}

static void adp_set(JSContext* ctx, SAdp* a, int i, JSValue v)
{
    JSValue old = a->v[i];
    a->v[i] = v;
    JS_FreeValue(ctx, old);
}

static JSValue adp_call(JSContext* ctx, JSValueConst adp, JSValueConst x)
{
    SAdp* k;
    JSValue obj = adp_new(ctx, AK_CALL, &k);
    if (JS_IsException(obj))
        return obj;
    k->v[0] = JS_DupValue(ctx, adp);
    k->v[1] = JS_DupValue(ctx, x);
    return obj;
}

static JSValue resolved(JSContext* ctx, JSValueConst v)
{
    JSValue p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p) && dyn_settle(ctx, p, v, 0) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static JSValue resolved_num(JSContext* ctx, double n)
{
    JSValue v = JS_NewFloat64(ctx, n), p = resolved(ctx, v);
    JS_FreeValue(ctx, v);
    return p;
}

static JSValue rethrow(JSContext* ctx, JSValueConst e)
{
    return JS_Throw(ctx, JS_DupValue(ctx, e));
}

static JSValue call_method(JSContext* ctx, JSValueConst obj, const char* name, int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name), r;
    if (JS_IsException(fn))
        return fn;
    r = JS_Call(ctx, fn, obj, argc, argv);
    JS_FreeValue(ctx, fn);
    return r;
}

static void call_drop(JSContext* ctx, JSValue r)
{
    if (JS_IsException(r))
        wst_drop_exc(ctx);
    else
        JS_FreeValue(ctx, r);
}

static int close_quietly(JSContext* ctx, JSValueConst o)
{
    JSValue fn;
    if (!JS_IsObject(o))
        return 0;
    fn = JS_GetPropertyStr(ctx, o, "close");
    if (JS_IsException(fn)) {
        wst_drop_exc(ctx);
        return JS_HasUncatchableException(ctx) ? -1 : 0;
    }
    if (JS_IsFunction(ctx, fn))
        call_drop(ctx, JS_Call(ctx, fn, o, 0, NULL));
    JS_FreeValue(ctx, fn);
    return JS_HasUncatchableException(ctx) ? -1 : 0;
}

static int has_method(JSContext* ctx, JSValueConst o, const char* name)
{
    JSValue fn;
    int r;
    if (!JS_IsObject(o))
        return 0;
    fn = JS_GetPropertyStr(ctx, o, name);
    if (JS_IsException(fn))
        return -1;
    r = JS_IsFunction(ctx, fn);
    JS_FreeValue(ctx, fn);
    return r;
}

static JSValue dict_str(JSContext* ctx, JSValueConst d, const char* k)
{
    return JS_IsObject(d) ? JS_GetPropertyStr(ctx, d, k) : JS_UNDEFINED;
}

static double u8_len(JSContext* ctx, JSValueConst v)
{
    WVInfo vi;
    double n = 0;
    if (wst_view_info(ctx, v, &vi) > 0) {
        n = vi.len;
        JS_FreeValue(ctx, vi.buffer);
    }
    return n;
}

static JSValue u8_sub(JSContext* ctx, JSValueConst v, double start, double n)
{
    WVInfo vi;
    JSValue r;
    if (wst_view_info(ctx, v, &vi) <= 0)
        return JS_ThrowTypeError(ctx, "not a view");
    r = wst_new_u8(ctx, vi.buffer, vi.off + start, n);
    JS_FreeValue(ctx, vi.buffer);
    return r;
}

static uint8_t* u8_ptr(JSContext* ctx, JSValueConst v, size_t* len)
{
    WVInfo vi;
    size_t sz = 0;
    uint8_t* p;
    *len = 0;
    if (wst_view_info(ctx, v, &vi) <= 0)
        return NULL;
    p = JS_PeekArrayBuffer(vi.buffer, &sz);
    JS_FreeValue(ctx, vi.buffer);
    if (!p || vi.off + vi.len > (double)sz)
        return NULL;
    *len = (size_t)vi.len;
    return p + (size_t)vi.off;
}

static void ab_free(JSRuntime* rt, void* opaque, void* ptr)
{
    (void)opaque;
    js_free_rt(rt, ptr);
}

static JSValue u8_zero(JSContext* ctx, double n)
{
    uint8_t* b = js_mallocz(ctx, n > 0 ? (size_t)n : 1);
    JSValue ab, r;
    if (!b)
        return JS_EXCEPTION;
    ab = JS_NewArrayBuffer(ctx, b, (size_t)n, ab_free, NULL, 0);
    if (JS_IsException(ab)) {
        js_free(ctx, b);
        return ab;
    }
    r = wst_new_u8(ctx, ab, 0, n);
    JS_FreeValue(ctx, ab);
    return r;
}

static JSValue u8_copy(JSContext* ctx, JSValueConst v, double n)
{
    size_t len;
    uint8_t* p = u8_ptr(ctx, v, &len);
    JSValue ab, r;
    if (!p && len == 0 && n > 0)
        return JS_ThrowTypeError(ctx, "the buffer is detached");
    if ((double)len < n)
        n = (double)len;
    ab = JS_NewArrayBufferCopy(ctx, p, (size_t)n);
    if (JS_IsException(ab))
        return ab;
    r = wst_new_u8(ctx, ab, 0, n);
    JS_FreeValue(ctx, ab);
    return r;
}

int wst_bytes_of_chunk(JSContext* ctx, JSValueConst v, const char* what, JSValue* out)
{
    const char* cs;
    size_t len, i, sz;
    uint8_t* p;
    JSValue ab;
    if (!JS_IsString(v))
        return wst_buffer_source_bytes(ctx, v, what, out);
    cs = JS_ToCStringLen2(ctx, &len, v, 0);
    if (!cs)
        return -1;
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)cs, len);
    JS_FreeCString(ctx, cs);
    if (JS_IsException(ab))
        return -1;
    p = JS_PeekArrayBuffer(ab, &sz);
    for (i = 0; p && i + 2 < sz; i++) {
        if (p[i] == 0xED && p[i + 1] >= 0xA0) {
            p[i] = 0xEF;
            p[i + 1] = 0xBF;
            p[i + 2] = 0xBD;
            i += 2;
        }
    }
    *out = wst_new_u8(ctx, ab, 0, (double)sz);
    JS_FreeValue(ctx, ab);
    return JS_IsException(*out) ? -1 : 0;
}

static JSValue to_string_free(JSContext* ctx, JSValue v)
{
    JSValue r = JS_ToString(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

static int get_len(JSContext* ctx, JSValueConst o, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "length");
    double d;
    *out = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_ToFloat64(ctx, &d, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    *out = !(d > 0) ? 0 : d > 9007199254740991.0 ? (int64_t)9007199254740991LL : (int64_t)d;
    return 0;
}

static JSValue type_err(JSContext* ctx, const char* msg)
{
    return wst_err(ctx, 0, "%s", msg);
}

static int react(JSContext* ctx, JSValueConst p, JSPromiseReactionFunc* fn, JSValueConst data, int magic)
{
    JSValue rx;
    int r;
    if (JS_IsException(p))
        return -1;
    rx = JS_NewPromiseReaction(ctx, fn, data, magic);
    if (JS_IsException(rx))
        return -1;
    r = JS_PromiseReact(ctx, p, rx);
    JS_FreeValue(ctx, rx);
    return r;
}

static void mark_handled_free(JSContext* ctx, JSValue p)
{
    if (JS_IsException(p)) {
        wst_drop_exc(ctx);
        return;
    }
    JS_PromiseMarkHandled(ctx, p);
    JS_FreeValue(ctx, p);
}

static JSValue io_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fd);

static int def_fn(JSContext* ctx, JSValueConst obj, JSAtom atom, const char* name, int len, int magic, JSValueConst adp, int getter)
{
    JSValue f = JS_NewCFunctionData(ctx, io_fn, len, magic, 1, &adp);
    if (JS_IsException(f))
        return -1;
    JS_DefinePropertyValueStr(ctx, f, "name", JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    if (getter)
        return JS_DefinePropertyGetSet(ctx, obj, atom, f, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    return JS_DefinePropertyValue(ctx, obj, atom, f, JS_PROP_C_W_E);
}

static int io_define(JSContext* ctx, JSValueConst o, JSValueConst adp, int sink)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return -1;
    JSAtom a;
    int r = 0;
    if (sink) {
        a = JS_NewAtom(ctx, "write");
        r |= def_fn(ctx, o, a, "write", 1, IO_WRITE, adp, 0);
        JS_FreeAtom(ctx, a);
        a = JS_NewAtom(ctx, "flush");
        r |= def_fn(ctx, o, a, "flush", 0, IO_FLUSH, adp, 0);
        JS_FreeAtom(ctx, a);
    } else {
        a = JS_NewAtom(ctx, "read");
        r |= def_fn(ctx, o, a, "read", 1, IO_READ, adp, 0);
        JS_FreeAtom(ctx, a);
    }
    a = JS_NewAtom(ctx, "close");
    r |= def_fn(ctx, o, a, "close", 0, IO_CLOSE, adp, 0);
    JS_FreeAtom(ctx, a);
    a = JS_NewAtom(ctx, "closed");
    r |= def_fn(ctx, o, a, "get closed", 0, IO_CLOSED, adp, 1);
    JS_FreeAtom(ctx, a);
    a = JS_ValueToAtom(ctx, w->x[WSX_DISPOSE]);
    r |= def_fn(ctx, o, a, "[Symbol.dispose]", 0, IO_DISPOSE, adp, 0);
    JS_FreeAtom(ctx, a);
    return r < 0 ? -1 : 0;
}

static JSValue io_object(JSContext* ctx, JSValueConst adp, int sink)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o) || io_define(ctx, o, adp, sink) == 0)
        return o;
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue tors_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    SRBSC* c = a->rs->c;
    JSValue view = a->v[2], req = a->v[3], ch;
    double n, len;
    int r = 0;
    (void)magic;
    a->v[2] = JS_UNDEFINED;
    a->v[3] = JS_UNDEFINED;
    if (is_reject) {
        if (close_quietly(ctx, a->v[0]) == 0)
            rethrow(ctx, arg);
        r = -1;
        goto done;
    }
    if (JS_ToFloat64(ctx, &n, arg) < 0) {
        r = -1;
        goto done;
    }
    len = u8_len(ctx, view);
    if (!(n >= 0) || n > len || n != floor(n)) {
        if (close_quietly(ctx, a->v[0]) == 0)
            wst_throw(ctx, 1, "ByteSource.read resolved a byte count outside [0, buf.length]");
        r = -1;
        goto done;
    }
    if (n == 0) {
        close_quietly(ctx, a->v[0]);
        if (a->rs->state == RS_READABLE && !c->close_requested)
            r = wst_rbsc_close(ctx, c);
        if (r == 0 && c->pending.len > 0)
            r = wst_rbsc_respond(ctx, c, 0);
        goto done;
    }
    if (JS_IsNull(req)) {
        ch = n == len ? JS_DupValue(ctx, view) : n * 2 >= len ? u8_sub(ctx, view, 0, n) : u8_copy(ctx, view, n);
        if (JS_IsException(ch))
            r = -1;
        else {
            r = wst_rbsc_enqueue(ctx, c, ch);
            JS_FreeValue(ctx, ch);
        }
    } else if (!JS_IsNull(c->byob_request)) {
        r = wst_rbsc_respond(ctx, c, n);
    }
done:
    JS_FreeValue(ctx, view);
    JS_FreeValue(ctx, req);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue u8_fresh(JSContext* ctx, double n, uint8_t** out)
{
    uint8_t* b = js_malloc(ctx, n > 0 ? (size_t)n : 1);
    JSValue ab, r;
    if (!b)
        return JS_EXCEPTION;
    ab = JS_NewArrayBuffer(ctx, b, (size_t)n, ab_free, NULL, 0);
    if (JS_IsException(ab)) {
        js_free(ctx, b);
        return ab;
    }
    r = wst_new_u8(ctx, ab, 0, n);
    JS_FreeValue(ctx, ab);
    *out = b;
    return r;
}

static JSValue fdrs_read_sync(JSContext* ctx, JSValueConst data, SAdp* a, SAdp* k, JSValue req)
{
    JSValue view, nv, r;
    uint8_t* b = NULL;
    size_t len = 0;
    ssize_t n;
    if (JS_IsNull(req)) {
        view = u8_fresh(ctx, a->n0, &b);
        len = (size_t)a->n0;
    } else {
        view = wst_byobreq_view(ctx, req);
        if (!JS_IsException(view))
            b = u8_ptr(ctx, view, &len);
    }
    if (JS_IsException(view)) {
        JS_FreeValue(ctx, req);
        return wst_reject_exc(ctx);
    }
    do
        n = b && len ? read(k->fd, b, len) : 0;
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        int e = errno;
        JS_FreeValue(ctx, view);
        JS_FreeValue(ctx, req);
        close_quietly(ctx, a->v[0]);
        JS_ThrowTypeError(ctx, "%s: read failed: %s", k->name, strerror(e));
        return wst_reject_exc(ctx);
    }
    if (JS_IsNull(req) && (size_t)n < len && (size_t)n * 2 >= len)
        memset(b + n, 0, len - (size_t)n);
    adp_set(ctx, a, 2, view);
    adp_set(ctx, a, 3, req);
    nv = JS_NewInt64(ctx, (int64_t)n);
    r = tors_rx(ctx, data, 0, 0, nv);
    if (JS_IsException(r))
        return wst_reject_exc(ctx);
    return wst_resolved_undef(ctx);
}

static JSValue tors_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    SRBSC* c = a->rs->c;
    JSValue req, view, r, p, q, fn = JS_UNDEFINED;
    (void)argc;
    (void)argv;
    if (a->taken)
        return wst_resolved_undef(ctx);
    req = wst_rbsc_get_byob_request(ctx, c);
    if (JS_IsException(req))
        return wst_reject_exc(ctx);
    if (adp_is(a->v[0], AK_FDSRC)) {
        SAdp* k = adp_of(a->v[0]);
        fn = JS_GetPropertyStr(ctx, a->v[0], "read");
        if (JS_IsException(fn)) {
            JS_FreeValue(ctx, req);
            return wst_reject_exc(ctx);
        }
        if (JS_VALUE_GET_PTR(fn) == JS_VALUE_GET_PTR(k->v[4]) && k->sync && !k->closed && !k->pend) {
            JS_FreeValue(ctx, fn);
            return fdrs_read_sync(ctx, data, a, k, req);
        }
    }
    view = JS_IsNull(req) ? u8_zero(ctx, a->n0) : wst_byobreq_view(ctx, req);
    if (JS_IsException(view)) {
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, req);
        return wst_reject_exc(ctx);
    }
    r = JS_IsUndefined(fn) ? call_method(ctx, a->v[0], "read", 1, (JSValueConst*)&view) : JS_Call(ctx, fn, a->v[0], 1, (JSValueConst*)&view);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r)) {
        JSValue e;
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, view);
            JS_FreeValue(ctx, req);
            return JS_EXCEPTION;
        }
        e = JS_GetException(ctx);
        close_quietly(ctx, a->v[0]);
        p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, view);
        JS_FreeValue(ctx, req);
        return p;
    }
    p = JS_PromiseResolveValue(ctx, r);
    JS_FreeValue(ctx, r);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, view);
        JS_FreeValue(ctx, req);
        return wst_reject_exc(ctx);
    }
    adp_set(ctx, a, 2, view);
    adp_set(ctx, a, 3, req);
    q = wst_then(ctx, p, tors_rx, data, 0);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue src_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    if (close_quietly(ctx, adp_of(data)->v[0]) < 0)
        return JS_EXCEPTION;
    return wst_resolved_undef(ctx);
}

static JSValue to_readable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue d, v, adp, obj;
    double cs = 65536, hwm = 0;
    SAlgo pull, cancel;
    SAdp* a;
    SRS* s;
    int ok;
    (void)this_val;
    ok = has_method(ctx, src, "read");
    if (ok < 0)
        return JS_EXCEPTION;
    if (!ok)
        return wst_throw(ctx, 0, "toReadableStream: src must be a ByteSource (an object with read(buf))");
    if (wst_to_dict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "options", &d) < 0)
        return JS_EXCEPTION;
    v = dict_str(ctx, d, "chunkSize");
    if (JS_IsException(v) || (!JS_IsUndefined(v) && wst_enforce_ull(ctx, v, "options.chunkSize", &cs) < 0))
        goto fail_v;
    JS_FreeValue(ctx, v);
    if (cs == 0 || cs > 1073741824.0) {
        JS_FreeValue(ctx, d);
        return wst_throw(ctx, 1, "toReadableStream: chunkSize must be in 1..2^30");
    }
    v = wst_dict_get(ctx, d, WSA_highWaterMark);
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToFloat64(ctx, &hwm, v) < 0))
        goto fail_v;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, d);
    if (isnan(hwm) || hwm < 0)
        return wst_throw(ctx, 1, "toReadableStream: highWaterMark must be a non-negative number");
    adp = adp_new(ctx, AK_TORS, &a);
    if (JS_IsException(adp))
        return adp;
    a->v[0] = JS_DupValue(ctx, src);
    a->n0 = cs;
    sa_init(&pull);
    sa_init(&cancel);
    sa_set_c(ctx, &pull, tors_pull, adp);
    sa_set_c(ctx, &cancel, src_cancel, adp);
    obj = wst_rs_new(ctx, JS_UNDEFINED, &s);
    if (JS_IsException(obj) || wst_rbsc_setup(ctx, s, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, &pull, &cancel, hwm, 0, 0) < 0) {
        sa_clear(ctx, &pull);
        sa_clear(ctx, &cancel);
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    a->rs = s;
    a->v[1] = JS_DupValue(ctx, obj);
    ((SRBSC*)s->c)->started = 1;
    JS_FreeValue(ctx, s->native);
    s->native = adp;
    return obj;
fail_v:
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

static JSValue tows_step_fn(JSContext* ctx, JSValueConst data, JSValueConst fn);

static JSValue tows_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    double n;
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    if (JS_ToFloat64(ctx, &n, arg) < 0)
        return JS_EXCEPTION;
    if (!(n > 0) || n > a->n2)
        return wst_throw(ctx, 0, "ByteSink.write accepted a byte count outside [1, buf.length]");
    a->n1 += n;
    return tows_step_fn(ctx, data, JS_UNDEFINED);
}

static JSValue tows_step_fn(JSContext* ctx, JSValueConst data, JSValueConst fn)
{
    SAdp* a = adp_of(data);
    double len = u8_len(ctx, a->v[2]);
    JSValue view, r, p, q;
    if (a->n1 >= len)
        return JS_UNDEFINED;
    view = a->n1 == 0 ? JS_DupValue(ctx, a->v[2]) : u8_sub(ctx, a->v[2], a->n1, len - a->n1);
    if (JS_IsException(view))
        return view;
    a->n2 = len - a->n1;
    r = JS_IsUndefined(fn) ? call_method(ctx, a->v[0], "write", 1, (JSValueConst*)&view) : JS_Call(ctx, fn, a->v[0], 1, (JSValueConst*)&view);
    JS_FreeValue(ctx, view);
    if (JS_IsException(r))
        return r;
    p = JS_PromiseResolveValue(ctx, r);
    JS_FreeValue(ctx, r);
    if (JS_IsException(p))
        return p;
    q = wst_then(ctx, p, tows_rx, data, 0);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue promise_of(JSContext* ctx, JSValue r)
{
    JSValue p;
    if (JS_IsException(r))
        return wst_reject_exc(ctx);
    p = JS_PromiseResolveValue(ctx, r);
    JS_FreeValue(ctx, r);
    if (JS_IsException(p))
        return wst_reject_exc(ctx);
    return p;
}

static JSValue tows_write_fn(JSContext* ctx, JSValueConst data, JSValueConst c, JSValueConst fn)
{
    SAdp* a = adp_of(data);
    JSValue bytes;
    if (wst_bytes_of_chunk(ctx, c, "WritableStream chunk", &bytes) < 0)
        return wst_reject_exc(ctx);
    adp_set(ctx, a, 2, bytes);
    a->n1 = 0;
    return promise_of(ctx, tows_step_fn(ctx, data, fn));
}

int (*dyn_sink_direct)(JSContext* ctx, JSValueConst sink, JSValueConst write_fn, const uint8_t* p, size_t n);
JSClassID dyn_sink_direct_cid;

static const uint8_t* tows_peek(JSContext* ctx, JSValueConst c, size_t* n, JSValue* hold)
{
    WVInfo vi;
    size_t sz = 0;
    uint8_t* base;
    *n = 0;
    *hold = JS_UNDEFINED;
    if (JS_IsString(c))
        return NULL;
    if (wst_is_ab(c)) {
        if (wst_is_detached(c))
            return NULL;
        return JS_PeekArrayBuffer(c, n);
    }
    if (wst_view_info(ctx, c, &vi) <= 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    if (!wst_is_ab(vi.buffer) || wst_is_detached(vi.buffer)) {
        JS_FreeValue(ctx, vi.buffer);
        return NULL;
    }
    base = JS_PeekArrayBuffer(vi.buffer, &sz);
    if (!base || vi.off + vi.len > (double)sz) {
        JS_FreeValue(ctx, vi.buffer);
        return NULL;
    }
    *hold = vi.buffer;
    *n = (size_t)vi.len;
    return base + (size_t)vi.off;
}

static JSValue fdsink_step(JSContext* ctx, JSValueConst data);
static JSValue fdsink_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg);

static JSValue fdws_park(JSContext* ctx, JSValueConst kv, SAdp* k, const uint8_t* b, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, b, n), rem, q, kk, t;
    if (JS_IsException(ab))
        return wst_reject_exc(ctx);
    rem = wst_new_u8(ctx, ab, 0, (double)n);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(rem))
        return wst_reject_exc(ctx);
    adp_set(ctx, k, 3, JS_DupValue(ctx, rem));
    k->n1 = 0;
    q = promise_of(ctx, fdsink_step(ctx, kv));
    kk = adp_call(ctx, kv, rem);
    JS_FreeValue(ctx, rem);
    if (JS_IsException(kk)) {
        JS_FreeValue(ctx, q);
        return wst_reject_exc(ctx);
    }
    t = wst_then(ctx, q, fdsink_rx, kk, 1);
    JS_FreeValue(ctx, kk);
    if (!JS_IsException(t))
        adp_set(ctx, k, 0, t);
    else
        wst_drop_exc(ctx);
    return q;
}

static void fdws_write_u(JSContext* ctx, JSValueConst data, SAdp* a, JSValueConst c, JSValueConst rx)
{
    JSValueConst kv = a->v[0];
    SAdp* k = adp_of(kv);
    JSValue p, fn, hold, bytes = JS_UNDEFINED, e;
    const uint8_t* b;
    size_t n = 0, off = 0;
    int err = 0;
    fn = JS_GetProperty(ctx, kv, wst_atom(ctx, WSA_write));
    if (JS_IsException(fn)) {
        p = wst_reject_exc(ctx);
        goto react;
    }
    if (JS_VALUE_GET_PTR(fn) != JS_VALUE_GET_PTR(k->v[4]) || k->closed || k->flag || JS_PromiseState(ctx, k->v[0]) != JS_PROMISE_FULFILLED) {
        p = tows_write_fn(ctx, data, c, fn);
        JS_FreeValue(ctx, fn);
        goto react;
    }
    JS_FreeValue(ctx, fn);
    b = tows_peek(ctx, c, &n, &hold);
    if (!b) {
        if (wst_bytes_of_chunk(ctx, c, "WritableStream chunk", &bytes) < 0) {
            p = wst_reject_exc(ctx);
            goto react;
        }
        b = u8_ptr(ctx, bytes, &n);
    }
    if (n == 0) {
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, bytes);
        JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
        return;
    }
    err = dyn_stdio_write(k->fd, b, n, &off);
    if (err == 0) {
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, bytes);
        JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
        return;
    }
    if (err < 0) {
        p = fdws_park(ctx, kv, k, b + off, n - off);
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, bytes);
        goto react;
    }
    JS_FreeValue(ctx, hold);
    JS_FreeValue(ctx, bytes);
    JS_ThrowTypeError(ctx, "%s: write failed: %s", k->name, strerror(err));
    e = JS_GetException(ctx);
    k->flag = 1;
    adp_set(ctx, k, 2, JS_DupValue(ctx, e));
    JS_EnqueueReaction(ctx, rx, e, 1);
    JS_FreeValue(ctx, e);
    return;
react:
    if (!JS_IsException(p))
        JS_PromiseReact(ctx, p, rx);
    JS_FreeValue(ctx, p);
}

static void tows_write_u(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv, JSValueConst rx)
{
    SAdp* a = adp_of(data);
    JSValueConst c = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue p, bytes = JS_UNDEFINED, hold, fn;
    const uint8_t* b;
    size_t n = 0;
    int r = 0;
    if (adp_is(a->v[0], AK_FDSINK)) {
        fdws_write_u(ctx, data, a, c, rx);
        return;
    }
    if (!dyn_sink_direct || JS_GetClassID(a->v[0]) != dyn_sink_direct_cid) {
        p = tows_write_fn(ctx, data, c, JS_UNDEFINED);
        goto react;
    }
    fn = JS_GetProperty(ctx, a->v[0], wst_atom(ctx, WSA_write));
    if (JS_IsException(fn)) {
        p = wst_reject_exc(ctx);
        goto react;
    }
    b = tows_peek(ctx, c, &n, &hold);
    if (!b) {
        if (wst_bytes_of_chunk(ctx, c, "WritableStream chunk", &bytes) < 0) {
            JS_FreeValue(ctx, fn);
            p = wst_reject_exc(ctx);
            goto react;
        }
        b = u8_ptr(ctx, bytes, &n);
    }
    if (b && n == 0) {
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, bytes);
        JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
        return;
    }
    if (b)
        r = dyn_sink_direct(ctx, a->v[0], fn, b, n);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, hold);
    if (r == 0) {
        if (JS_IsUndefined(bytes) && wst_bytes_of_chunk(ctx, c, "WritableStream chunk", &bytes) < 0) {
            p = wst_reject_exc(ctx);
            goto react;
        }
        adp_set(ctx, a, 2, bytes);
        a->n1 = 0;
        p = promise_of(ctx, tows_step_fn(ctx, data, JS_UNDEFINED));
        goto react;
    }
    JS_FreeValue(ctx, bytes);
    if (r > 0) {
        JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
        return;
    }
    p = wst_reject_exc(ctx);
react:
    if (!JS_IsException(p))
        JS_PromiseReact(ctx, p, rx);
    JS_FreeValue(ctx, p);
}

static JSValue tows_close_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    JSValue r, p, q;
    int ok;
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    ok = has_method(ctx, a->v[0], "finish");
    if (ok < 0)
        return JS_EXCEPTION;
    if (ok) {
        r = call_method(ctx, a->v[0], "finish", 0, NULL);
        if (JS_IsException(r))
            return r;
        p = JS_PromiseResolveValue(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(p))
            return p;
        q = wst_transform_undef(ctx, p);
        JS_FreeValue(ctx, p);
        return q;
    }
    close_quietly(ctx, a->v[0]);
    return JS_UNDEFINED;
}

static JSValue tows_close(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue r = JS_UNDEFINED, p, q;
    int ok;
    (void)argc;
    (void)argv;
    ok = has_method(ctx, a->v[0], "flush");
    if (ok < 0)
        return wst_reject_exc(ctx);
    if (ok) {
        r = call_method(ctx, a->v[0], "flush", 0, NULL);
        if (JS_IsException(r))
            return wst_reject_exc(ctx);
    }
    p = promise_of(ctx, r);
    q = wst_then(ctx, p, tows_close_rx, data, 0);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue to_writable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst sink = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue d, v, adp, obj;
    SAlgo wr, cl, ab;
    double hwm = 1;
    SAdp* a;
    SWS* s;
    int ok;
    (void)this_val;
    ok = has_method(ctx, sink, "write");
    if (ok < 0)
        return JS_EXCEPTION;
    if (!ok)
        return wst_throw(ctx, 0, "toWritableStream: sink must be a ByteSink (an object with write(buf))");
    if (wst_to_dict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "options", &d) < 0)
        return JS_EXCEPTION;
    v = wst_dict_get(ctx, d, WSA_highWaterMark);
    JS_FreeValue(ctx, d);
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToFloat64(ctx, &hwm, v) < 0)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (isnan(hwm) || hwm < 0)
        return wst_throw(ctx, 1, "toWritableStream: highWaterMark must be a non-negative number");
    adp = adp_new(ctx, AK_TOWS, &a);
    if (JS_IsException(adp))
        return adp;
    a->v[0] = JS_DupValue(ctx, sink);
    sa_init(&wr);
    sa_init(&cl);
    sa_init(&ab);
    sa_set_u(ctx, &wr, tows_write_u, adp);
    sa_set_c(ctx, &cl, tows_close, adp);
    sa_set_c(ctx, &ab, src_cancel, adp);
    obj = wst_ws_create(ctx, &wr, &cl, &ab, JS_UNDEFINED, hwm, JS_UNDEFINED, 0, &s);
    sa_clear(ctx, &wr);
    sa_clear(ctx, &cl);
    sa_clear(ctx, &ab);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, adp);
        return obj;
    }
    a->ws = s;
    a->v[1] = JS_DupValue(ctx, obj);
    s->c->started = 1;
    JS_FreeValue(ctx, s->native);
    s->native = adp;
    return obj;
}

static JSValue fromrs_fill(JSContext* ctx, SAdp* a, JSValueConst out)
{
    size_t ol, ll, n;
    uint8_t* op = u8_ptr(ctx, out, &ol);
    uint8_t* lp = u8_ptr(ctx, a->v[1], &ll);
    if (!op || !lp)
        return JS_ThrowTypeError(ctx, "the buffer is detached");
    n = ll < ol ? ll : ol;
    memmove(op, lp, n);
    if (n == ll)
        adp_set(ctx, a, 1, JS_UNDEFINED);
    else {
        JSValue rest = u8_sub(ctx, a->v[1], (double)n, (double)(ll - n));
        if (JS_IsException(rest))
            return rest;
        adp_set(ctx, a, 1, rest);
    }
    return JS_NewFloat64(ctx, (double)n);
}

static JSValue fromrs_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* k = adp_of(data);
    SAdp* a = adp_of(k->v[0]);
    JSValue dv, val, left;
    int done;
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    dv = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_done));
    if (JS_IsException(dv))
        return dv;
    done = JS_ToBool(ctx, dv);
    JS_FreeValue(ctx, dv);
    if (done) {
        a->eof = 1;
        return JS_NewInt32(ctx, 0);
    }
    val = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_value));
    if (JS_IsException(val))
        return val;
    if (wst_bytes_of_chunk(ctx, val, "ReadableStream chunk", &left) < 0) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, val);
    if (u8_len(ctx, left) == 0) {
        JS_FreeValue(ctx, left);
        adp_set(ctx, a, 1, JS_UNDEFINED);
        return call_method(ctx, a->v[2], "read", 1, (JSValueConst*)&k->v[1]);
    }
    adp_set(ctx, a, 1, left);
    return fromrs_fill(ctx, a, k->v[1]);
}

static JSValue fromrs_read(JSContext* ctx, JSValueConst adp, SAdp* a, JSValueConst buf)
{
    WVInfo vi;
    JSValue p, k, q, n;
    int r = wst_view_info(ctx, buf, &vi);
    if (r < 0)
        return wst_reject_exc(ctx);
    if (r > 0)
        JS_FreeValue(ctx, vi.buffer);
    if (r == 0 || vi.esize != 1)
        return wst_reject_err(ctx, 0, "ByteSource.read: buf must be a byte-wide view (Uint8Array)");
    if (a->closed)
        return wst_reject_err(ctx, 0, "use of a closed native resource");
    if (vi.len == 0)
        return resolved_num(ctx, 0);
    if (!JS_IsUndefined(a->v[1])) {
        n = fromrs_fill(ctx, a, buf);
        if (JS_IsException(n))
            return wst_reject_exc(ctx);
        p = resolved(ctx, n);
        JS_FreeValue(ctx, n);
        return p;
    }
    if (a->eof)
        return resolved_num(ctx, 0);
    p = wst_reader_read_promise(ctx, a->rd);
    if (JS_IsException(p))
        return wst_reject_exc(ctx);
    k = adp_call(ctx, adp, buf);
    q = JS_IsException(k) ? JS_EXCEPTION : wst_then(ctx, p, fromrs_rx, k, 0);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, p);
    return q;
}

static void fromrs_close(JSContext* ctx, SAdp* a)
{
    if (a->closed)
        return;
    a->closed = 1;
    if (a->rd->s) {
        mark_handled_free(ctx, wst_rs_cancel(ctx, a->rd->s, JS_UNDEFINED));
        if (a->rd->s)
            wst_reader_release(ctx, a->rd);
    }
}

static JSValue from_readable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_rs_of(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "fromReadableStream: rs");
    JSValue robj, adp, src;
    SReader* rd;
    SAdp* a;
    (void)this_val;
    if (!s)
        return JS_EXCEPTION;
    robj = wst_rs_acquire_reader(ctx, s, 0, &rd);
    if (JS_IsException(robj))
        return robj;
    adp = adp_new(ctx, AK_FROMRS, &a);
    if (JS_IsException(adp)) {
        JS_FreeValue(ctx, robj);
        return adp;
    }
    a->v[0] = robj;
    a->rd = rd;
    src = io_object(ctx, adp, 0);
    if (!JS_IsException(src))
        a->v[2] = JS_DupValue(ctx, src);
    JS_FreeValue(ctx, adp);
    return src;
}

static JSValue fdsrc_read(JSContext* ctx, JSValueConst adp, SAdp* a, JSValueConst buf);
static void fdsrc_close(JSContext* ctx, SAdp* a);
static JSValue fdsink_write(JSContext* ctx, JSValueConst adp, SAdp* a, JSValueConst buf);
static JSValue fdsink_flush(JSContext* ctx, JSValueConst adp, SAdp* a);

static JSValue io_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fd)
{
    JSValueConst adp = fd[0];
    SAdp* a = adp_of(adp);
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue pin, r = JS_UNDEFINED;
    if (magic == IO_DISPOSE)
        return call_method(ctx, this_val, "close", 0, NULL);
    if (magic == IO_CLOSED)
        return JS_NewBool(ctx, a->closed);
    pin = JS_DupValue(ctx, adp);
    switch (a->kind * 8 + magic) {
    case AK_FROMRS * 8 + IO_READ:
        r = fromrs_read(ctx, adp, a, x);
        break;
    case AK_FROMRS * 8 + IO_CLOSE:
        fromrs_close(ctx, a);
        break;
    case AK_FDSRC * 8 + IO_READ:
        r = fdsrc_read(ctx, adp, a, x);
        break;
    case AK_FDSRC * 8 + IO_CLOSE:
        fdsrc_close(ctx, a);
        break;
    case AK_FDSINK * 8 + IO_WRITE:
        r = fdsink_write(ctx, adp, a, x);
        break;
    case AK_FDSINK * 8 + IO_FLUSH:
        r = fdsink_flush(ctx, adp, a);
        break;
    case AK_FDSINK * 8 + IO_CLOSE:
        a->closed = 1;
        break;
    default:
        break;
    }
    JS_FreeValue(ctx, pin);
    return r;
}

static JSValue body_stream(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst bytes = argc > 0 ? argv[0] : JS_UNDEFINED;
    SAlgo pull, cancel;
    SRBSC* c;
    SRS* s;
    JSValue obj;
    (void)this_val;
    sa_init(&pull);
    sa_init(&cancel);
    obj = wst_rbs_create(ctx, JS_UNDEFINED, &pull, &cancel, &s);
    if (JS_IsException(obj))
        return obj;
    c = s->c;
    c->started = 1;
    if ((u8_len(ctx, bytes) > 0 && wst_rbsc_enqueue(ctx, c, bytes) < 0) || wst_rbsc_close(ctx, c) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (argc > 1 && JS_ToBool(ctx, argv[1]))
        s->disturbed = 1;
    return obj;
}

static void body_step(JSContext* ctx, JSValueConst adp);

static JSValue body_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    JSValue dv, val, out, e;
    WVInfo vi;
    uint32_t i;
    size_t off = 0, len;
    uint8_t *dst, *src;
    int done, r;
    (void)magic;
    if (is_reject) {
        if (a->rd->s)
            wst_reader_release(ctx, a->rd);
        sdef_reject(ctx, &a->d, arg);
        return JS_UNDEFINED;
    }
    dv = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_done));
    done = JS_ToBool(ctx, dv);
    JS_FreeValue(ctx, dv);
    if (done) {
        if (a->rd->s)
            wst_reader_release(ctx, a->rd);
        out = u8_zero(ctx, a->n0);
        if (JS_IsException(out)) {
            e = JS_GetException(ctx);
            sdef_reject(ctx, &a->d, e);
            JS_FreeValue(ctx, e);
            return JS_UNDEFINED;
        }
        dst = u8_ptr(ctx, out, &len);
        for (i = 0; i < (uint32_t)a->n1; i++) {
            JSValue part = JS_GetPropertyUint32(ctx, a->v[1], i);
            size_t pl;
            src = u8_ptr(ctx, part, &pl);
            if (src && dst && off + pl <= len)
                memcpy(dst + off, src, pl);
            off += pl;
            JS_FreeValue(ctx, part);
        }
        sdef_resolve(ctx, &a->d, out);
        JS_FreeValue(ctx, out);
        return JS_UNDEFINED;
    }
    val = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_value));
    r = wst_view_info(ctx, val, &vi);
    if (r > 0)
        JS_FreeValue(ctx, vi.buffer);
    if (r <= 0 || vi.type != JS_TYPED_ARRAY_UINT8) {
        if (r < 0)
            wst_drop_exc(ctx);
        JS_FreeValue(ctx, val);
        e = type_err(ctx, "body stream chunks must be Uint8Array");
        if (a->rd->s)
            mark_handled_free(ctx, wst_rs_cancel(ctx, a->rd->s, e));
        if (a->rd->s)
            wst_reader_release(ctx, a->rd);
        sdef_reject(ctx, &a->d, e);
        JS_FreeValue(ctx, e);
        return JS_UNDEFINED;
    }
    JS_DefinePropertyValueUint32(ctx, a->v[1], (uint32_t)a->n1, val, JS_PROP_C_W_E);
    a->n1 += 1;
    a->n0 += vi.len;
    body_step(ctx, data);
    return JS_UNDEFINED;
}

static void body_step(JSContext* ctx, JSValueConst adp)
{
    SAdp* a = adp_of(adp);
    JSValue p = wst_reader_read_promise(ctx, a->rd);
    if (JS_IsException(p)) {
        JSValue e = JS_GetException(ctx);
        sdef_reject(ctx, &a->d, e);
        JS_FreeValue(ctx, e);
        return;
    }
    if (react(ctx, p, body_rx, adp, 0) < 0)
        wst_drop_exc(ctx);
    JS_FreeValue(ctx, p);
}

static JSValue body_read_all(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst rs = argc > 0 ? argv[0] : JS_UNDEFINED;
    SRS* s = wst_opaque(rs, WSTC_RS);
    JSValue robj, adp, r;
    SAdp* a;
    (void)this_val;
    if (!s)
        return wst_reject_err(ctx, 0, "body is not a ReadableStream");
    if (wst_rs_locked(s) || s->disturbed)
        return wst_reject_err(ctx, 0, "body stream is locked or disturbed");
    adp = adp_new(ctx, AK_BODY, &a);
    if (JS_IsException(adp))
        return wst_reject_exc(ctx);
    robj = wst_rs_acquire_reader(ctx, s, 0, &a->rd);
    if (JS_IsException(robj) || sdef_new(ctx, &a->d) < 0) {
        JS_FreeValue(ctx, robj);
        JS_FreeValue(ctx, adp);
        return wst_reject_exc(ctx);
    }
    a->v[0] = robj;
    a->v[1] = JS_NewArray(ctx);
    body_step(ctx, adp);
    r = JS_DupValue(ctx, a->d.p);
    JS_FreeValue(ctx, adp);
    return r;
}

static SRS* rs_arg(JSContext* ctx, int argc, JSValueConst* argv)
{
    return wst_rs_of(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "stream");
}

static JSValue is_disturbed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = rs_arg(ctx, argc, argv);
    (void)this_val;
    return s ? JS_NewBool(ctx, s->disturbed) : JS_EXCEPTION;
}

static JSValue is_locked(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = rs_arg(ctx, argc, argv);
    (void)this_val;
    return s ? JS_NewBool(ctx, wst_rs_locked(s)) : JS_EXCEPTION;
}

static JSValue tee_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = rs_arg(ctx, argc, argv);
    JSValue out[2], arr;
    (void)this_val;
    if (!s || wst_rs_tee(ctx, s, out) < 0)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    JS_DefinePropertyValueUint32(ctx, arr, 0, out[0], JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, arr, 1, out[1], JS_PROP_C_W_E);
    return arr;
}

static JSValue rs_of_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc > 0 && wst_opaque(argv[0], WSTC_RS))
        return JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue fast_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    SRS* s = a->rs;
    SWS* d = a->ws;
    SRBSC* rc = s->c;
    if (magic == 0 && !is_reject) {
        JSValue q;
        if (close_quietly(ctx, a->v[2]) < 0)
            return JS_EXCEPTION;
        if (s->state == RS_READABLE) {
            sa_clear(ctx, &rc->pull);
            sa_clear(ctx, &rc->cancel);
            wst_rs_close(ctx, s);
        }
        q = wst_writer_close_with_error_propagation(ctx, a->wr);
        if (JS_IsException(q))
            return JS_EXCEPTION;
        react(ctx, q, fast_rx, data, 1);
        JS_FreeValue(ctx, q);
        return JS_UNDEFINED;
    }
    if (magic == 0) {
        close_quietly(ctx, a->v[2]);
        close_quietly(ctx, a->v[3]);
        wst_rbsc_error(ctx, rc, arg);
        wst_wsdc_error_if_needed(ctx, d->c, arg);
        mark_handled_free(ctx, wst_ws_abort(ctx, d, arg));
    }
    if (a->wr->s)
        wst_writer_release(ctx, a->wr);
    if (a->rd->s)
        wst_reader_release(ctx, a->rd);
    if (is_reject)
        sdef_reject(ctx, &a->d, arg);
    else
        sdef_resolve(ctx, &a->d, JS_UNDEFINED);
    return JS_UNDEFINED;
}

JSValue wst_fast_pipe(JSContext* ctx, SRS* s, SWS* d, int prevent_close, int prevent_abort, int prevent_cancel)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    SAdp *src = adp_of(s->native), *sink = adp_of(d->native), *a;
    SRBSC* rc = s->c;
    SWSDC* wc = d->c;
    JSValue adp, args[2], p, r;
    if (prevent_close || prevent_abort || prevent_cancel || !JS_IsFunction(ctx, w->x[WSX_FAST_PIPE]) || !src || !sink)
        return JS_UNDEFINED;
    if (!s->bytes || s->state != RS_READABLE || rc->qtotal != 0 || rc->pending.len != 0 || rc->close_requested || !rc->started || rc->pulling)
        return JS_UNDEFINED;
    if (d->state != WS_WRITABLE || wc->q.len != 0 || wst_ws_has_op_in_flight(d) || wst_ws_close_queued_or_in_flight(d) || !wc->started)
        return JS_UNDEFINED;
    adp = adp_new(ctx, AK_FAST, &a);
    if (JS_IsException(adp))
        return adp;
    a->v[0] = wst_rs_acquire_reader(ctx, s, 0, &a->rd);
    if (JS_IsException(a->v[0])) {
        a->v[0] = JS_UNDEFINED;
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    a->v[1] = wst_ws_acquire_writer(ctx, d, &a->wr);
    if (JS_IsException(a->v[1]) || sdef_new(ctx, &a->d) < 0) {
        if (JS_IsException(a->v[1]))
            a->v[1] = JS_UNDEFINED;
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    a->v[2] = JS_DupValue(ctx, src->v[0]);
    a->v[3] = JS_DupValue(ctx, sink->v[0]);
    a->v[4] = JS_DupValue(ctx, s->obj);
    a->v[5] = JS_DupValue(ctx, d->obj);
    a->rs = s;
    a->ws = d;
    s->disturbed = 1;
    src->taken = 1;
    args[0] = a->v[2];
    args[1] = a->v[3];
    r = JS_Call(ctx, w->x[WSX_NATIVE_PIPE], JS_UNDEFINED, 2, (JSValueConst*)args);
    if (!JS_IsException(r) && JS_IsUndefined(r))
        r = JS_Call(ctx, w->x[WSX_FAST_PIPE], JS_UNDEFINED, 2, (JSValueConst*)args);
    p = promise_of(ctx, r);
    react(ctx, p, fast_rx, adp, 0);
    JS_FreeValue(ctx, p);
    r = JS_DupValue(ctx, a->d.p);
    JS_FreeValue(ctx, adp);
    return wst_ret(ctx, r);
}

static JSValue sock_gone_err(JSContext* ctx)
{
    return type_err(ctx, "TCPConn: the connection closed");
}

static void sock_conn(JSContext* ctx, SAdp* a, const char* m)
{
    call_drop(ctx, call_method(ctx, a->v[0], m, 0, NULL));
}

static void sock_finish(JSContext* ctx, SAdp* a)
{
    if (a->read_done && a->write_done && !a->done)
        sock_conn(ctx, a, "close");
}

static void sock_wake(JSContext* ctx, SAdp* a, JSValueConst e)
{
    SDef d = a->d;
    if (JS_IsUndefined(d.p))
        return;
    sdef_init(&a->d);
    if (JS_IsUndefined(e))
        sdef_resolve(ctx, &d, JS_UNDEFINED);
    else
        sdef_reject(ctx, &d, e);
    sdef_free(JS_GetRuntime(ctx), &d);
}

static JSValue sock_buffered(JSContext* ctx, SAdp* a, double* q)
{
    JSValue v = JS_GetPropertyStr(ctx, a->v[0], "bufferedAmount");
    if (JS_IsException(v))
        return v;
    if (JS_ToFloat64(ctx, q, v) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    return JS_UNDEFINED;
}

static JSValue sock_wait_drain(JSContext* ctx, SAdp* a)
{
    double q;
    if (a->done) {
        JSValue e = sock_gone_err(ctx), p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        return p;
    }
    if (JS_IsException(sock_buffered(ctx, a, &q)))
        return wst_reject_exc(ctx);
    if (q == 0)
        return wst_resolved_undef(ctx);
    if (JS_IsUndefined(a->d.p) && sdef_new(ctx, &a->d) < 0)
        return wst_reject_exc(ctx);
    return JS_DupValue(ctx, a->d.p);
}

static JSValue sock_step(JSContext* ctx, JSValueConst data);

static JSValue sock_step_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    return sock_step(ctx, data);
}

static JSValue sock_step(JSContext* ctx, JSValueConst data)
{
    SAdp* a = adp_of(data);
    JSValue pin = JS_DupValue(ctx, data), r = JS_UNDEFINED;
    double len = u8_len(ctx, a->v[3]), n, q;
    while (a->n1 < len) {
        JSValue chunk, wr;
        if (a->done) {
            r = JS_Throw(ctx, sock_gone_err(ctx));
            break;
        }
        n = len - a->n1 < a->n0 ? len - a->n1 : a->n0;
        if (JS_IsException(sock_buffered(ctx, a, &q))) {
            r = JS_EXCEPTION;
            break;
        }
        if (q > 0 && q + n > a->n0) {
            JSValue w = sock_wait_drain(ctx, a);
            r = wst_then(ctx, w, sock_step_rx, data, 0);
            JS_FreeValue(ctx, w);
            break;
        }
        chunk = a->n1 == 0 && n == len ? JS_DupValue(ctx, a->v[3]) : u8_sub(ctx, a->v[3], a->n1, n);
        if (JS_IsException(chunk)) {
            r = chunk;
            break;
        }
        wr = call_method(ctx, a->v[0], "write", 1, (JSValueConst*)&chunk);
        JS_FreeValue(ctx, chunk);
        if (JS_IsException(wr) && JS_HasUncatchableException(ctx)) {
            r = JS_EXCEPTION;
            break;
        }
        if (JS_IsException(wr)) {
            JSValue e = JS_GetException(ctx), m;
            const char* ms;
            if (!a->done)
                sock_conn(ctx, a, "close");
            m = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "message") : JS_UNDEFINED;
            ms = JS_IsException(m) ? NULL : JS_ToCString(ctx, m);
            if (!ms)
                wst_drop_exc(ctx);
            r = JS_Throw(ctx, wst_err(ctx, 0, "TCPConn: the connection closed (%s)", ms ? ms : "undefined"));
            JS_FreeCString(ctx, ms);
            JS_FreeValue(ctx, m);
            JS_FreeValue(ctx, e);
            break;
        }
        JS_FreeValue(ctx, wr);
        a->n1 += n;
    }
    JS_FreeValue(ctx, pin);
    return r;
}

static JSValue sock_write(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue bytes;
    if (wst_bytes_of_chunk(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WritableStream chunk", &bytes) < 0)
        return wst_reject_exc(ctx);
    adp_set(ctx, a, 3, bytes);
    a->n1 = 0;
    return promise_of(ctx, sock_step(ctx, data));
}

static JSValue sock_close_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    JSValue pin, r;
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    a->write_done = 1;
    if (a->done)
        return JS_UNDEFINED;
    pin = JS_DupValue(ctx, data);
    r = call_method(ctx, a->v[0], "end", 0, NULL);
    if (!JS_IsException(r)) {
        JS_FreeValue(ctx, r);
        r = JS_UNDEFINED;
        sock_finish(ctx, a);
    }
    JS_FreeValue(ctx, pin);
    return r;
}

static JSValue sock_close(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    JSValue w = sock_wait_drain(ctx, adp_of(data)), q;
    (void)argc;
    (void)argv;
    q = wst_then(ctx, w, sock_close_rx, data, 0);
    JS_FreeValue(ctx, w);
    return q;
}

static JSValue sock_abort(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin = JS_DupValue(ctx, data);
    (void)argc;
    (void)argv;
    a->read_done = a->write_done = 1;
    if (!a->done)
        sock_conn(ctx, a, "close");
    JS_FreeValue(ctx, pin);
    return wst_resolved_undef(ctx);
}

static JSValue sock_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin = JS_DupValue(ctx, data);
    (void)argc;
    (void)argv;
    if (!a->done)
        sock_conn(ctx, a, "resume");
    JS_FreeValue(ctx, pin);
    return wst_resolved_undef(ctx);
}

static JSValue sock_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin = JS_DupValue(ctx, data);
    (void)argc;
    (void)argv;
    a->read_done = 1;
    if (!a->done) {
        sock_conn(ctx, a, "pause");
        sock_finish(ctx, a);
    }
    JS_FreeValue(ctx, pin);
    return wst_resolved_undef(ctx);
}

static JSValue sock_drain_close(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* a = adp_of(data);
    (void)magic;
    (void)arg;
    if (!is_reject && !a->done)
        sock_conn(ctx, a, "close");
    return JS_UNDEFINED;
}

static int sock_read_end(JSContext* ctx, SAdp* a)
{
    SRBSC* rc = a->rs->c;
    if (a->rs->state == RS_READABLE && !rc->close_requested) {
        if (wst_rbsc_close(ctx, rc) < 0)
            return -1;
        if (rc->pending.len > 0 && wst_rbsc_respond(ctx, rc, 0) < 0)
            return -1;
    }
    return 0;
}

static JSValue sock_tap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fd)
{
    JSValueConst adp = fd[0];
    SAdp* a = adp_of(adp);
    SRBSC* rc = a->rs->c;
    JSValue pin = JS_DupValue(ctx, adp), r = JS_UNDEFINED, e;
    int kind = 0;
    (void)this_val;
    (void)magic;
    if (argc > 0 && JS_ToInt32(ctx, &kind, argv[0]) < 0) {
        JS_FreeValue(ctx, pin);
        return JS_EXCEPTION;
    }
    if (kind == 0) {
        if (a->read_done || a->rs->state != RS_READABLE || rc->close_requested)
            sock_conn(ctx, a, "pause");
        else if (wst_rbsc_enqueue(ctx, rc, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
            r = JS_EXCEPTION;
        else {
            JSValue ds = wst_rbsc_desired(ctx, rc);
            double v = 1;
            if (JS_IsException(ds) || (!JS_IsNull(ds) && JS_ToFloat64(ctx, &v, ds) < 0))
                r = JS_EXCEPTION;
            else if (JS_IsNull(ds) || v <= 0)
                sock_conn(ctx, a, "pause");
            JS_FreeValue(ctx, ds);
        }
    } else if (kind == 1) {
        sock_wake(ctx, a, JS_UNDEFINED);
    } else if (kind == 3) {
        if (sock_read_end(ctx, a) < 0)
            r = JS_EXCEPTION;
        else {
            a->read_done = 1;
            if (a->half_open)
                sock_finish(ctx, a);
            else {
                JSValue w = sock_wait_drain(ctx, a);
                react(ctx, w, sock_drain_close, adp, 0);
                JS_FreeValue(ctx, w);
            }
        }
    } else {
        a->done = 1;
        if (sock_read_end(ctx, a) < 0)
            r = JS_EXCEPTION;
        else {
            e = sock_gone_err(ctx);
            sock_wake(ctx, a, e);
            JS_FreeValue(ctx, e);
            if (a->ws->state == WS_WRITABLE && !a->write_done) {
                e = sock_gone_err(ctx);
                wst_wsdc_error_if_needed(ctx, a->ws->c, e);
                JS_FreeValue(ctx, e);
            }
        }
    }
    JS_FreeValue(ctx, pin);
    return r;
}

static JSValue socket_streams(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue d, v, adp, rs, ws, tap, out;
    double rhwm = 65536, piece = 65536, conn_hwm = 0;
    SAlgo pull, cancel, wr, cl, ab;
    int half;
    SAdp* a;
    SRS* s;
    SWS* w;
    (void)this_val;
    if (wst_to_dict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "options", &d) < 0)
        return JS_EXCEPTION;
    v = dict_str(ctx, d, "readableHighWaterMark");
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToFloat64(ctx, &rhwm, v) < 0))
        goto fail_v;
    JS_FreeValue(ctx, v);
    v = dict_str(ctx, d, "writableHighWaterMark");
    if (JS_IsException(v))
        goto fail_v;
    if (isnan(rhwm) || rhwm < 0) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, d);
        return wst_throw(ctx, 1, "TCPConn.streams: readableHighWaterMark must be a non-negative number");
    }
    if (!JS_IsUndefined(v) && wst_enforce_ull(ctx, v, "options.writableHighWaterMark", &piece) < 0)
        goto fail_v;
    JS_FreeValue(ctx, v);
    if (piece == 0 || piece > 1073741824.0) {
        JS_FreeValue(ctx, d);
        return wst_throw(ctx, 1, "TCPConn.streams: writableHighWaterMark must be in 1..2^30");
    }
    conn_hwm = 16384;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToFloat64(ctx, &conn_hwm, argv[2]) < 0) {
            JS_FreeValue(ctx, d);
            return JS_EXCEPTION;
        }
        if (!(conn_hwm >= 1)) {
            JS_FreeValue(ctx, d);
            return wst_throw(ctx, 1, "TCPConn.streams: the connection highWaterMark must be at least 1");
        }
    }
    if (piece > conn_hwm)
        piece = conn_hwm;
    v = dict_str(ctx, d, "allowHalfOpen");
    JS_FreeValue(ctx, d);
    if (JS_IsException(v))
        return v;
    half = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    adp = adp_new(ctx, AK_SOCK, &a);
    if (JS_IsException(adp))
        return adp;
    a->v[0] = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    a->n0 = piece;
    a->half_open = half;
    sa_init(&pull);
    sa_init(&cancel);
    sa_set_c(ctx, &pull, sock_pull, adp);
    sa_set_c(ctx, &cancel, sock_cancel, adp);
    rs = wst_rs_new(ctx, JS_UNDEFINED, &s);
    if (JS_IsException(rs) || wst_rbsc_setup(ctx, s, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, &pull, &cancel, rhwm, 0, 0) < 0) {
        sa_clear(ctx, &pull);
        sa_clear(ctx, &cancel);
        JS_FreeValue(ctx, rs);
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    ((SRBSC*)s->c)->started = 1;
    a->rs = s;
    a->v[1] = JS_DupValue(ctx, rs);
    sa_init(&wr);
    sa_init(&cl);
    sa_init(&ab);
    sa_set_c(ctx, &wr, sock_write, adp);
    sa_set_c(ctx, &cl, sock_close, adp);
    sa_set_c(ctx, &ab, sock_abort, adp);
    ws = wst_ws_create(ctx, &wr, &cl, &ab, JS_UNDEFINED, 1, JS_UNDEFINED, 0, &w);
    sa_clear(ctx, &wr);
    sa_clear(ctx, &cl);
    sa_clear(ctx, &ab);
    if (JS_IsException(ws)) {
        JS_FreeValue(ctx, rs);
        JS_FreeValue(ctx, adp);
        return ws;
    }
    w->c->started = 1;
    a->ws = w;
    a->v[2] = JS_DupValue(ctx, ws);
    tap = JS_NewCFunctionData(ctx, sock_tap, 2, 0, 1, (JSValueConst*)&adp);
    out = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, out, "readable", rs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "writable", ws, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "tap", tap, JS_PROP_C_W_E);
    JS_FreeValue(ctx, adp);
    return out;
fail_v:
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, d);
    return JS_EXCEPTION;
}

static JSValue nat_call(JSContext* ctx, int which, int argc, JSValueConst* argv)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    return JS_Call(ctx, w->x[which], JS_UNDEFINED, argc, argv);
}

static void fd_watch(JSContext* ctx, int fd, int wr, JSValueConst fn)
{
    JSValueConst args[3];
    JSValue f = JS_NewInt32(ctx, fd), m = JS_NewInt32(ctx, wr);
    args[0] = f;
    args[1] = m;
    args[2] = fn;
    call_drop(ctx, nat_call(ctx, WSX_FD_WATCH, 3, args));
}

static void fdsrc_settle(JSContext* ctx, SAdp* a, int is_reject, JSValueConst v)
{
    SDef d = a->d;
    sdef_init(&a->d);
    a->pend = 0;
    adp_set(ctx, a, 1, JS_UNDEFINED);
    fd_watch(ctx, a->fd, 0, JS_NULL);
    if (is_reject)
        sdef_reject(ctx, &d, v);
    else
        sdef_resolve(ctx, &d, v);
    sdef_free(JS_GetRuntime(ctx), &d);
}

static JSValue fdsrc_ready(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fdata)
{
    SAdp* a = adp_of(fdata[0]);
    JSValue pin, args[2], n;
    double v;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (!a->pend)
        return JS_UNDEFINED;
    pin = JS_DupValue(ctx, fdata[0]);
    args[0] = JS_NewInt32(ctx, a->fd);
    args[1] = JS_DupValue(ctx, a->v[1]);
    n = nat_call(ctx, WSX_FD_READ, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(n)) {
        JSValue e = JS_GetException(ctx);
        fdsrc_settle(ctx, a, 1, e);
        JS_FreeValue(ctx, e);
    } else {
        if (JS_ToFloat64(ctx, &v, n) == 0 && v >= 0)
            fdsrc_settle(ctx, a, 0, n);
        JS_FreeValue(ctx, n);
    }
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue fdsrc_read(JSContext* ctx, JSValueConst adp, SAdp* a, JSValueConst buf)
{
    WVInfo vi;
    JSValue args[2], n, fn;
    int r = wst_view_info(ctx, buf, &vi);
    if (r < 0)
        return wst_reject_exc(ctx);
    if (r > 0)
        JS_FreeValue(ctx, vi.buffer);
    if (r == 0 || vi.type != JS_TYPED_ARRAY_UINT8) {
        JSValue e = wst_err(ctx, 0, "%s.read: buf must be a Uint8Array", a->name), p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        return p;
    }
    if (a->closed || vi.length == 0)
        return resolved_num(ctx, 0);
    if (a->pend) {
        JSValue e = wst_err(ctx, 0, "%s.read: a read is already pending", a->name), p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        return p;
    }
    if (a->sync) {
        args[0] = JS_NewInt32(ctx, a->fd);
        args[1] = buf;
        n = nat_call(ctx, WSX_FD_READ, 2, (JSValueConst*)args);
        if (JS_IsException(n))
            return wst_reject_exc(ctx);
        args[0] = resolved(ctx, n);
        JS_FreeValue(ctx, n);
        return args[0];
    }
    if (sdef_new(ctx, &a->d) < 0)
        return wst_reject_exc(ctx);
    a->pend = 1;
    adp_set(ctx, a, 1, JS_DupValue(ctx, buf));
    fn = JS_NewCFunctionData(ctx, fdsrc_ready, 0, 0, 1, &adp);
    fd_watch(ctx, a->fd, 0, fn);
    JS_FreeValue(ctx, fn);
    return JS_DupValue(ctx, a->d.p);
}

static void fdsrc_close(JSContext* ctx, SAdp* a)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return;
    if (a->closed)
        return;
    a->closed = 1;
    if (a->pend) {
        JSValue z = JS_NewInt32(ctx, 0);
        fdsrc_settle(ctx, a, 0, z);
    }
    if (JS_IsObject(w->x[WSX_STDIN]) && JS_VALUE_GET_PTR(w->x[WSX_STDIN]) == JS_VALUE_GET_PTR(a->obj)) {
        JS_FreeValue(ctx, w->x[WSX_STDIN]);
        w->x[WSX_STDIN] = JS_UNDEFINED;
    }
}

static JSValue fdsink_step(JSContext* ctx, JSValueConst data);

static JSValue fdsink_wready(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fdata)
{
    SAdp* a = adp_of(fdata[0]);
    JSValue pin = JS_DupValue(ctx, fdata[0]);
    SDef d;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    fd_watch(ctx, a->fd, 1, JS_NULL);
    d = a->d;
    sdef_init(&a->d);
    sdef_resolve(ctx, &d, JS_UNDEFINED);
    sdef_free(JS_GetRuntime(ctx), &d);
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue fdsink_step_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    return fdsink_step(ctx, data);
}

static JSValue fdsink_step(JSContext* ctx, JSValueConst data)
{
    SAdp* a = adp_of(data);
    double len = u8_len(ctx, a->v[3]), n;
    JSValue args[2], r, fn, p;
    while (a->n1 < len) {
        args[0] = JS_NewInt32(ctx, a->fd);
        args[1] = a->n1 == 0 ? JS_DupValue(ctx, a->v[3]) : u8_sub(ctx, a->v[3], a->n1, len - a->n1);
        if (JS_IsException(args[1]))
            return args[1];
        r = nat_call(ctx, WSX_FD_WRITE, 2, (JSValueConst*)args);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(r))
            return r;
        if (JS_ToFloat64(ctx, &n, r) < 0) {
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
        if (n < 0) {
            if (sdef_new(ctx, &a->d) < 0)
                return JS_EXCEPTION;
            fn = JS_NewCFunctionData(ctx, fdsink_wready, 0, 0, 1, &data);
            fd_watch(ctx, a->fd, 1, fn);
            JS_FreeValue(ctx, fn);
            p = JS_DupValue(ctx, a->d.p);
            r = wst_then(ctx, p, fdsink_step_rx, data, 0);
            JS_FreeValue(ctx, p);
            return r;
        }
        a->n1 += n;
    }
    return JS_NewFloat64(ctx, len);
}

static JSValue fdsink_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* k = adp_of(data);
    JSValueConst adp = k->v[0];
    SAdp* a = adp_of(adp);
    JSValue f;
    if (magic == 1) {
        if (is_reject && !a->flag) {
            a->flag = 1;
            adp_set(ctx, a, 2, JS_DupValue(ctx, arg));
        }
        return JS_UNDEFINED;
    }
    if (is_reject)
        return rethrow(ctx, arg);
    if (a->flag)
        return rethrow(ctx, a->v[2]);
    if (magic == 2) {
        int e = dyn_stdio_flush(a->fd);
        if (!e)
            return JS_UNDEFINED;
        JS_ThrowTypeError(ctx, "%s: write failed: %s", a->name, strerror(e));
        f = JS_GetException(ctx);
        a->flag = 1;
        adp_set(ctx, a, 2, JS_DupValue(ctx, f));
        return JS_Throw(ctx, f);
    }
    adp_set(ctx, a, 3, JS_DupValue(ctx, k->v[1]));
    a->n1 = 0;
    f = JS_NewInt32(ctx, a->fd);
    call_drop(ctx, nat_call(ctx, WSX_FD_FLUSH, 1, (JSValueConst*)&f));
    return promise_of(ctx, fdsink_step(ctx, adp));
}

static JSValue fdsink_write(JSContext* ctx, JSValueConst adp, SAdp* a, JSValueConst buf)
{
    char what[48];
    JSValue bytes, copy, k, p, t;
    if (a->closed) {
        JSValue e = wst_err(ctx, 0, "%s: the sink is closed", a->name);
        p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        return p;
    }
    snprintf(what, sizeof(what), "%s.write buf", a->name);
    if (wst_bytes_of_chunk(ctx, buf, what, &bytes) < 0)
        return wst_reject_exc(ctx);
    copy = u8_copy(ctx, bytes, u8_len(ctx, bytes));
    JS_FreeValue(ctx, bytes);
    if (JS_IsException(copy))
        return wst_reject_exc(ctx);
    k = adp_call(ctx, adp, copy);
    JS_FreeValue(ctx, copy);
    if (JS_IsException(k))
        return wst_reject_exc(ctx);
    p = wst_then(ctx, a->v[0], fdsink_rx, k, 0);
    t = JS_IsException(p) ? JS_EXCEPTION : wst_then(ctx, p, fdsink_rx, k, 1);
    JS_FreeValue(ctx, k);
    if (!JS_IsException(t))
        adp_set(ctx, a, 0, t);
    return p;
}

static JSValue fdsink_flush(JSContext* ctx, JSValueConst adp, SAdp* a)
{
    JSValue k = adp_call(ctx, adp, JS_UNDEFINED), p;
    if (JS_IsException(k))
        return wst_reject_exc(ctx);
    p = wst_then(ctx, a->v[0], fdsink_rx, k, 2);
    JS_FreeValue(ctx, k);
    return p;
}

static JSValue fd_object(JSContext* ctx, int fd)
{
    static const char* const names[3] = { "stdin", "stdout", "stderr" };
    JSValue adp, f, s;
    SAdp* a;
    if (!wst_rt(ctx))
        return JS_EXCEPTION;
    adp = adp_new_proto(ctx, fd == 0 ? AK_FDSRC : AK_FDSINK, wst_rt(ctx)->x[WSX_OBJ_PROTO], &a);
    if (JS_IsException(adp))
        return adp;
    a->fd = fd;
    a->name = names[fd];
    if (fd == 0) {
        f = JS_NewInt32(ctx, 0);
        s = nat_call(ctx, WSX_FD_SYNC, 1, (JSValueConst*)&f);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, adp);
            return s;
        }
        a->sync = JS_ToBool(ctx, s);
        JS_FreeValue(ctx, s);
    } else {
        a->v[0] = wst_resolved_undef(ctx);
    }
    if (io_define(ctx, adp, adp, fd != 0) < 0) {
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    a->v[4] = JS_GetPropertyStr(ctx, adp, fd ? "write" : "read");
    return adp;
}

static JSValue stdio_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    int which = 0;
    (void)this_val;
    if (argc > 0 && JS_ToInt32(ctx, &which, argv[0]) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToBool(ctx, argv[1])) {
        JSValue src, web;
        SRS* s = wst_opaque(w->x[WSX_STDIN_RS], WSTC_RS);
        if (which == 0 && s && s->state == RS_READABLE)
            return JS_DupValue(ctx, w->x[WSX_STDIN_RS]);
        src = stdio_fn(ctx, this_val, 1, argv);
        if (JS_IsException(src))
            return src;
        web = which == 0 ? to_readable(ctx, JS_UNDEFINED, 1, (JSValueConst*)&src) : to_writable(ctx, JS_UNDEFINED, 1, (JSValueConst*)&src);
        JS_FreeValue(ctx, src);
        if (which == 0 && !JS_IsException(web)) {
            JS_FreeValue(ctx, w->x[WSX_STDIN_RS]);
            w->x[WSX_STDIN_RS] = JS_DupValue(ctx, web);
        }
        return web;
    }
    if (which == 0) {
        if (JS_IsUndefined(w->x[WSX_STDIN])) {
            JSValue o = fd_object(ctx, 0);
            if (JS_IsException(o))
                return o;
            w->x[WSX_STDIN] = o;
        }
        return JS_DupValue(ctx, w->x[WSX_STDIN]);
    }
    return fd_object(ctx, which == 1 ? 1 : 2);
}

static JSValue msg_obj(JSContext* ctx, const char* type, JSValue value, int has_value)
{
    JSValue o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "type", JS_NewString(ctx, type), JS_PROP_C_W_E);
    if (has_value)
        JS_DefinePropertyValueStr(ctx, o, "value", value, JS_PROP_C_W_E);
    return o;
}

static const char* const err_names[7] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError" };

static int err_index(const char* n)
{
    int i;
    for (i = 0; i < 7; i++)
        if (n && !strcmp(n, err_names[i]))
            return i;
    return -1;
}

static JSValue pack_value(JSContext* ctx, JSValueConst v)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    JSValue name = JS_NewString(ctx, "Error"), msg = JS_NewString(ctx, ""), t, o;
    const char* ns;
    int isErr = JS_IsObject(v) ? JS_IsInstanceOf(ctx, v, w->x[WSX_ERR0]) : 0;
    if (isErr < 0) {
        if (JS_HasUncatchableException(ctx))
            goto fail;
        wst_drop_exc(ctx);
        isErr = 0;
    }
    if (!isErr) {
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, msg);
        return JS_DupValue(ctx, v);
    }
    t = JS_GetPropertyStr(ctx, v, "name");
    t = JS_IsException(t) ? t : to_string_free(ctx, t);
    if (JS_IsException(t)) {
        if (JS_HasUncatchableException(ctx))
            goto fail;
        wst_drop_exc(ctx);
    } else {
        JS_FreeValue(ctx, name);
        name = t;
        t = JS_GetPropertyStr(ctx, v, "message");
        t = JS_IsException(t) ? t : to_string_free(ctx, t);
        if (JS_IsException(t)) {
            if (JS_HasUncatchableException(ctx))
                goto fail;
            wst_drop_exc(ctx);
        } else {
            JS_FreeValue(ctx, msg);
            msg = t;
        }
    }
    ns = JS_ToCString(ctx, name);
    if (!ns)
        goto fail;
    o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "@@dyna.streams.error", JS_NewString(ctx, err_index(ns) < 0 ? "Error" : ns), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "message", msg, JS_PROP_C_W_E);
    JS_FreeCString(ctx, ns);
    JS_FreeValue(ctx, name);
    return o;
fail:
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    return JS_EXCEPTION;
}

static JSValue unpack_value(JSContext* ctx, JSValueConst v)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    JSValue t, m, r;
    const char* ns;
    int i;
    if (!JS_IsObject(v))
        return JS_DupValue(ctx, v);
    t = JS_GetPropertyStr(ctx, v, "@@dyna.streams.error");
    if (JS_IsException(t))
        return t;
    if (!JS_IsString(t)) {
        JS_FreeValue(ctx, t);
        return JS_DupValue(ctx, v);
    }
    ns = JS_ToCString(ctx, t);
    JS_FreeValue(ctx, t);
    if (!ns)
        return JS_EXCEPTION;
    i = err_index(ns);
    JS_FreeCString(ctx, ns);
    if (i < 0)
        i = 0;
    m = JS_GetPropertyStr(ctx, v, "message");
    if (JS_IsException(m))
        return m;
    r = JS_CallConstructor(ctx, w->x[WSX_ERR0 + i], 1, (JSValueConst*)&m);
    JS_FreeValue(ctx, m);
    return r;
}

static int port_post(JSContext* ctx, JSValueConst port, JSValue msg)
{
    JSValue r = call_method(ctx, port, "post", 1, (JSValueConst*)&msg);
    JS_FreeValue(ctx, msg);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int post_error(JSContext* ctx, JSValueConst port, JSValueConst e)
{
    JSValue te, pv = pack_value(ctx, e);
    if (!JS_IsException(pv) && port_post(ctx, port, msg_obj(ctx, "error", pv, 1)) == 0)
        return 0;
    if (JS_HasUncatchableException(ctx))
        return -1;
    wst_drop_exc(ctx);
    te = type_err(ctx, "the error could not be cloned");
    pv = pack_value(ctx, te);
    if (JS_IsException(pv) || port_post(ctx, port, msg_obj(ctx, "error", pv, 1)) < 0)
        wst_drop_exc(ctx);
    JS_FreeValue(ctx, te);
    return JS_HasUncatchableException(ctx) ? -1 : 0;
}

static void x_finish(JSContext* ctx, SAdp* a)
{
    JSValue held = JS_UNINITIALIZED;
    if (a->done)
        return;
    a->done = 1;
    a->listening = 0;
    if (JS_HasUncatchableException(ctx))
        held = JS_GetException(ctx);
    call_drop(ctx, call_method(ctx, a->v[0], "close", 0, NULL));
    if (!JS_IsUninitialized(held)) {
        JS_Throw(ctx, held);
        JS_SetUncatchableException(ctx, TRUE);
    }
}

static void x_listen(JSContext* ctx, SAdp* a, int on)
{
    JSValueConst fn = on ? a->v[2] : JS_NULL;
    call_drop(ctx, call_method(ctx, a->v[0], "listen", 1, &fn));
}

static int msg_type(JSContext* ctx, JSValueConst d, JSValue* value)
{
    JSValue t = JS_GetPropertyStr(ctx, d, "type");
    const char* s;
    int r = -1;
    if (JS_IsException(t))
        return -2;
    if (JS_IsString(t)) {
        s = JS_ToCString(ctx, t);
        if (s) {
            r = !strcmp(s, "chunk") ? 0 : !strcmp(s, "close") ? 1 : !strcmp(s, "error") ? 2 : !strcmp(s, "pull") ? 3 : -1;
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, t);
    *value = JS_UNDEFINED;
    if (r == 0 || r == 2) {
        *value = JS_GetPropertyStr(ctx, d, "value");
        if (JS_IsException(*value))
            return -2;
    }
    return r;
}

static void bp_release(JSContext* ctx, SAdp* a)
{
    SDef d;
    if (!a->flag)
        return;
    d = a->d;
    sdef_init(&a->d);
    a->flag = 0;
    sdef_resolve(ctx, &d, JS_UNDEFINED);
    sdef_free(JS_GetRuntime(ctx), &d);
}

static JSValue x_onmsg(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fdata)
{
    SAdp* a = adp_of(fdata[0]);
    JSValue pin, d, value = JS_UNDEFINED, r = JS_UNDEFINED, e;
    int t;
    (void)this_val;
    (void)magic;
    if (a->done || argc < 1)
        return JS_UNDEFINED;
    d = JS_GetPropertyStr(ctx, argv[0], "data");
    if (JS_IsException(d))
        return d;
    if (!JS_IsObject(d)) {
        JS_FreeValue(ctx, d);
        return JS_UNDEFINED;
    }
    pin = JS_DupValue(ctx, fdata[0]);
    t = msg_type(ctx, d, &value);
    if (t == -2)
        r = JS_EXCEPTION;
    else if (a->kind == AK_XRS) {
        SRSDC* c = a->rs->c;
        if (t == 0) {
            a->pend = 0;
            if (wst_rsdc_enqueue(ctx, c, value) < 0)
                r = JS_EXCEPTION;
        } else if (t == 1) {
            wst_rsdc_close(ctx, c);
            x_finish(ctx, a);
        } else if (t == 2) {
            e = unpack_value(ctx, value);
            if (JS_IsException(e))
                r = e;
            else {
                wst_rsdc_error(ctx, c, e);
                JS_FreeValue(ctx, e);
                x_finish(ctx, a);
            }
        }
        if (!JS_IsException(r) && !a->done && !a->pend && a->listening) {
            a->listening = 0;
            x_listen(ctx, a, 0);
        }
    } else {
        if (t == 3)
            bp_release(ctx, a);
        else if (t == 2) {
            e = unpack_value(ctx, value);
            if (JS_IsException(e))
                r = e;
            else {
                wst_wsdc_error_if_needed(ctx, a->ws->c, e);
                JS_FreeValue(ctx, e);
                bp_release(ctx, a);
                x_finish(ctx, a);
            }
        }
        if (!JS_IsException(r) && !a->done && !a->flag && a->listening) {
            a->listening = 0;
            x_listen(ctx, a, 0);
        }
    }
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, pin);
    return r;
}

static JSValue xrs_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin;
    (void)argc;
    (void)argv;
    if (a->done)
        return wst_resolved_undef(ctx);
    pin = JS_DupValue(ctx, data);
    a->pend = 1;
    if (!a->listening) {
        a->listening = 1;
        x_listen(ctx, a, 1);
    }
    if (port_post(ctx, a->v[0], msg_obj(ctx, "pull", JS_UNDEFINED, 0)) < 0) {
        JSValue e = JS_GetException(ctx), p;
        x_finish(ctx, a);
        p = wst_rejected(ctx, e);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, pin);
        return p;
    }
    JS_FreeValue(ctx, pin);
    return wst_resolved_undef(ctx);
}

static JSValue x_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin;
    int rc;
    if (a->done)
        return wst_resolved_undef(ctx);
    pin = JS_DupValue(ctx, data);
    rc = post_error(ctx, a->v[0], argc > 0 ? argv[0] : JS_UNDEFINED);
    x_finish(ctx, a);
    JS_FreeValue(ctx, pin);
    if (rc < 0)
        return JS_EXCEPTION;
    return wst_resolved_undef(ctx);
}

static JSValue xws_write_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SAdp* k = adp_of(data);
    SAdp* a = adp_of(k->v[0]);
    SWS* s = a->ws;
    (void)magic;
    if (is_reject)
        return rethrow(ctx, arg);
    if (a->done) {
        if (s->state == WS_ERRORING || s->state == WS_ERRORED)
            return rethrow(ctx, s->stored_error);
        return wst_throw(ctx, 0, "the other side of a transferred stream went away");
    }
    if (sdef_new(ctx, &a->d) < 0)
        return JS_EXCEPTION;
    a->flag = 1;
    if (port_post(ctx, a->v[0], msg_obj(ctx, "chunk", JS_DupValue(ctx, k->v[1]), 1)) < 0) {
        JSValue e = JS_GetException(ctx);
        int rc = post_error(ctx, a->v[0], e);
        x_finish(ctx, a);
        if (rc < 0) {
            JS_FreeValue(ctx, e);
            return JS_EXCEPTION;
        }
        return JS_Throw(ctx, e);
    }
    return JS_UNDEFINED;
}

static JSValue xws_write(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue wait, k, q;
    if (a->done)
        return wst_reject_err(ctx, 0, "the other side of a transferred stream went away");
    if (a->flag && !a->listening) {
        a->listening = 1;
        x_listen(ctx, a, 1);
    }
    wait = a->flag ? JS_DupValue(ctx, a->d.p) : wst_resolved_undef(ctx);
    k = adp_call(ctx, data, argc > 0 ? argv[0] : JS_UNDEFINED);
    q = JS_IsException(k) ? JS_EXCEPTION : wst_then(ctx, wait, xws_write_rx, k, 0);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, wait);
    return q;
}

static JSValue xws_close(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SAdp* a = adp_of(data);
    JSValue pin;
    (void)argc;
    (void)argv;
    if (!a->done) {
        pin = JS_DupValue(ctx, data);
        if (port_post(ctx, a->v[0], msg_obj(ctx, "close", JS_UNDEFINED, 0)) < 0)
            wst_drop_exc(ctx);
        x_finish(ctx, a);
        JS_FreeValue(ctx, pin);
    }
    return wst_resolved_undef(ctx);
}

static JSValue x_new(JSContext* ctx, int kind, JSValue port, SAdp** out)
{
    SAdp* a;
    JSValue adp = adp_new(ctx, kind, &a);
    if (JS_IsException(adp)) {
        JS_FreeValue(ctx, port);
        return adp;
    }
    a->v[0] = port;
    a->v[2] = JS_NewCFunctionData(ctx, x_onmsg, 1, 0, 1, (JSValueConst*)&adp);
    *out = a;
    return adp;
}

static JSValue cross_readable(JSContext* ctx, JSValue port, SRS** out)
{
    SAlgo pull, cancel;
    SAdp* a;
    JSValue adp = x_new(ctx, AK_XRS, port, &a), obj;
    if (JS_IsException(adp))
        return adp;
    sa_init(&pull);
    sa_init(&cancel);
    sa_set_c(ctx, &pull, xrs_pull, adp);
    sa_set_c(ctx, &cancel, x_cancel, adp);
    obj = wst_rs_create(ctx, JS_UNDEFINED, &pull, &cancel, 0, JS_UNDEFINED, 0, &a->rs);
    sa_clear(ctx, &pull);
    sa_clear(ctx, &cancel);
    if (!JS_IsException(obj)) {
        a->v[1] = JS_DupValue(ctx, obj);
        if (out)
            *out = a->rs;
    }
    JS_FreeValue(ctx, adp);
    return obj;
}

static JSValue cross_writable(JSContext* ctx, JSValue port, SWS** out)
{
    SAlgo wr, cl, ab;
    SAdp* a;
    JSValue adp = x_new(ctx, AK_XWS, port, &a), obj;
    if (JS_IsException(adp))
        return adp;
    if (sdef_new(ctx, &a->d) < 0) {
        JS_FreeValue(ctx, adp);
        return JS_EXCEPTION;
    }
    a->flag = 1;
    sa_init(&wr);
    sa_init(&cl);
    sa_init(&ab);
    sa_set_c(ctx, &wr, xws_write, adp);
    sa_set_c(ctx, &cl, xws_close, adp);
    sa_set_c(ctx, &ab, x_cancel, adp);
    obj = wst_ws_create(ctx, &wr, &cl, &ab, JS_UNDEFINED, 1, JS_UNDEFINED, 0, &a->ws);
    sa_clear(ctx, &wr);
    sa_clear(ctx, &cl);
    sa_clear(ctx, &ab);
    if (!JS_IsException(obj)) {
        a->v[1] = JS_DupValue(ctx, obj);
        if (out)
            *out = a->ws;
    }
    JS_FreeValue(ctx, adp);
    return obj;
}

enum { XR_NONE, XR_RS, XR_WS, XR_TS };

static int xfer_kind(JSValueConst x)
{
    if (wst_opaque(x, WSTC_RS))
        return XR_RS;
    if (wst_opaque(x, WSTC_WS))
        return XR_WS;
    if (wst_opaque(x, WSTC_TS))
        return XR_TS;
    return XR_NONE;
}

int js_streams_kind(JSValueConst x)
{
    return xfer_kind(x);
}

JSValue js_streams_reader_open(JSContext* ctx, JSValueConst rs)
{
    SRS* s = wst_rs_of(ctx, rs, "src");
    if (!s)
        return JS_EXCEPTION;
    return wst_rs_acquire_reader(ctx, s, 0, NULL);
}

JSValue js_streams_reader_read(JSContext* ctx, JSValueConst reader)
{
    SReader* r = wst_opaque(reader, WSTC_RSDR);
    if (!r || !r->s)
        return wst_throw(ctx, 0, "the reader was released");
    return wst_reader_read_promise(ctx, r);
}

void js_streams_reader_close(JSContext* ctx, JSValueConst reader, int cancel)
{
    SReader* r = wst_opaque(reader, WSTC_RSDR);
    if (!r || !r->s)
        return;
    if (cancel)
        mark_handled_free(ctx, wst_rs_cancel(ctx, r->s, JS_UNDEFINED));
    if (r->s)
        wst_reader_release(ctx, r);
}

typedef struct XWalk {
    JSValueConst items;
    uint32_t nitems;
    JSValue memo;
} XWalk;

static JSValue map_call(JSContext* ctx, int which, JSValueConst map, int argc, JSValueConst* argv)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    return JS_Call(ctx, w->x[which], map, argc, argv);
}

static JSValue xwalk(JSContext* ctx, XWalk* w, JSValueConst x);

static int xwalk_slot(JSContext* ctx, XWalk* w, JSValueConst x, JSValue* out, int arr, JSAtom key, uint32_t idx)
{
    JSValue v = arr ? JS_GetPropertyUint32(ctx, x, idx) : JS_GetProperty(ctx, x, key), nv;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    nv = xwalk(ctx, w, v);
    if (JS_IsException(nv)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (JS_VALUE_GET_PTR(nv) != JS_VALUE_GET_PTR(v)) {
        if (JS_VALUE_GET_PTR(*out) == JS_VALUE_GET_PTR(x)) {
            JSValue copy, args[2];
            if (arr)
                copy = call_method(ctx, x, "slice", 0, NULL);
            else {
                JSPropertyEnum* tab;
                uint32_t n, i;
                copy = JS_NewObject(ctx);
                if (!JS_IsException(copy) && JS_GetOwnPropertyNames(ctx, &tab, &n, x, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                    for (i = 0; i < n; i++) {
                        JSValue e = JS_GetProperty(ctx, x, tab[i].atom);
                        if (JS_IsException(e) || JS_SetProperty(ctx, copy, tab[i].atom, e) < 0) {
                            JS_FreeValue(ctx, copy);
                            copy = JS_EXCEPTION;
                            break;
                        }
                    }
                    JS_FreePropertyEnum(ctx, tab, n);
                } else if (!JS_IsException(copy)) {
                    JS_FreeValue(ctx, copy);
                    copy = JS_EXCEPTION;
                }
            }
            if (JS_IsException(copy)) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, nv);
                return -1;
            }
            args[0] = x;
            args[1] = copy;
            call_drop(ctx, map_call(ctx, WSX_MAP_SET, w->memo, 2, (JSValueConst*)args));
            JS_FreeValue(ctx, *out);
            *out = copy;
        }
        if ((arr ? JS_SetPropertyUint32(ctx, *out, idx, nv) : JS_SetProperty(ctx, *out, key, nv)) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    } else {
        JS_FreeValue(ctx, nv);
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue xwalk(JSContext* ctx, XWalk* w, JSValueConst x)
{
    JSValue m, out, args[2];
    uint32_t i;
    int isarr;
    if (!JS_IsObject(x))
        return JS_DupValue(ctx, x);
    for (i = 0; i < w->nitems; i++) {
        JSValue it = JS_GetPropertyUint32(ctx, w->items, i);
        int same = JS_VALUE_GET_PTR(it) == JS_VALUE_GET_PTR(x);
        JS_FreeValue(ctx, it);
        if (same) {
            JSValue o = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, o, "@@dyna.streams.ref", JS_NewInt32(ctx, (int)i), JS_PROP_C_W_E);
            return o;
        }
    }
    if (xfer_kind(x) != XR_NONE)
        return js_throw_dom_exception(ctx, "DataCloneError", "a stream in the message must also be in the transfer list");
    m = map_call(ctx, WSX_MAP_GET, w->memo, 1, &x);
    if (JS_IsException(m) || !JS_IsUndefined(m))
        return m;
    args[0] = x;
    args[1] = x;
    m = map_call(ctx, WSX_MAP_SET, w->memo, 2, (JSValueConst*)args);
    if (JS_IsException(m))
        return m;
    JS_FreeValue(ctx, m);
    {
        WVInfo vi;
        int r = wst_view_info(ctx, x, &vi);
        if (r > 0)
            JS_FreeValue(ctx, vi.buffer);
        if (r != 0 || wst_is_ab(x))
            return r < 0 ? JS_EXCEPTION : JS_DupValue(ctx, x);
    }
    out = JS_DupValue(ctx, x);
    isarr = JS_IsArray(ctx, x);
    if (isarr < 0)
        goto fail;
    if (isarr) {
        int64_t len;
        if (get_len(ctx, x, &len) < 0)
            goto fail;
        for (i = 0; i < (uint32_t)len; i++)
            if (xwalk_slot(ctx, w, x, &out, 1, 0, i) < 0)
                goto fail;
        return out;
    } else {
        JSPropertyEnum* tab;
        uint32_t n;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, x, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail;
        for (i = 0; i < n; i++)
            if (xwalk_slot(ctx, w, x, &out, 0, tab[i].atom, 0) < 0) {
                JS_FreePropertyEnum(ctx, tab, n);
                goto fail;
            }
        JS_FreePropertyEnum(ctx, tab, n);
        return out;
    }
fail:
    JS_FreeValue(ctx, out);
    return JS_EXCEPTION;
}

static JSValue port_pair(JSContext* ctx, JSValue* id)
{
    JSValue pr = nat_call(ctx, WSX_PORT_PAIR, 0, NULL), port;
    if (JS_IsException(pr))
        return pr;
    port = JS_GetPropertyUint32(ctx, pr, 0);
    *id = JS_GetPropertyUint32(ctx, pr, 1);
    JS_FreeValue(ctx, pr);
    return port;
}

static int ph_push(JSContext* ctx, JSValueConst phs, uint32_t i, const char* kind, JSValue a, JSValue b)
{
    JSValue o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "kind", JS_NewString(ctx, kind), JS_PROP_C_W_E);
    if (JS_IsUndefined(b))
        JS_DefinePropertyValueStr(ctx, o, "id", a, JS_PROP_C_W_E);
    else {
        JS_DefinePropertyValueStr(ctx, o, "r", a, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "w", b, JS_PROP_C_W_E);
    }
    return JS_DefinePropertyValueUint32(ctx, phs, i, o, JS_PROP_C_W_E);
}

static void pipe_handled(JSContext* ctx, SRS* s, SWS* d)
{
    if (s && d)
        mark_handled_free(ctx, wst_rs_pipe_to(ctx, s, d, 0, 0, 0, JS_UNDEFINED));
}

static JSValue xfer_fin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* fdata)
{
    SAdp* a = adp_of(fdata[0]);
    JSValue pin = JS_DupValue(ctx, fdata[0]);
    int64_t n = 0, i, k = 0;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (magic == 0) {
        get_len(ctx, a->v[1], &n);
        for (i = 0; i < n; i++) {
            JSValue p = JS_GetPropertyUint32(ctx, a->v[1], (uint32_t)i);
            call_drop(ctx, call_method(ctx, p, "close", 0, NULL));
            JS_FreeValue(ctx, p);
        }
        get_len(ctx, a->v[2], &n);
        for (i = 0; i < n; i++) {
            JSValue id = JS_GetPropertyUint32(ctx, a->v[2], (uint32_t)i), p;
            p = nat_call(ctx, WSX_PORT_CLAIM, 1, (JSValueConst*)&id);
            if (JS_IsException(p))
                wst_drop_exc(ctx);
            else
                call_drop(ctx, call_method(ctx, p, "close", 0, NULL));
            JS_FreeValue(ctx, p);
            JS_FreeValue(ctx, id);
        }
    } else {
        get_len(ctx, a->v[0], &n);
        for (i = 0; i < n; i++) {
            JSValue it = JS_GetPropertyUint32(ctx, a->v[0], (uint32_t)i);
            JSValue p = JS_GetPropertyUint32(ctx, a->v[1], (uint32_t)k++), x;
            SRS* xs = NULL;
            SWS* xw = NULL;
            int kind = xfer_kind(it);
            if (kind == XR_RS) {
                x = cross_writable(ctx, p, &xw);
                pipe_handled(ctx, wst_opaque(it, WSTC_RS), JS_IsException(x) ? NULL : xw);
                JS_FreeValue(ctx, x);
            } else if (kind == XR_WS) {
                x = cross_readable(ctx, p, &xs);
                pipe_handled(ctx, JS_IsException(x) ? NULL : xs, wst_opaque(it, WSTC_WS));
                JS_FreeValue(ctx, x);
            } else {
                STS* t = wst_opaque(it, WSTC_TS);
                JSValue p2 = JS_GetPropertyUint32(ctx, a->v[1], (uint32_t)k++);
                x = cross_writable(ctx, p, &xw);
                pipe_handled(ctx, t->rs, JS_IsException(x) ? NULL : xw);
                JS_FreeValue(ctx, x);
                x = cross_readable(ctx, p2, &xs);
                pipe_handled(ctx, JS_IsException(x) ? NULL : xs, t->ws);
                JS_FreeValue(ctx, x);
            }
            if (JS_HasException(ctx))
                wst_drop_exc(ctx);
            JS_FreeValue(ctx, it);
        }
    }
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue transfer_prepare(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    WstRt* wr = wst_rt(ctx);
    if (!wr)
        return JS_EXCEPTION;
    JSValueConst msg = argc > 0 ? argv[0] : JS_UNDEFINED, tr = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue items = JS_UNDEFINED, rest = JS_UNDEFINED, adp = JS_UNDEFINED, data = JS_UNDEFINED, env, out, phs, ids, ports;
    uint32_t ni = 0, nr = 0, np = 0, nid = 0, j;
    int64_t len, i;
    XWalk w;
    SAdp* a;
    int isarr = JS_IsArray(ctx, tr);
    (void)this_val;
    if (isarr <= 0)
        return isarr < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    if (get_len(ctx, tr, &len) < 0)
        return JS_EXCEPTION;
    items = JS_NewArray(ctx);
    rest = JS_NewArray(ctx);
    for (i = 0; i < len; i++) {
        JSValue x = JS_GetPropertyUint32(ctx, tr, (uint32_t)i);
        if (JS_IsException(x))
            goto fail;
        if (xfer_kind(x) == XR_NONE) {
            JS_DefinePropertyValueUint32(ctx, rest, nr++, x, JS_PROP_C_W_E);
            continue;
        }
        for (j = 0; j < ni; j++) {
            JSValue it = JS_GetPropertyUint32(ctx, items, j);
            int same = JS_VALUE_GET_PTR(it) == JS_VALUE_GET_PTR(x);
            JS_FreeValue(ctx, it);
            if (same) {
                JS_FreeValue(ctx, x);
                js_throw_dom_exception(ctx, "DataCloneError", "transfer[%d] lists the same stream twice", (int)i);
                goto fail;
            }
        }
        JS_DefinePropertyValueUint32(ctx, items, ni++, x, JS_PROP_C_W_E);
    }
    if (ni == 0) {
        JS_FreeValue(ctx, items);
        JS_FreeValue(ctx, rest);
        return JS_UNDEFINED;
    }
    for (j = 0; j < ni; j++) {
        JSValue it = JS_GetPropertyUint32(ctx, items, j);
        int kind = xfer_kind(it), bad = 0;
        const char* m = NULL;
        if (kind == XR_RS && wst_rs_locked(wst_opaque(it, WSTC_RS)))
            m = "a locked ReadableStream cannot be transferred";
        else if (kind == XR_WS && wst_ws_locked(wst_opaque(it, WSTC_WS)))
            m = "a locked WritableStream cannot be transferred";
        else if (kind == XR_TS) {
            STS* t = wst_opaque(it, WSTC_TS);
            uint32_t q;
            bad = (t->rs && wst_rs_locked(t->rs)) || (t->ws && wst_ws_locked(t->ws));
            for (q = 0; q < ni && !bad; q++) {
                JSValue o = JS_GetPropertyUint32(ctx, items, q);
                bad = JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(t->readable) || JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(t->writable);
                JS_FreeValue(ctx, o);
            }
            if (bad)
                m = "a TransformStream with a locked side cannot be transferred";
        }
        JS_FreeValue(ctx, it);
        if (m) {
            js_throw_dom_exception(ctx, "DataCloneError", "%s", m);
            goto fail;
        }
    }
    w.items = items;
    w.nitems = ni;
    w.memo = JS_CallConstructor(ctx, wr->x[WSX_MAP_CTOR], 0, NULL);
    if (JS_IsException(w.memo))
        goto fail;
    data = xwalk(ctx, &w, msg);
    JS_FreeValue(ctx, w.memo);
    if (JS_IsException(data))
        goto fail;
    adp = adp_new(ctx, AK_XFER, &a);
    if (JS_IsException(adp))
        goto fail;
    a->v[0] = JS_DupValue(ctx, items);
    ports = a->v[1] = JS_NewArray(ctx);
    ids = a->v[2] = JS_NewArray(ctx);
    phs = JS_NewArray(ctx);
    for (j = 0; j < ni; j++) {
        JSValue it = JS_GetPropertyUint32(ctx, items, j), id, id2, p;
        int kind = xfer_kind(it);
        JS_FreeValue(ctx, it);
        p = port_pair(ctx, &id);
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, phs);
            goto fail;
        }
        JS_DefinePropertyValueUint32(ctx, ports, np++, p, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, ids, nid++, JS_DupValue(ctx, id), JS_PROP_C_W_E);
        if (kind != XR_TS) {
            ph_push(ctx, phs, j, kind == XR_RS ? "RS" : "WS", id, JS_UNDEFINED);
            continue;
        }
        p = port_pair(ctx, &id2);
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, id);
            JS_FreeValue(ctx, phs);
            goto fail;
        }
        JS_DefinePropertyValueUint32(ctx, ports, np++, p, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, ids, nid++, JS_DupValue(ctx, id2), JS_PROP_C_W_E);
        ph_push(ctx, phs, j, "TS", id, id2);
    }
    env = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, env, "@@dyna.streams.envelope", JS_NewInt32(ctx, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, env, "data", data, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, env, "ports", phs, JS_PROP_C_W_E);
    out = JS_NewArray(ctx);
    JS_DefinePropertyValueUint32(ctx, out, 0, env, JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 1, rest, JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 2, JS_NewCFunctionData(ctx, xfer_fin, 0, 0, 1, (JSValueConst*)&adp), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 3, JS_NewCFunctionData(ctx, xfer_fin, 0, 1, 1, (JSValueConst*)&adp), JS_PROP_C_W_E);
    JS_FreeValue(ctx, adp);
    JS_FreeValue(ctx, items);
    return out;
fail:
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, adp);
    JS_FreeValue(ctx, items);
    JS_FreeValue(ctx, rest);
    return JS_EXCEPTION;
}

static JSValue xfix(JSContext* ctx, JSValueConst made, int64_t nmade, JSValue seen, JSValueConst x)
{
    WstRt* wr = wst_rt(ctx);
    if (!wr)
        return JS_EXCEPTION;
    JSValue i, has, args[2], pr;
    int isarr;
    if (!JS_IsObject(x))
        return JS_DupValue(ctx, x);
    i = JS_GetPropertyStr(ctx, x, "@@dyna.streams.ref");
    if (JS_IsException(i))
        return i;
    if (JS_IsNumber(i)) {
        double d;
        JS_ToFloat64(ctx, &d, i);
        if (d >= 0 && d < (double)nmade) {
            JSPropertyEnum* tab;
            uint32_t n;
            if (JS_GetOwnPropertyNames(ctx, &tab, &n, x, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, i);
                return JS_EXCEPTION;
            }
            JS_FreePropertyEnum(ctx, tab, n);
            if (n == 1)
                {
                JSAtom at = JS_ValueToAtom(ctx, i);
                JSValue r = at == JS_ATOM_NULL ? JS_EXCEPTION : JS_GetProperty(ctx, made, at);
                JS_FreeAtom(ctx, at);
                JS_FreeValue(ctx, i);
                return r;
            }
        }
    }
    JS_FreeValue(ctx, i);
    has = map_call(ctx, WSX_MAP_GET, seen, 1, &x);
    if (JS_IsException(has))
        return has;
    if (!JS_IsUndefined(has)) {
        JS_FreeValue(ctx, has);
        return JS_DupValue(ctx, x);
    }
    args[0] = x;
    args[1] = JS_TRUE;
    call_drop(ctx, map_call(ctx, WSX_MAP_SET, seen, 2, (JSValueConst*)args));
    isarr = JS_IsArray(ctx, x);
    if (isarr < 0)
        return JS_EXCEPTION;
    if (isarr) {
        int64_t len, k;
        if (get_len(ctx, x, &len) < 0)
            return JS_EXCEPTION;
        for (k = 0; k < len; k++) {
            JSValue v = JS_GetPropertyUint32(ctx, x, (uint32_t)k), nv;
            if (JS_IsException(v))
                return v;
            nv = xfix(ctx, made, nmade, seen, v);
            JS_FreeValue(ctx, v);
            if (JS_IsException(nv) || JS_SetPropertyInt64(ctx, x, k, nv) < 0)
                return JS_EXCEPTION;
        }
        return JS_DupValue(ctx, x);
    }
    pr = JS_GetPrototype(ctx, x);
    if (JS_IsException(pr))
        return pr;
    if (JS_IsNull(pr) || JS_VALUE_GET_PTR(pr) == JS_VALUE_GET_PTR(wr->x[WSX_OBJ_PROTO])) {
        JSPropertyEnum* tab;
        uint32_t n, k;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, x, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, pr);
            return JS_EXCEPTION;
        }
        for (k = 0; k < n; k++) {
            JSValue v = JS_GetProperty(ctx, x, tab[k].atom), nv;
            if (JS_IsException(v))
                break;
            nv = xfix(ctx, made, nmade, seen, v);
            JS_FreeValue(ctx, v);
            if (JS_IsException(nv) || JS_SetProperty(ctx, x, tab[k].atom, nv) < 0)
                break;
        }
        JS_FreePropertyEnum(ctx, tab, n);
        if (k < n) {
            JS_FreeValue(ctx, pr);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, pr);
    return JS_DupValue(ctx, x);
}

static JSValue claim(JSContext* ctx, JSValueConst ph, const char* k)
{
    JSValue id = JS_GetPropertyStr(ctx, ph, k), p;
    if (JS_IsException(id))
        return id;
    p = nat_call(ctx, WSX_PORT_CLAIM, 1, (JSValueConst*)&id);
    JS_FreeValue(ctx, id);
    return p;
}

static JSValue transfer_revive(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    WstRt* wr = wst_rt(ctx);
    if (!wr)
        return JS_EXCEPTION;
    JSValueConst env = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue ports, made = JS_UNDEFINED, seen, data, r;
    int64_t n, i;
    int isarr;
    (void)this_val;
    ports = JS_GetPropertyStr(ctx, env, "ports");
    if (JS_IsException(ports))
        return ports;
    isarr = JS_IsArray(ctx, ports);
    if (isarr <= 0) {
        JS_FreeValue(ctx, ports);
        return isarr < 0 ? JS_EXCEPTION : wst_throw(ctx, 0, "malformed transferred-stream message");
    }
    if (get_len(ctx, ports, &n) < 0)
        goto fail;
    made = JS_NewArray(ctx);
    for (i = 0; i < n; i++) {
        JSValue ph = JS_GetPropertyUint32(ctx, ports, (uint32_t)i), k, x = JS_EXCEPTION;
        const char* ks;
        if (JS_IsException(ph))
            goto fail;
        k = JS_GetPropertyStr(ctx, ph, "kind");
        ks = JS_IsException(k) ? NULL : JS_ToCString(ctx, k);
        JS_FreeValue(ctx, k);
        if (ks && !strcmp(ks, "RS")) {
            JSValue p = claim(ctx, ph, "id");
            x = JS_IsException(p) ? p : cross_readable(ctx, p, NULL);
        } else if (ks && !strcmp(ks, "WS")) {
            JSValue p = claim(ctx, ph, "id");
            x = JS_IsException(p) ? p : cross_writable(ctx, p, NULL);
        } else if (ks && !strcmp(ks, "TS")) {
            JSValue pr = claim(ctx, ph, "r"), pw, xr, xw;
            SRS* s = NULL;
            SWS* w = NULL;
            xr = JS_IsException(pr) ? pr : cross_readable(ctx, pr, &s);
            pw = JS_IsException(xr) ? JS_EXCEPTION : claim(ctx, ph, "w");
            xw = JS_IsException(pw) ? pw : cross_writable(ctx, pw, &w);
            if (JS_IsException(xw)) {
                JS_FreeValue(ctx, xr);
                x = JS_EXCEPTION;
            } else
                x = wst_ts_shell(ctx, xr, s, xw, w);
        } else {
            if (ks || !JS_HasException(ctx))
                wst_throw(ctx, 0, "malformed transferred-stream message");
        }
        JS_FreeCString(ctx, ks);
        JS_FreeValue(ctx, ph);
        if (JS_IsException(x))
            goto fail;
        JS_DefinePropertyValueUint32(ctx, made, (uint32_t)i, x, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, ports);
    seen = JS_CallConstructor(ctx, wr->x[WSX_MAP_CTOR], 0, NULL);
    if (JS_IsException(seen)) {
        JS_FreeValue(ctx, made);
        return seen;
    }
    data = JS_GetPropertyStr(ctx, env, "data");
    r = JS_IsException(data) ? data : xfix(ctx, made, n, seen, data);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, seen);
    JS_FreeValue(ctx, made);
    return r;
fail:
    JS_FreeValue(ctx, ports);
    JS_FreeValue(ctx, made);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry adapt_funcs[] = {
    JS_CFUNC_DEF("toReadableStream", 2, to_readable),
    JS_CFUNC_DEF("toWritableStream", 2, to_writable),
    JS_CFUNC_DEF("fromReadableStream", 1, from_readable),
    JS_CFUNC_DEF("socketStreams", 3, socket_streams),
    JS_CFUNC_DEF("stdio", 1, stdio_fn),
    JS_CFUNC_DEF("transferPrepare", 2, transfer_prepare),
    JS_CFUNC_DEF("transferRevive", 1, transfer_revive),
    JS_CFUNC_DEF("bodyStream", 2, body_stream),
    JS_CFUNC_DEF("bodyReadAll", 1, body_read_all),
    JS_CFUNC_DEF("isDisturbed", 1, is_disturbed),
    JS_CFUNC_DEF("isLocked", 1, is_locked),
    JS_CFUNC_DEF("tee", 1, tee_fn),
    JS_CFUNC_DEF("rsOf", 1, rs_of_fn),
};

int wst_init_adapt(JSContext* ctx, JSValueConst internal)
{
    static const WstClassSpec spec = { WSTC_ADP, "WstAdapter", NULL, 0, NULL, 0, NULL, 0, adp_finalizer, adp_mark };
    static const char* const nat[] = { "fastPipe", "nativePipe", "portPair", "portClaim", "fdSync", "fdRead", "fdWrite", "fdWatch", "fdFlushStdio" };
    WstRt* w = wst_rt(ctx);
    if (!w)
        return -1;
    JSValue tmp, g, sym, map, mp, op;
    int i, r;
    if (wst_register(ctx, &spec, &tmp) < 0)
        return -1;
    if (wst_slot(ctx, &w->x[WSX_NATIVES], js_streams_natives(ctx)) < 0)
        return -1;
    for (i = 0; i < (int)countof(nat); i++)
        if (wst_slot(ctx, &w->x[WSX_FAST_PIPE + i], JS_GetPropertyStr(ctx, w->x[WSX_NATIVES], nat[i])) < 0)
            return -1;
    g = JS_GetGlobalObject(ctx);
    sym = JS_GetPropertyStr(ctx, g, "Symbol");
    r = JS_IsException(sym) ? -1 : wst_slot(ctx, &w->x[WSX_DISPOSE], JS_GetPropertyStr(ctx, sym, "dispose"));
    JS_FreeValue(ctx, sym);
    op = r < 0 ? JS_EXCEPTION : JS_GetPropertyStr(ctx, g, "Object");
    r = JS_IsException(op) ? -1 : wst_slot(ctx, &w->x[WSX_OBJ_PROTO], JS_GetPropertyStr(ctx, op, "prototype"));
    JS_FreeValue(ctx, op);
    map = r < 0 ? JS_EXCEPTION : JS_GetPropertyStr(ctx, g, "Map");
    mp = JS_IsException(map) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, map, "prototype");
    r = JS_IsException(mp) ? -1 : 0;
    if (wst_slot(ctx, &w->x[WSX_MAP_CTOR], map) < 0)
        r = -1;
    if (r == 0 && (wst_slot(ctx, &w->x[WSX_MAP_GET], JS_GetPropertyStr(ctx, mp, "get")) < 0 || wst_slot(ctx, &w->x[WSX_MAP_SET], JS_GetPropertyStr(ctx, mp, "set")) < 0))
        r = -1;
    JS_FreeValue(ctx, mp);
    for (i = 0; r == 0 && i < 7; i++)
        r = wst_slot(ctx, &w->x[WSX_ERR0 + i], JS_GetPropertyStr(ctx, g, err_names[i]));
    JS_FreeValue(ctx, g);
    if (r < 0)
        return -1;
    for (i = 0; JS_IsObject(internal) && i < (int)countof(adapt_funcs); i++) {
        const JSCFunctionListEntry* e = &adapt_funcs[i];
        JSValue f = JS_NewCFunction(ctx, e->u.func.cfunc.generic, e->name, e->u.func.length);
        if (JS_IsException(f) || JS_DefinePropertyValueStr(ctx, internal, e->name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
    return 0;
}
