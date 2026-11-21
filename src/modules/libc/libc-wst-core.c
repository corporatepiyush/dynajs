#include "libc-wst.h"
#include <math.h>
#include <stdarg.h>

JSClassID wst_cid[WSTC_N];

static const char* const wst_atom_names[WSA_N] = {
    "value",
    "done",
    "highWaterMark",
    "size",
    "start",
    "pull",
    "cancel",
    "type",
    "autoAllocateChunkSize",
    "write",
    "close",
    "abort",
    "mode",
    "prototype",
    "readable",
    "writable",
    "preventClose",
    "preventAbort",
    "preventCancel",
    "signal",
    "min",
    "transform",
    "flush",
    "readableType",
    "writableType",
};

WstRt* wst_rt(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    WstRt* w;
    int i;

    if (!ts) {
        if (!JS_HasException(ctx))
            JS_ThrowInternalError(ctx, "web streams: the runtime is shutting down");
        return NULL;
    }
    if (ts->wst)
        return ts->wst;
    w = js_mallocz(ctx, sizeof(*w));
    if (!w)
        return NULL;
    for (i = 0; i < WSA_N; i++) {
        w->atoms[i] = JS_NewAtom(ctx, wst_atom_names[i]);
        if (w->atoms[i] == JS_ATOM_NULL) {
            while (i-- > 0)
                JS_FreeAtom(ctx, w->atoms[i]);
            js_free(ctx, w);
            return NULL;
        }
    }
    w->rs_iter_proto = JS_UNDEFINED;
    w->abort_ctor = JS_UNDEFINED;
    w->ab_ctor = JS_UNDEFINED;
    w->ab_transfer = JS_UNDEFINED;
    w->sig_ctor = JS_UNDEFINED;
    w->sig_add = JS_UNDEFINED;
    w->sig_remove = JS_UNDEFINED;
    w->blen_size = w->count_size = w->dec_ctor = w->dec_decode = w->type_error_ctor = JS_UNDEFINED;
    w->pc_ctor = w->pc_close = JS_UNDEFINED;
    for (i = 0; i < WSX_N; i++)
        w->x[i] = JS_UNDEFINED;
    for (i = 0; i < WSTC_N; i++)
        w->ctors[i] = JS_UNDEFINED;
    ts->wst = w;
    return w;
}

int wst_slot(JSContext* ctx, JSValue* slot, JSValue v)
{
    JS_FreeValue(ctx, *slot);
    *slot = JS_IsException(v) ? JS_UNDEFINED : v;
    return JS_IsException(v) ? -1 : 0;
}

JSAtom wst_atom(JSContext* ctx, int a)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return ((WstRt*)ts->wst)->atoms[a];
}

const char* wst_atom_name(int a)
{
    return wst_atom_names[a];
}

void wst_free_rt(JSRuntime* rt, JSThreadState* ts)
{
    WstRt* w = ts->wst;
    int i;

    if (!w)
        return;
    for (i = 0; i < WSA_N; i++)
        JS_FreeAtomRT(rt, w->atoms[i]);
    JS_FreeValueRT(rt, w->rs_iter_proto);
    JS_FreeValueRT(rt, w->abort_ctor);
    JS_FreeValueRT(rt, w->ab_ctor);
    JS_FreeValueRT(rt, w->ab_transfer);
    JS_FreeValueRT(rt, w->sig_ctor);
    JS_FreeValueRT(rt, w->sig_add);
    JS_FreeValueRT(rt, w->sig_remove);
    JS_FreeValueRT(rt, w->blen_size);
    JS_FreeValueRT(rt, w->count_size);
    JS_FreeValueRT(rt, w->dec_ctor);
    JS_FreeValueRT(rt, w->dec_decode);
    JS_FreeValueRT(rt, w->type_error_ctor);
    JS_FreeValueRT(rt, w->pc_ctor);
    JS_FreeValueRT(rt, w->pc_close);
    for (i = 0; i < WSX_N; i++)
        JS_FreeValueRT(rt, w->x[i]);
    for (i = 0; i < WSTC_N; i++)
        JS_FreeValueRT(rt, w->ctors[i]);
    js_free_rt(rt, w);
    ts->wst = NULL;
}

void* wst_opaque(JSValueConst v, int k)
{
    return JS_GetOpaque(v, wst_cid[k]);
}

JSValue wst_proto(JSContext* ctx, int k)
{
    return JS_GetClassProto(ctx, wst_cid[k]);
}

JSValue wst_new_obj(JSContext* ctx, int k, JSValueConst proto, size_t sz, void** out)
{
    JSValue obj, p;
    JSValue* st;

    if (JS_IsUndefined(proto)) {
        p = JS_GetClassProto(ctx, wst_cid[k]);
        obj = JS_NewObjectProtoClassBare(ctx, p, wst_cid[k]);
        JS_FreeValue(ctx, p);
    } else {
        obj = JS_NewObjectProtoClassBare(ctx, proto, wst_cid[k]);
    }
    if (JS_IsException(obj))
        return obj;
    st = js_mallocz(ctx, sz);
    if (!st) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    *st = obj;
    JS_SetOpaque(obj, st);
    *out = st;
    return obj;
}

JSValue wst_ctl_get(JSContext* ctx, int k, void* c, JSValueConst stream, JSValue* slot)
{
    SCtlHead* h = c;
    JSValue p, o;
    if (!JS_IsUndefined(h->obj))
        return h->obj;
    p = JS_GetClassProto(ctx, wst_cid[k]);
    o = JS_NewObjectProtoClassBare(ctx, p, wst_cid[k]);
    JS_FreeValue(ctx, p);
    if (JS_IsException(o))
        return o;
    JS_SetOpaque(o, c);
    h->obj = o;
    h->stream = JS_DupValue(ctx, stream);
    *slot = o;
    return o;
}

void wst_ctl_drop(JSRuntime* rt, void* c)
{
    SCtlHead* h = c;
    if (JS_IsUndefined(h->obj))
        js_free_rt(rt, c);
    else
        h->s = NULL;
}

void wst_ctl_fin(JSRuntime* rt, void* c)
{
    SCtlHead* h = c;
    if (!h)
        return;
    JS_FreeValueRT(rt, h->stream);
    h->stream = JS_UNDEFINED;
    if (!h->s)
        js_free_rt(rt, c);
    else
        h->obj = JS_UNDEFINED;
}

void wst_ctl_mark(JSRuntime* rt, void* c, JS_MarkFunc* mf)
{
    SCtlHead* h = c;
    if (h)
        JS_MarkValue(rt, h->stream, mf);
}

JSValue wst_proto_from_target(JSContext* ctx, JSValueConst new_target)
{
    JSValue proto = JS_GetProperty(ctx, new_target, wst_atom(ctx, WSA_prototype));
    if (JS_IsException(proto) || JS_IsObject(proto))
        return proto;
    JS_FreeValue(ctx, proto);
    return JS_UNDEFINED;
}

void sdef_init(SDef* d)
{
    d->p = JS_UNDEFINED;
    d->st = 0;
    d->tag = 0;
}

int sdef_new(JSContext* ctx, SDef* d)
{
    JSValue p = JS_NewPromisePending(ctx);
    if (JS_IsException(p))
        return -1;
    JS_FreeValue(ctx, d->p);
    d->p = p;
    d->st = 0;
    return 0;
}

void sdef_resolve(JSContext* ctx, SDef* d, JSValueConst v)
{
    if (d->st != 0 || JS_IsUndefined(d->p))
        return;
    d->st = 1;
    dyn_settle(ctx, d->p, v, 0);
}

void sdef_reject(JSContext* ctx, SDef* d, JSValueConst e)
{
    if (d->st != 0 || JS_IsUndefined(d->p))
        return;
    d->st = 2;
    dyn_settle(ctx, d->p, e, 1);
}

int sdef_new_resolved(JSContext* ctx, SDef* d, JSValueConst v)
{
    if (sdef_new(ctx, d) < 0)
        return -1;
    sdef_resolve(ctx, d, v);
    return 0;
}

int sdef_new_rejected(JSContext* ctx, SDef* d, JSValueConst e)
{
    if (sdef_new(ctx, d) < 0)
        return -1;
    sdef_reject(ctx, d, e);
    JS_PromiseMarkHandled(ctx, d->p);
    return 0;
}

void sdef_free(JSRuntime* rt, SDef* d)
{
    JS_FreeValueRT(rt, d->p);
    d->p = JS_UNDEFINED;
}

void sdef_mark(JSRuntime* rt, SDef* d, JS_MarkFunc* mf)
{
    JS_MarkValue(rt, d->p, mf);
}

void sq_init(SQ* q, uint32_t esz)
{
    q->a = NULL;
    q->head = q->len = q->cap = 0;
    q->esz = esz;
}

int sq_grow(JSContext* ctx, SQ* q)
{
    uint32_t nc = q->cap ? q->cap * 2 : 4, i;
    uint8_t* na = js_malloc(ctx, (size_t)nc * q->esz);
    if (!na)
        return -1;
    for (i = 0; i < q->len; i++)
        memcpy(na + (size_t)i * q->esz, q->a + (size_t)((q->head + i) & (q->cap - 1)) * q->esz, q->esz);
    js_free(ctx, q->a);
    q->a = na;
    q->cap = nc;
    q->head = 0;
    return 0;
}

void* sq_push(JSContext* ctx, SQ* q)
{
    uint8_t* slot;

    if (q->len == q->cap && sq_grow(ctx, q) < 0)
        return NULL;
    slot = q->a + (size_t)((q->head + q->len) & (q->cap - 1)) * q->esz;
    memset(slot, 0, q->esz);
    q->len++;
    return slot;
}

void* sq_at(SQ* q, uint32_t i)
{
    return q->a + (size_t)((q->head + i) & (q->cap - 1)) * q->esz;
}

void* sq_peek(SQ* q)
{
    return q->len ? sq_at(q, 0) : NULL;
}

void sq_shift(SQ* q, void* out)
{
    memcpy(out, sq_at(q, 0), q->esz);
    q->head = (q->head + 1) & (q->cap - 1);
    q->len--;
    if (q->len == 0)
        q->head = 0;
}

void sq_release(JSRuntime* rt, SQ* q)
{
    js_free_rt(rt, q->a);
    q->a = NULL;
    q->head = q->len = q->cap = 0;
}

void sq_steal(SQ* dst, SQ* src)
{
    *dst = *src;
    src->a = NULL;
    src->head = src->len = src->cap = 0;
}

void sa_init(SAlgo* a)
{
    a->cfn = NULL;
    a->fn = JS_UNDEFINED;
    a->self = JS_UNDEFINED;
}

void sa_set_js(JSContext* ctx, SAlgo* a, JSValueConst fn, JSValueConst self)
{
    sa_clear(ctx, a);
    if (!JS_IsObject(fn))
        return;
    a->fn = JS_DupValue(ctx, fn);
    a->self = JS_DupValue(ctx, self);
}

void sa_set_c(JSContext* ctx, SAlgo* a, SAlgoC* cfn, JSValueConst data)
{
    sa_clear(ctx, a);
    a->cfn = cfn;
    a->data = JS_DupValue(ctx, data);
}

void sa_set_u(JSContext* ctx, SAlgo* a, SAlgoU* ufn, JSValueConst data)
{
    sa_clear(ctx, a);
    a->ufn = ufn;
    a->fn = JS_NULL;
    a->data = JS_DupValue(ctx, data);
}

static JSValue sa_upon_settle_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    return dyn_settle(ctx, data, arg, is_reject) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

void sa_clear(JSContext* ctx, SAlgo* a)
{
    JSValue f = a->fn, s = a->self;
    sa_init(a);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, s);
}

void sa_free(JSRuntime* rt, SAlgo* a)
{
    JS_FreeValueRT(rt, a->fn);
    JS_FreeValueRT(rt, a->self);
    sa_init(a);
}

void sa_mark(JSRuntime* rt, SAlgo* a, JS_MarkFunc* mf)
{
    JS_MarkValue(rt, a->fn, mf);
    JS_MarkValue(rt, a->self, mf);
}

JSValue sa_call(JSContext* ctx, SAlgo* a, int argc, JSValueConst* argv)
{
    JSValue r, f, s;
    int k = sa_kind(a);

    if (k == SA_C) {
        JSValue d = JS_DupValue(ctx, a->data);
        r = a->cfn(ctx, d, argc, argv);
        JS_FreeValue(ctx, d);
        return r;
    }
    if (k == SA_U) {
        JSValue d, rx;
        r = JS_NewPromisePending(ctx);
        if (JS_IsException(r))
            return r;
        rx = JS_NewPromiseReaction(ctx, sa_upon_settle_rx, r, 0);
        if (JS_IsException(rx)) {
            JS_FreeValue(ctx, r);
            return rx;
        }
        d = JS_DupValue(ctx, a->data);
        a->ufn(ctx, d, argc, argv, rx);
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, rx);
        return r;
    }
    if (k != SA_JS)
        return wst_resolved_undef(ctx);
    f = JS_DupValue(ctx, a->fn);
    s = JS_DupValue(ctx, a->self);
    r = wst_promise_call(ctx, f, s, argc, argv);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, s);
    return r;
}

void sa_call_upon(JSContext* ctx, SAlgo* a, int argc, JSValueConst* argv, JSValueConst rx)
{
    JSValue r, p;
    int k = sa_kind(a);
    if (JS_IsException(rx))
        return;
    if (k == SA_U) {
        JSValue d = JS_DupValue(ctx, a->data);
        a->ufn(ctx, d, argc, argv, rx);
        JS_FreeValue(ctx, d);
        return;
    }
    if (k == SA_JS || k == SA_NONE) {
        if (k == SA_NONE) {
            JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
            return;
        }
        {
            JSValue f = JS_DupValue(ctx, a->fn), s = JS_DupValue(ctx, a->self);
            r = JS_Call(ctx, f, s, argc, argv);
            JS_FreeValue(ctx, f);
            JS_FreeValue(ctx, s);
        }
        if (JS_IsException(r)) {
            JSValue e;
            if (JS_HasUncatchableException(ctx))
                return;
            e = JS_GetException(ctx);
            JS_EnqueueReaction(ctx, rx, e, 1);
            JS_FreeValue(ctx, e);
            return;
        }
        if (!JS_IsObject(r)) {
            JS_EnqueueReaction(ctx, rx, r, 0);
            JS_FreeValue(ctx, r);
            return;
        }
        p = wst_promise_resolved_with(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(p))
            p = wst_reject_exc(ctx);
    } else {
        p = sa_call(ctx, a, argc, argv);
    }
    if (!JS_IsException(p))
        JS_PromiseReact(ctx, p, rx);
    JS_FreeValue(ctx, p);
}

static JSValue wst_verr(JSContext* ctx, int range, const char* fmt, va_list ap) __attribute__((format(printf, 3, 0)));
static JSValue wst_verr(JSContext* ctx, int range, const char* fmt, va_list ap)
{
    if (range)
        JS_ThrowRangeErrorV(ctx, fmt, ap);
    else
        JS_ThrowTypeErrorV(ctx, fmt, ap);
    if (JS_HasUncatchableException(ctx))
        return JS_EXCEPTION;
    return JS_GetException(ctx);
}

JSValue wst_err(JSContext* ctx, int range, const char* fmt, ...)
{
    va_list ap;
    JSValue e;
    va_start(ap, fmt);
    e = wst_verr(ctx, range, fmt, ap);
    va_end(ap);
    return e;
}

JSValue wst_throw(JSContext* ctx, int range, const char* fmt, ...)
{
    va_list ap;
    JSValue e;
    va_start(ap, fmt);
    e = wst_verr(ctx, range, fmt, ap);
    va_end(ap);
    return JS_Throw(ctx, e);
}

JSValue wst_rejected(JSContext* ctx, JSValueConst e)
{
    JSValue p;
    if (JS_IsException(e))
        return JS_EXCEPTION;
    p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p) && dyn_settle(ctx, p, e, 1) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

JSValue wst_reject_err(JSContext* ctx, int range, const char* msg)
{
    JSValue e = wst_err(ctx, range, "%s", msg), p;
    p = wst_rejected(ctx, e);
    JS_FreeValue(ctx, e);
    return p;
}

JSValue wst_reject_exc(JSContext* ctx)
{
    JSValue e, p;
    if (JS_HasUncatchableException(ctx))
        return JS_EXCEPTION;
    e = JS_GetException(ctx);
    p = wst_rejected(ctx, e);
    JS_FreeValue(ctx, e);
    return p;
}

JSValue wst_resolved_undef(JSContext* ctx)
{
    JSValue p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p) && dyn_settle(ctx, p, JS_UNDEFINED, 0) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

JSValue wst_promise_resolved_with(JSContext* ctx, JSValueConst v)
{
    JSValue f[2], r, p;
    if (!JS_IsObject(v))
        return JS_PromiseResolveValue(ctx, v);
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p))
        return p;
    r = JS_Call(ctx, f[0], JS_UNDEFINED, 1, &v);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, p);
        return r;
    }
    JS_FreeValue(ctx, r);
    return p;
}

JSValue wst_promise_call(JSContext* ctx, JSValueConst fn, JSValueConst self, int argc, JSValueConst* argv)
{
    JSValue r = JS_Call(ctx, fn, self, argc, argv), p;
    if (JS_IsException(r))
        return wst_reject_exc(ctx);
    p = wst_promise_resolved_with(ctx, r);
    JS_FreeValue(ctx, r);
    if (JS_IsException(p))
        return wst_reject_exc(ctx);
    return p;
}

JSValue wst_iter_result(JSContext* ctx, JSValueConst v, int done)
{
    return JS_NewIterResult(ctx, JS_DupValue(ctx, v), done);
}

JSValue wst_rx(JSContext* ctx, JSValue* slot, JSPromiseReactionFunc* fn, JSValueConst data, int magic)
{
    if (JS_IsUndefined(*slot))
        *slot = JS_NewPromiseReaction(ctx, fn, data, magic);
    return *slot;
}

int wst_upon(JSContext* ctx, JSValueConst p, JSValueConst rx)
{
    if (JS_IsException(rx))
        return -1;
    return JS_PromiseReact(ctx, p, rx);
}

int wst_upon_resolved_with(JSContext* ctx, JSValueConst v, JSPromiseReactionFunc* fn, JSValueConst data)
{
    JSValue rx = JS_NewPromiseReaction(ctx, fn, data, 0);
    int ret;
    if (JS_IsException(rx))
        return -1;
    ret = JS_PromiseUponResolvedWith(ctx, v, rx);
    JS_FreeValue(ctx, rx);
    return ret;
}

int wst_upon_start(JSContext* ctx, JSValueConst v, JSJobFunc* job, JSPromiseReactionFunc* fn, JSValueConst data)
{
    JSValueConst a[2];
    if (JS_IsObject(v))
        return wst_upon_resolved_with(ctx, v, fn, data);
    a[0] = data;
    a[1] = v;
    return JS_EnqueueJob(ctx, job, 2, a);
}

static JSValue wst_transform_undef_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    return dyn_settle(ctx, data, is_reject ? arg : JS_UNDEFINED, is_reject) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue wst_transform_undef(JSContext* ctx, JSValueConst p)
{
    JSValue q = JS_NewPromisePending(ctx), rx;
    if (JS_IsException(q))
        return q;
    rx = JS_NewPromiseReaction(ctx, wst_transform_undef_rx, q, 0);
    if (JS_IsException(rx) || JS_PromiseReact(ctx, p, rx) < 0) {
        JS_FreeValue(ctx, rx);
        JS_FreeValue(ctx, q);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, rx);
    return q;
}

void wst_drop_exc(JSContext* ctx)
{
    if (!JS_HasUncatchableException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

JSValue wst_ret(JSContext* ctx, JSValue v)
{
    if (!JS_HasUncatchableException(ctx))
        return v;
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

int wst_to_dict(JSContext* ctx, JSValueConst v, const char* what, JSValue* out)
{
    *out = JS_NULL;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsObject(v)) {
        wst_throw(ctx, 0, "%s must be an object", what);
        return -1;
    }
    *out = JS_DupValue(ctx, v);
    return 0;
}

JSValue wst_dict_get(JSContext* ctx, JSValueConst d, int a)
{
    if (JS_IsNull(d))
        return JS_UNDEFINED;
    return JS_GetProperty(ctx, d, wst_atom(ctx, a));
}

int wst_cb_member(JSContext* ctx, JSValueConst d, int a, const char* what, JSValue* out)
{
    JSValue v = wst_dict_get(ctx, d, a);
    *out = JS_UNDEFINED;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        wst_throw(ctx, 0, "%s.%s must be a function", what, wst_atom_names[a]);
        return -1;
    }
    *out = v;
    return 0;
}

int wst_enforce_ull(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v) < 0)
        return -1;
    if (isnan(x) || isinf(x)) {
        wst_throw(ctx, 0, "%s must be a finite number", what);
        return -1;
    }
    x = x < 0 ? ceil(x) : floor(x);
    if (x < 0 || x > 9007199254740991.0) {
        wst_throw(ctx, 0, "%s is out of range", what);
        return -1;
    }
    *out = x + 0.0;
    return 0;
}

int wst_strategy(JSContext* ctx, JSValueConst s, const char* what, double* hwm, int* hwm_set, JSValue* size)
{
    JSValue d, raw;
    *hwm_set = 0;
    *size = JS_UNDEFINED;
    if (wst_to_dict(ctx, s, what, &d) < 0)
        return -1;
    raw = wst_dict_get(ctx, d, WSA_highWaterMark);
    if (JS_IsException(raw))
        goto fail;
    if (!JS_IsUndefined(raw)) {
        int r = JS_ToFloat64(ctx, hwm, raw);
        JS_FreeValue(ctx, raw);
        if (r < 0)
            goto fail;
        *hwm_set = 1;
    }
    if (wst_cb_member(ctx, d, WSA_size, what, size) < 0)
        goto fail;
    JS_FreeValue(ctx, d);
    return 0;
fail:
    JS_FreeValue(ctx, d);
    return -1;
}

int wst_extract_hwm(JSContext* ctx, double hwm, int hwm_set, double def, double* out)
{
    if (!hwm_set) {
        *out = def;
        return 0;
    }
    if (isnan(hwm) || hwm < 0) {
        wst_throw(ctx, 1, "highWaterMark must be a non-negative number");
        return -1;
    }
    *out = hwm;
    return 0;
}

int wst_size_call(JSContext* ctx, JSValueConst size_fn, JSValueConst chunk, JSValue* out)
{
    JSValue f;
    f = JS_DupValue(ctx, size_fn);
    *out = JS_Call(ctx, f, JS_UNDEFINED, 1, &chunk);
    JS_FreeValue(ctx, f);
    return JS_IsException(*out) ? -1 : 0;
}

int wst_queue_push_slow(JSContext* ctx, SQ* q, double* total, JSValueConst v, JSValueConst size)
{
    double s;
    SQV* e;
    if (!JS_IsNumber(size) || (JS_ToFloat64(ctx, &s, size), isnan(s)) || s < 0) {
        wst_throw(ctx, 1, "chunk size must be a finite, non-negative number");
        return -1;
    }
    if (isinf(s)) {
        wst_throw(ctx, 1, "chunk size must be finite");
        return -1;
    }
    e = sq_push_t(ctx, q, sizeof(SQV));
    if (!e)
        return -1;
    e->v = JS_DupValue(ctx, v);
    e->size = s;
    e->sentinel = 0;
    *total += s;
    return 0;
}

JSValue wst_illegal_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    WST_NEED_NEW(ctx, new_target);
    return wst_throw(ctx, 0, "Illegal constructor");
}

int wst_register(JSContext* ctx, const WstClassSpec* sp, JSValue* ctor_out)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSClassDef def;
    JSValue proto, ctor;

    JS_NewClassID(&wst_cid[sp->k]);
    if (!JS_IsRegisteredClass(rt, wst_cid[sp->k])) {
        memset(&def, 0, sizeof(def));
        def.class_name = sp->name;
        def.finalizer = sp->fin;
        def.gc_mark = sp->mark;
        if (JS_NewClass(rt, wst_cid[sp->k], &def) < 0)
            return -1;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (sp->nproto && JS_SetPropertyFunctionList(ctx, proto, sp->proto, sp->nproto) < 0)
        goto fail;
    *ctor_out = JS_UNDEFINED;
    if (sp->ctor) {
        ctor = JS_NewCFunction2(ctx, sp->ctor, sp->name, sp->len, JS_CFUNC_constructor_or_func, 0);
        if (JS_IsException(ctor))
            goto fail;
        if (JS_SetConstructor(ctx, ctor, proto) < 0
            || (sp->nstat && JS_SetPropertyFunctionList(ctx, ctor, sp->stat, sp->nstat) < 0)) {
            JS_FreeValue(ctx, ctor);
            goto fail;
        }
        *ctor_out = ctor;
    }
    JS_SetClassProto(ctx, wst_cid[sp->k], proto);
    return 0;
fail:
    JS_FreeValue(ctx, proto);
    return -1;
}

static JSClassID wst_then_cid;

typedef struct WstThen {
    JSPromiseReactionFunc* fn;
    JSValue q;
    JSValue data;
    int magic;
} WstThen;

static void wst_then_finalizer(JSRuntime* rt, JSValue val)
{
    WstThen* t = JS_GetOpaque(val, wst_then_cid);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->q);
    JS_FreeValueRT(rt, t->data);
    js_free_rt(rt, t);
}

static void wst_then_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    WstThen* t = JS_GetOpaque(val, wst_then_cid);
    if (!t)
        return;
    JS_MarkValue(rt, t->q, mf);
    JS_MarkValue(rt, t->data, mf);
}

static const JSClassDef wst_then_class = { "WstThen", .finalizer = wst_then_finalizer, .gc_mark = wst_then_mark };

static JSValue wst_then_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    WstThen* t = JS_GetOpaque(data, wst_then_cid);
    JSValue r, q = JS_DupValue(ctx, t->q);
    int rc;
    (void)magic;
    if (!t->fn)
        r = is_reject ? JS_Throw(ctx, JS_DupValue(ctx, arg)) : JS_DupValue(ctx, arg);
    else
        r = t->fn(ctx, t->data, t->magic, is_reject, arg);
    if (JS_IsException(r)) {
        JSValue e;
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, q);
            return JS_EXCEPTION;
        }
        e = JS_GetException(ctx);
        rc = dyn_settle(ctx, q, e, 1);
        JS_FreeValue(ctx, e);
    } else {
        rc = dyn_settle(ctx, q, r, 0);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, q);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue wst_then(JSContext* ctx, JSValueConst p, JSPromiseReactionFunc* fn, JSValueConst data, int magic)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue rec, rx, q;
    WstThen* t;

    if (JS_IsException(p))
        return JS_EXCEPTION;
    JS_NewClassID(&wst_then_cid);
    if (!JS_IsRegisteredClass(rt, wst_then_cid) && JS_NewClass(rt, wst_then_cid, &wst_then_class) < 0)
        return JS_EXCEPTION;
    q = JS_NewPromisePending(ctx);
    if (JS_IsException(q))
        return q;
    rec = JS_NewObjectProtoClass(ctx, JS_NULL, wst_then_cid);
    t = js_mallocz(ctx, sizeof(*t));
    if (JS_IsException(rec) || !t) {
        js_free(ctx, t);
        JS_FreeValue(ctx, rec);
        JS_FreeValue(ctx, q);
        return JS_EXCEPTION;
    }
    t->fn = fn;
    t->q = JS_DupValue(ctx, q);
    t->data = JS_DupValue(ctx, data);
    t->magic = magic;
    JS_SetOpaque(rec, t);
    rx = JS_NewPromiseReaction(ctx, wst_then_rx, rec, 0);
    JS_FreeValue(ctx, rec);
    if (JS_IsException(rx) || JS_PromiseReact(ctx, p, rx) < 0) {
        JS_FreeValue(ctx, rx);
        JS_FreeValue(ctx, q);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, rx);
    return q;
}
