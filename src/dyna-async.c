#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASYNC)

#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "modules/libc/libc-internal.h"

typedef struct {
    JSValue* v;
    uint32_t head, len, cap;
} AsRing;

static inline uint32_t as_ring_ix(const AsRing* r, uint32_t i)
{
    uint32_t x = r->head + i;
    return x >= r->cap ? x - r->cap : x;
}

static int as_ring_push(JSContext* ctx, AsRing* r, JSValue v)
{
    if (r->len == r->cap) {
        uint32_t oc = r->cap, nc = oc ? oc * 2 : 4;
        JSValue* nv = js_realloc(ctx, r->v, nc * sizeof(*nv));
        if (!nv) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (r->head) {
            uint32_t tail = oc - r->head;
            memmove(nv + nc - tail, nv + r->head, tail * sizeof(*nv));
            r->head = nc - tail;
        }
        r->v = nv;
        r->cap = nc;
    }
    r->v[as_ring_ix(r, r->len)] = v;
    r->len++;
    return 0;
}

static JSValue as_ring_shift(AsRing* r)
{
    JSValue v;
    if (!r->len)
        return JS_UNDEFINED;
    v = r->v[r->head];
    if (++r->head == r->cap)
        r->head = 0;
    r->len--;
    return v;
}

static void as_ring_free(JSRuntime* rt, AsRing* r)
{
    uint32_t i;
    for (i = 0; i < r->len; i++)
        JS_FreeValueRT(rt, r->v[as_ring_ix(r, i)]);
    js_free_rt(rt, r->v);
    r->v = NULL;
    r->len = r->cap = r->head = 0;
}

static void as_ring_mark(JSRuntime* rt, AsRing* r, JS_MarkFunc* mf)
{
    uint32_t i;
    for (i = 0; i < r->len; i++)
        JS_MarkValue(rt, r->v[as_ring_ix(r, i)], mf);
}

enum { AS_SEM, AS_REL, AS_QUEUE, AS_CHAN, AS_CHIT, AS_POOL, AS_PMAP, AS_RETRY, AS_TIMED, AS_NCLASS };
static JSClassID as_cid[AS_NCLASS];

typedef struct {
    int32_t avail;
    AsRing waiters;
} AsSem;

typedef struct {
    JSValue sem;
    uint8_t used;
} AsRel;

typedef struct {
    double cap;
    uint8_t closed, iterating;
    AsRing q, blocked;
    JSValue receiver;
} AsChan;

typedef struct {
    int32_t n, active;
    AsRing q;
} AsPool;

typedef struct {
    JSValue items, fn, results, outer, resolved;
    uint32_t n, next, inflight, settled, conc;
    uint8_t failed;
} AsPmap;

typedef struct {
    JSValue fn, on_retry, outer, run;
    double backoff, factor;
    int32_t retries, attempt;
} AsRetry;

typedef struct {
    JSValue fn, self, timer, fire;
    JSValue* av;
    uint32_t ac;
    double ms, last;
    uint8_t throttle;
} AsTimed;

static void* as_get(JSContext* ctx, JSValueConst v, int k)
{
    return JS_GetOpaque2(ctx, v, as_cid[k]);
}

static int as_number(JSContext* ctx, JSValueConst v, double* d)
{
    if (!JS_IsNumber(v))
        return 0;
    JS_ToFloat64(ctx, d, v);
    return 1;
}

static int as_is_i32(double d)
{
    return d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d;
}

static int as_pos_int(JSContext* ctx, JSValueConst v, int32_t* out)
{
    double d;
    if (!as_number(ctx, v, &d) || !as_is_i32(d) || d < 1)
        return 0;
    *out = (int32_t)d;
    return 1;
}

static int as_ms(JSContext* ctx, JSValueConst v, double* d)
{
    return as_number(ctx, v, d) && isfinite(*d) && *d >= 0;
}

static JSValue as_set_timeout(JSContext* ctx, JSValueConst fn, double ms)
{
    JSValueConst a[2];
    a[0] = fn;
    a[1] = JS_NewFloat64(ctx, ms);
    return js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
}

static void as_clear_timeout(JSContext* ctx, JSValue* t)
{
    if (!JS_IsNull(*t) && !JS_IsUndefined(*t)) {
        JSValue r = js_os_clearTimeout(ctx, JS_UNDEFINED, 1, (JSValueConst*)t);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, *t);
    }
    *t = JS_NULL;
}

static JSValue as_rejected(JSContext* ctx)
{
    JSValue e, p;
    if (JS_HasUncatchableException(ctx))
        return JS_EXCEPTION;
    e = JS_GetException(ctx);
    p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p) && dyn_settle(ctx, p, e, 1) < 0) {
        JS_FreeValue(ctx, p);
        p = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, e);
    return p;
}

static JSValue as_resolved(JSContext* ctx, JSValueConst v)
{
    JSValue p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p) && dyn_settle(ctx, p, v, 0) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static int as_settle_exc(JSContext* ctx, JSValueConst p)
{
    JSValue e;
    int rc;
    if (JS_HasUncatchableException(ctx))
        return -1;
    e = JS_GetException(ctx);
    rc = dyn_settle(ctx, p, e, 1);
    JS_FreeValue(ctx, e);
    return rc;
}

static int as_react(JSContext* ctx, JSValueConst promise, JSPromiseReactionFunc* fn, JSValueConst data, int magic)
{
    JSValue r = JS_NewPromiseReaction(ctx, fn, data, magic);
    int rc;
    if (JS_IsException(r))
        return -1;
    rc = JS_PromiseReact(ctx, promise, r);
    JS_FreeValue(ctx, r);
    return rc;
}

static int as_react_value(JSContext* ctx, JSValue v, JSPromiseReactionFunc* fn, JSValueConst data, int magic)
{
    JSValue p = JS_PromiseResolveValue(ctx, v);
    int rc;
    JS_FreeValue(ctx, v);
    if (JS_IsException(p))
        return -1;
    rc = as_react(ctx, p, fn, data, magic);
    JS_FreeValue(ctx, p);
    return rc;
}

static JSValue as_new(JSContext* ctx, JSValueConst new_target, int k, void* opaque)
{
    JSValue proto, obj;
    if (JS_IsUndefined(new_target))
        proto = JS_GetClassProto(ctx, as_cid[k]);
    else
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, as_cid[k]);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, opaque);
    return obj;
}

static JSValue as_alloc_obj(JSContext* ctx, JSValueConst new_target, int k, size_t sz, void** out)
{
    void* p;
    JSValue obj;
    if (JS_IsUndefined(new_target) && k != AS_REL && k != AS_CHIT && k != AS_PMAP && k != AS_RETRY && k != AS_TIMED)
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    p = js_mallocz(ctx, sz);
    if (!p)
        return JS_EXCEPTION;
    obj = as_new(ctx, new_target, k, p);
    if (JS_IsException(obj)) {
        js_free(ctx, p);
        return obj;
    }
    *out = p;
    return obj;
}

static JSValue as_sleep_fire(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    return dyn_settle(ctx, data[0], JS_UNDEFINED, 0) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue as_sleep(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double ms;
    JSValue p, fn, t;
    (void)this_val;
    if (!as_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &ms))
        return JS_ThrowTypeError(ctx, "sleep: ms must be a non-negative finite number");
    p = JS_NewPromisePending(ctx);
    if (JS_IsException(p))
        return p;
    fn = JS_NewCFunctionData(ctx, as_sleep_fire, 0, 0, 1, (JSValueConst*)&p);
    t = JS_IsException(fn) ? JS_EXCEPTION : as_set_timeout(ctx, fn, ms);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, p);
        return t;
    }
    return p;
}

static JSValue as_wt_fire(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    JSValue e;
    int rc;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (JS_PromiseState(ctx, data[0]) != JS_PROMISE_PENDING)
        return JS_UNDEFINED;
    e = JS_NewError(ctx);
    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_DupValue(ctx, data[1]), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    rc = dyn_settle(ctx, data[0], e, 1);
    JS_FreeValue(ctx, e);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue as_wt_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue outer = JS_GetPropertyUint32(ctx, data, 0), t;
    int rc = 0;
    (void)magic;
    if (JS_PromiseState(ctx, outer) == JS_PROMISE_PENDING) {
        t = JS_GetPropertyUint32(ctx, data, 1);
        as_clear_timeout(ctx, &t);
        rc = dyn_settle(ctx, outer, arg, is_reject);
    }
    JS_FreeValue(ctx, outer);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue as_wt_thenable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    return as_wt_done(ctx, data[0], 0, magic, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue as_with_timeout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst p = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue then, outer, fire, t, st, msg, d[2];
    double ms;
    int rc;
    (void)this_val;
    if (!JS_ToBool(ctx, p))
        return JS_ThrowTypeError(ctx, "withTimeout: first argument must be a promise");
    then = JS_IsObject(p) || JS_IsString(p) ? JS_GetPropertyStr(ctx, p, "then") : JS_UNDEFINED;
    if (JS_IsException(then))
        return then;
    if (!JS_IsFunction(ctx, then)) {
        JS_FreeValue(ctx, then);
        return JS_ThrowTypeError(ctx, "withTimeout: first argument must be a promise");
    }
    if (!as_ms(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &ms)) {
        JS_FreeValue(ctx, then);
        return JS_ThrowTypeError(ctx, "withTimeout: ms must be a non-negative finite number");
    }
    if (argc > 2 && JS_IsString(argv[2]) && JS_ToBool(ctx, argv[2])) {
        msg = JS_DupValue(ctx, argv[2]);
    } else {
        char b[96];
        JSValue n = JS_NewFloat64(ctx, ms), ns = JS_ToString(ctx, n);
        const char* c = JS_IsException(ns) ? NULL : JS_ToCString(ctx, ns);
        JS_FreeValue(ctx, ns);
        if (!c) {
            JS_FreeValue(ctx, then);
            return JS_EXCEPTION;
        }
        snprintf(b, sizeof b, "withTimeout: timed out after %s ms", c);
        JS_FreeCString(ctx, c);
        msg = JS_NewString(ctx, b);
    }
    outer = JS_NewPromisePending(ctx);
    st = JS_NewArray(ctx);
    if (JS_IsException(msg) || JS_IsException(outer) || JS_IsException(st)
        || JS_SetPropertyUint32(ctx, st, 0, JS_DupValue(ctx, outer)) < 0 || JS_SetPropertyUint32(ctx, st, 1, JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, msg);
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, then);
        JS_FreeValue(ctx, outer);
        return JS_EXCEPTION;
    }
    d[0] = outer;
    d[1] = msg;
    fire = JS_NewCFunctionData(ctx, as_wt_fire, 0, 0, 2, (JSValueConst*)d);
    JS_FreeValue(ctx, msg);
    t = JS_IsException(fire) ? JS_EXCEPTION : as_set_timeout(ctx, fire, ms);
    JS_FreeValue(ctx, fire);
    if (JS_IsException(t) || JS_SetPropertyUint32(ctx, st, 1, t) < 0) {
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, then);
        JS_FreeValue(ctx, outer);
        return JS_EXCEPTION;
    }
    if (JS_GetClassID(p) == JS_GetClassID(outer)) {
        JS_PromiseMarkHandled(ctx, p);
        rc = as_react(ctx, p, as_wt_done, st, 0);
    } else {
        JSValue f[2], r;
        f[0] = JS_NewCFunctionData(ctx, as_wt_thenable, 1, 0, 1, (JSValueConst*)&st);
        f[1] = JS_NewCFunctionData(ctx, as_wt_thenable, 1, 1, 1, (JSValueConst*)&st);
        r = JS_IsException(f[0]) || JS_IsException(f[1]) ? JS_EXCEPTION : JS_Call(ctx, then, p, 2, (JSValueConst*)f);
        JS_FreeValue(ctx, f[0]);
        JS_FreeValue(ctx, f[1]);
        rc = JS_IsException(r) ? -1 : 0;
        JS_FreeValue(ctx, r);
        if (rc < 0) {
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, then);
                JS_FreeValue(ctx, st);
                JS_FreeValue(ctx, outer);
                return JS_EXCEPTION;
            }
            r = JS_GetException(ctx);
            rc = JS_IsException(as_wt_done(ctx, st, 0, 1, r)) ? -1 : 0;
            JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, then);
    JS_FreeValue(ctx, st);
    if (rc < 0) {
        JS_FreeValue(ctx, outer);
        return JS_EXCEPTION;
    }
    return outer;
}

static JSValue as_retry_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg);

static JSValue as_retry_call(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    AsRetry* r = JS_GetOpaque(data, as_cid[AS_RETRY]);
    JSValue v;
    (void)magic;
    (void)is_reject;
    (void)arg;
    v = JS_Call(ctx, r->fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(v)) {
        JSValue e;
        if (JS_HasUncatchableException(ctx))
            return JS_EXCEPTION;
        e = JS_GetException(ctx);
        v = as_retry_done(ctx, data, 0, 1, e);
        JS_FreeValue(ctx, e);
        return v;
    }
    if (as_react_value(ctx, v, as_retry_done, data, 0) < 0 && as_settle_exc(ctx, r->outer) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue as_retry_run(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    return as_retry_call(ctx, data[0], 0, 0, JS_UNDEFINED);
}

static JSValue as_retry_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    AsRetry* r = JS_GetOpaque(data, as_cid[AS_RETRY]);
    double wait;
    (void)magic;
    if (!is_reject || r->attempt >= r->retries)
        return dyn_settle(ctx, r->outer, arg, is_reject) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    wait = r->backoff * pow(r->factor, r->attempt);
    r->attempt++;
    if (!JS_IsNull(r->on_retry)) {
        JSValue a[2], h;
        a[0] = (JSValue)arg;
        a[1] = JS_NewInt32(ctx, r->attempt);
        h = JS_Call(ctx, r->on_retry, JS_UNDEFINED, 2, (JSValueConst*)a);
        if (JS_IsException(h)) {
            if (as_settle_exc(ctx, r->outer) < 0)
                return JS_EXCEPTION;
            return JS_UNDEFINED;
        }
        JS_FreeValue(ctx, h);
    }
    if (wait > 0) {
        JSValue t = as_set_timeout(ctx, r->run, wait);
        if (JS_IsException(t))
            as_settle_exc(ctx, r->outer);
        JS_FreeValue(ctx, t);
    } else if (as_react_value(ctx, JS_UNDEFINED, as_retry_call, data, 0) < 0) {
        as_settle_exc(ctx, r->outer);
    }
    return JS_UNDEFINED;
}

static int as_opt(JSContext* ctx, JSValueConst opts, const char* k, JSValue* out)
{
    *out = JS_GetPropertyStr(ctx, opts, k);
    if (JS_IsException(*out))
        return -1;
    if (JS_IsNull(*out))
        *out = JS_UNDEFINED;
    return !JS_IsUndefined(*out);
}

static JSValue as_retry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED, opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    int32_t retries = 3;
    double backoff = 0, factor = 2, d;
    JSValue on_retry = JS_NULL, v, obj;
    AsRetry* r;
    int k;
    (void)this_val;
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "retry: fn must be a function");
    if (!JS_IsNull(opts) && !JS_IsUndefined(opts)) {
        if (!JS_IsObject(opts) || JS_IsFunction(ctx, opts))
            return JS_ThrowTypeError(ctx, "retry: opts must be an object");
        if ((k = as_opt(ctx, opts, "retries", &v)) < 0)
            return JS_EXCEPTION;
        if (k) {
            int ok = as_number(ctx, v, &d) && as_is_i32(d) && d >= 0;
            JS_FreeValue(ctx, v);
            if (!ok)
                return JS_ThrowTypeError(ctx, "retry: retries must be a non-negative integer");
            retries = (int32_t)d;
        }
        if ((k = as_opt(ctx, opts, "backoffMs", &v)) < 0)
            return JS_EXCEPTION;
        if (k) {
            int ok = as_number(ctx, v, &d) && isfinite(d) && d >= 0;
            JS_FreeValue(ctx, v);
            if (!ok)
                return JS_ThrowTypeError(ctx, "retry: backoffMs must be a non-negative number");
            backoff = d;
        }
        if ((k = as_opt(ctx, opts, "factor", &v)) < 0)
            return JS_EXCEPTION;
        if (k) {
            int ok = as_number(ctx, v, &d) && isfinite(d) && d >= 1;
            JS_FreeValue(ctx, v);
            if (!ok)
                return JS_ThrowTypeError(ctx, "retry: factor must be >= 1");
            factor = d;
        }
        if ((k = as_opt(ctx, opts, "onRetry", &v)) < 0)
            return JS_EXCEPTION;
        if (k) {
            if (!JS_IsFunction(ctx, v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "retry: onRetry must be a function");
            }
            on_retry = v;
        }
    }
    obj = as_alloc_obj(ctx, JS_UNDEFINED, AS_RETRY, sizeof(AsRetry), (void**)&r);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, on_retry);
        return obj;
    }
    r->fn = JS_DupValue(ctx, fn);
    r->on_retry = on_retry;
    r->retries = retries;
    r->backoff = backoff;
    r->factor = factor;
    r->outer = JS_NewPromisePending(ctx);
    r->run = JS_NewCFunctionData(ctx, as_retry_run, 0, 0, 1, (JSValueConst*)&obj);
    if (JS_IsException(r->outer) || JS_IsException(r->run)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    v = as_retry_call(ctx, obj, 0, 0, JS_UNDEFINED);
    if (!JS_IsException(v))
        v = JS_DupValue(ctx, r->outer);
    JS_FreeValue(ctx, obj);
    return v;
}

static JSValue as_rel_new(JSContext* ctx, JSValueConst sem)
{
    AsRel* r;
    JSValue obj = as_alloc_obj(ctx, JS_UNDEFINED, AS_REL, sizeof(AsRel), (void**)&r);
    if (!JS_IsException(obj))
        r->sem = JS_DupValue(ctx, sem);
    return obj;
}

static int as_sem_release(JSContext* ctx, JSValueConst semobj)
{
    AsSem* s = JS_GetOpaque(semobj, as_cid[AS_SEM]);
    JSValue p, rel;
    int rc;
    if (!s->waiters.len) {
        s->avail++;
        return 0;
    }
    p = as_ring_shift(&s->waiters);
    rel = as_rel_new(ctx, semobj);
    if (JS_IsException(rel)) {
        as_settle_exc(ctx, p);
        JS_FreeValue(ctx, p);
        return -1;
    }
    rc = dyn_settle(ctx, p, rel, 0);
    JS_FreeValue(ctx, rel);
    JS_FreeValue(ctx, p);
    return rc;
}

static JSValue as_rel_call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_val, int argc, JSValueConst* argv, int flags)
{
    AsRel* r = JS_GetOpaque(func_obj, as_cid[AS_REL]);
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)flags;
    if (!r || r->used)
        return JS_UNDEFINED;
    r->used = 1;
    if (as_sem_release(ctx, r->sem) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue as_sem_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int32_t n;
    AsSem* s;
    JSValue obj;
    if (!as_pos_int(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &n))
        return JS_ThrowTypeError(ctx, "Semaphore: n must be a positive integer");
    obj = as_alloc_obj(ctx, new_target, AS_SEM, sizeof(AsSem), (void**)&s);
    if (!JS_IsException(obj))
        s->avail = n;
    return obj;
}

static JSValue as_sem_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    AsSem* s = as_get(ctx, this_val, AS_SEM);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, magic ? (int32_t)s->waiters.len : s->avail);
}

static JSValue as_sem_acquire(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsSem* s = as_get(ctx, this_val, AS_SEM);
    JSValue p, rel;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->avail > 0) {
        s->avail--;
        rel = as_rel_new(ctx, this_val);
        if (JS_IsException(rel))
            return rel;
        p = as_resolved(ctx, rel);
        JS_FreeValue(ctx, rel);
        return p;
    }
    p = JS_NewPromisePending(ctx);
    if (JS_IsException(p) || as_ring_push(ctx, &s->waiters, JS_DupValue(ctx, p)) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static JSValue as_sem_run_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue rel = JS_GetPropertyUint32(ctx, data, 3), outer = JS_GetPropertyUint32(ctx, data, 2), r;
    int rc;
    (void)magic;
    r = JS_Call(ctx, rel, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, rel);
            JS_FreeValue(ctx, outer);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    rc = dyn_settle(ctx, outer, arg, is_reject);
    JS_FreeValue(ctx, rel);
    JS_FreeValue(ctx, outer);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue as_sem_run_go(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue fn = JS_GetPropertyUint32(ctx, data, 0), args = JS_GetPropertyUint32(ctx, data, 1), outer = JS_GetPropertyUint32(ctx, data, 2), v, r, ret = JS_UNDEFINED;
    JSValue* av = NULL;
    uint32_t n = 0, i;
    (void)magic;
    if (is_reject) {
        if (dyn_settle(ctx, outer, arg, 1) < 0)
            ret = JS_EXCEPTION;
        goto done;
    }
    JS_SetPropertyUint32(ctx, data, 3, JS_DupValue(ctx, arg));
    if (dyn_js_len_u32(ctx, args, &n)) {
        as_settle_exc(ctx, outer);
        goto done;
    }
    if (n) {
        av = js_malloc(ctx, n * sizeof(*av));
        if (!av) {
            as_settle_exc(ctx, outer);
            goto done;
        }
        for (i = 0; i < n; i++)
            av[i] = JS_GetPropertyUint32(ctx, args, i);
    }
    v = JS_Call(ctx, fn, JS_NULL, (int)n, (JSValueConst*)av);
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, av[i]);
    js_free(ctx, av);
    if (JS_IsException(v)) {
        JSValue e;
        if (JS_HasUncatchableException(ctx)) {
            ret = JS_EXCEPTION;
            goto done;
        }
        e = JS_GetException(ctx);
        r = JS_Call(ctx, arg, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, e);
            if (JS_HasUncatchableException(ctx)) {
                ret = JS_EXCEPTION;
                goto done;
            }
            e = JS_GetException(ctx);
        }
        if (dyn_settle(ctx, outer, e, 1) < 0)
            ret = JS_EXCEPTION;
        JS_FreeValue(ctx, e);
        goto done;
    }
    if (as_react_value(ctx, v, as_sem_run_done, data, 0) < 0 && as_settle_exc(ctx, outer) < 0)
        ret = JS_EXCEPTION;
done:
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, outer);
    return ret;
}

static JSValue as_sem_run(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsSem* s = as_get(ctx, this_val, AS_SEM);
    JSValue acq, st, outer, args;
    int i;
    if (!s)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        JS_ThrowTypeError(ctx, "Semaphore.run: fn must be a function");
        return as_rejected(ctx);
    }
    outer = JS_NewPromisePending(ctx);
    st = JS_NewArray(ctx);
    args = JS_NewArray(ctx);
    if (JS_IsException(outer) || JS_IsException(st) || JS_IsException(args))
        goto fail;
    for (i = 1; i < argc; i++)
        if (JS_SetPropertyUint32(ctx, args, (uint32_t)(i - 1), JS_DupValue(ctx, argv[i])) < 0)
            goto fail;
    if (JS_SetPropertyUint32(ctx, st, 0, JS_DupValue(ctx, argv[0])) < 0 || JS_SetPropertyUint32(ctx, st, 1, JS_DupValue(ctx, args)) < 0
        || JS_SetPropertyUint32(ctx, st, 2, JS_DupValue(ctx, outer)) < 0 || JS_SetPropertyUint32(ctx, st, 3, JS_UNDEFINED) < 0)
        goto fail;
    JS_FreeValue(ctx, args);
    acq = as_sem_acquire(ctx, this_val, 0, NULL);
    if (JS_IsException(acq)) {
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, outer);
        return acq;
    }
    if (as_react(ctx, acq, as_sem_run_go, st, 0) < 0) {
        JS_FreeValue(ctx, outer);
        outer = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, acq);
    return outer;
fail:
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, outer);
    return JS_EXCEPTION;
}

static int as_cap(JSContext* ctx, JSValueConst v, const char* who, double* cap)
{
    double d;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *cap = (double)INFINITY;
        return 0;
    }
    if (!as_number(ctx, v, &d) || (!isfinite(d) && d != (double)INFINITY)) {
        JS_ThrowTypeError(ctx, "%s: cap must be a positive number", who);
        return -1;
    }
    if (d != (double)INFINITY && (!as_is_i32(d) || d < 1)) {
        JS_ThrowRangeError(ctx, "%s: cap must be a positive integer", who);
        return -1;
    }
    *cap = d;
    return 0;
}

static JSValue as_queue_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    double cap;
    AsChan* q;
    JSValue obj;
    if (as_cap(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "Queue", &cap) < 0)
        return JS_EXCEPTION;
    obj = as_alloc_obj(ctx, new_target, AS_QUEUE, sizeof(AsChan), (void**)&q);
    if (!JS_IsException(obj)) {
        q->cap = cap;
        q->receiver = JS_NULL;
    }
    return obj;
}

static JSValue as_queue_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    AsChan* q = as_get(ctx, this_val, magic >> 1 ? AS_CHAN : AS_QUEUE);
    if (!q)
        return JS_EXCEPTION;
    return (magic & 1) ? JS_NewBool(ctx, q->closed) : JS_NewInt64(ctx, q->q.len);
}

static JSValue as_queue_push(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    AsChan* q = as_get(ctx, this_val, AS_QUEUE);
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!q)
        return JS_EXCEPTION;
    if (q->closed || (double)q->q.len >= q->cap) {
        if (magic)
            return JS_FALSE;
        if (q->closed)
            return JS_ThrowTypeError(ctx, "Queue: closed");
        return JS_ThrowRangeError(ctx, "Queue: full (cap %.0f)", q->cap);
    }
    if (as_ring_push(ctx, &q->q, JS_DupValue(ctx, v)) < 0)
        return JS_EXCEPTION;
    return magic ? JS_TRUE : JS_DupValue(ctx, this_val);
}

static JSValue as_queue_shift(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsChan* q = as_get(ctx, this_val, AS_QUEUE);
    (void)argc;
    (void)argv;
    if (!q)
        return JS_EXCEPTION;
    return as_ring_shift(&q->q);
}

static JSValue as_queue_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsChan* q = as_get(ctx, this_val, AS_QUEUE);
    (void)argc;
    (void)argv;
    if (!q)
        return JS_EXCEPTION;
    q->closed = 1;
    return JS_UNDEFINED;
}

static JSValue as_chan_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    double cap;
    AsChan* c;
    JSValue obj;
    if (as_cap(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "Channel", &cap) < 0)
        return JS_EXCEPTION;
    obj = as_alloc_obj(ctx, new_target, AS_CHAN, sizeof(AsChan), (void**)&c);
    if (!JS_IsException(obj)) {
        c->cap = cap;
        c->receiver = JS_NULL;
    }
    return obj;
}

static JSValue as_iter_result(JSContext* ctx, JSValueConst v, int done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if ((!done && JS_DefinePropertyValueStr(ctx, o, "value", JS_DupValue(ctx, v), JS_PROP_C_W_E) < 0)
        || JS_DefinePropertyValueStr(ctx, o, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static void as_chan_admit(JSContext* ctx, AsChan* c)
{
    while ((double)c->q.len < c->cap && c->blocked.len) {
        JSValue p = as_ring_shift(&c->blocked), v = as_ring_shift(&c->blocked);
        int rc = as_ring_push(ctx, &c->q, v) < 0 ? as_settle_exc(ctx, p) : dyn_settle(ctx, p, JS_UNDEFINED, 0);
        JS_FreeValue(ctx, p);
        if (rc < 0)
            return;
    }
}

static JSValue as_chan_send(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsChan* c = as_get(ctx, this_val, AS_CHAN);
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue p, res;
    if (!c)
        return JS_EXCEPTION;
    if (c->closed) {
        JS_ThrowTypeError(ctx, "Channel: send on a closed channel");
        return as_rejected(ctx);
    }
    if (!JS_IsNull(c->receiver)) {
        JSValue r = c->receiver;
        c->receiver = JS_NULL;
        int rc;
        res = as_iter_result(ctx, v, 0);
        rc = dyn_settle_value(ctx, r, res);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, r);
        if (rc < 0)
            return JS_EXCEPTION;
        return as_resolved(ctx, JS_UNDEFINED);
    }
    if ((double)c->q.len < c->cap) {
        if (as_ring_push(ctx, &c->q, JS_DupValue(ctx, v)) < 0)
            return JS_EXCEPTION;
        return as_resolved(ctx, JS_UNDEFINED);
    }
    p = JS_NewPromisePending(ctx);
    if (JS_IsException(p))
        return p;
    if (as_ring_push(ctx, &c->blocked, JS_DupValue(ctx, p)) < 0 || as_ring_push(ctx, &c->blocked, JS_DupValue(ctx, v)) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static JSValue as_chan_recv_on(JSContext* ctx, JSValueConst chan)
{
    AsChan* c = as_get(ctx, chan, AS_CHAN);
    JSValue v, res, p;
    if (!c)
        return JS_EXCEPTION;
    if (!JS_IsNull(c->receiver)) {
        JS_ThrowTypeError(ctx, "Channel: a recv() is already pending (single consumer)");
        return as_rejected(ctx);
    }
    if (c->q.len) {
        v = as_ring_shift(&c->q);
        as_chan_admit(ctx, c);
        res = as_iter_result(ctx, v, 0);
        JS_FreeValue(ctx, v);
        if (JS_IsException(res))
            return res;
        p = as_resolved(ctx, res);
        JS_FreeValue(ctx, res);
        return p;
    }
    if (c->closed) {
        c->iterating = 0;
        res = as_iter_result(ctx, JS_UNDEFINED, 1);
        p = as_resolved(ctx, res);
        JS_FreeValue(ctx, res);
        return p;
    }
    p = JS_NewPromisePending(ctx);
    if (!JS_IsException(p))
        c->receiver = JS_DupValue(ctx, p);
    return p;
}

static JSValue as_chan_recv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return as_chan_recv_on(ctx, this_val);
}

static JSValue as_chan_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsChan* c = as_get(ctx, this_val, AS_CHAN);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    c->closed = 1;
    c->iterating = 0;
    if (!JS_IsNull(c->receiver)) {
        JSValue r = c->receiver, res = as_iter_result(ctx, JS_UNDEFINED, 1);
        int rc;
        c->receiver = JS_NULL;
        rc = dyn_settle_value(ctx, r, res);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, r);
        if (rc < 0)
            return JS_EXCEPTION;
    }
    while (c->blocked.len) {
        JSValue p = as_ring_shift(&c->blocked), v = as_ring_shift(&c->blocked);
        int rc;
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Channel: closed while a send was waiting for room");
        rc = as_settle_exc(ctx, p);
        JS_FreeValue(ctx, p);
        if (rc < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue as_chan_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsChan* c = as_get(ctx, this_val, AS_CHAN);
    JSValue it;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (c->iterating)
        return JS_ThrowTypeError(ctx, "Channel: a for-await is already consuming this channel (single consumer)");
    c->iterating = 1;
    it = as_new(ctx, JS_UNDEFINED, AS_CHIT, JS_VALUE_GET_PTR(this_val));
    if (!JS_IsException(it))
        JS_DupValue(ctx, this_val);
    return it;
}

static JSValue as_chit_chan(JSValueConst it)
{
    return JS_MKPTR(JS_TAG_OBJECT, JS_GetOpaque(it, as_cid[AS_CHIT]));
}

static JSValue as_chit_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    AsChan* c;
    JSValue res, p;
    (void)argc;
    (void)argv;
    if (!as_get(ctx, this_val, AS_CHIT))
        return JS_EXCEPTION;
    if (!magic)
        return as_chan_recv_on(ctx, as_chit_chan(this_val));
    c = JS_GetOpaque(as_chit_chan(this_val), as_cid[AS_CHAN]);
    c->iterating = 0;
    res = as_iter_result(ctx, JS_UNDEFINED, 1);
    JS_DefinePropertyValueStr(ctx, res, "value", JS_UNDEFINED, JS_PROP_C_W_E);
    p = as_resolved(ctx, res);
    JS_FreeValue(ctx, res);
    return p;
}

static void as_pmap_launch(JSContext* ctx, JSValueConst st, AsPmap* m);

static JSValue as_pmap_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    AsPmap* m = JS_GetOpaque(data, as_cid[AS_PMAP]);
    m->inflight--;
    if (m->failed)
        return JS_UNDEFINED;
    if (is_reject) {
        m->failed = 1;
        return dyn_settle(ctx, m->outer, arg, 1) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    if (JS_SetPropertyUint32(ctx, m->results, (uint32_t)magic, JS_DupValue(ctx, arg)) < 0) {
        m->failed = 1;
        return as_settle_exc(ctx, m->outer) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    if (++m->settled == m->n) {
        if (dyn_settle(ctx, m->outer, m->results, 0) < 0)
            return JS_EXCEPTION;
    } else
        as_pmap_launch(ctx, data, m);
    return JS_UNDEFINED;
}

static JSValue as_pmap_call(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    AsPmap* m = JS_GetOpaque(data, as_cid[AS_PMAP]);
    JSValue a[3], v;
    (void)is_reject;
    (void)arg;
    a[0] = JS_GetPropertyUint32(ctx, m->items, (uint32_t)magic);
    a[1] = JS_NewInt32(ctx, magic);
    a[2] = m->items;
    v = JS_IsException(a[0]) ? JS_EXCEPTION : JS_Call(ctx, m->fn, JS_UNDEFINED, 3, (JSValueConst*)a);
    JS_FreeValue(ctx, a[0]);
    if (JS_IsException(v)) {
        JSValue e;
        if (JS_HasUncatchableException(ctx))
            return JS_EXCEPTION;
        e = JS_GetException(ctx);
        as_pmap_done(ctx, data, magic, 1, e);
        JS_FreeValue(ctx, e);
        return JS_UNDEFINED;
    }
    if (as_react_value(ctx, v, as_pmap_done, data, magic) < 0) {
        JSValue e;
        if (JS_HasUncatchableException(ctx))
            return JS_EXCEPTION;
        e = JS_GetException(ctx);
        as_pmap_done(ctx, data, magic, 1, e);
        JS_FreeValue(ctx, e);
    }
    return JS_UNDEFINED;
}

static void as_pmap_launch(JSContext* ctx, JSValueConst st, AsPmap* m)
{
    while (!m->failed && m->inflight < m->conc && m->next < m->n) {
        m->inflight++;
        if (as_react(ctx, m->resolved, as_pmap_call, st, (int)m->next) < 0) {
            m->failed = 1;
            as_settle_exc(ctx, m->outer);
            return;
        }
        m->next++;
    }
}

static JSValue as_pmap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst items = argc > 0 ? argv[0] : JS_UNDEFINED, fn = argc > 1 ? argv[1] : JS_UNDEFINED, opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    int32_t conc = 8;
    int64_t n;
    AsPmap* m;
    JSValue obj, r;
    (void)this_val;
    if (JS_IsArray(ctx, items) <= 0)
        return JS_ThrowTypeError(ctx, "pmap: items must be an array");
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "pmap: fn must be a function");
    if (!JS_IsNull(opts) && !JS_IsUndefined(opts)) {
        JSValue v;
        int k;
        if (!JS_IsObject(opts) || JS_IsFunction(ctx, opts))
            return JS_ThrowTypeError(ctx, "pmap: opts must be an object");
        if ((k = as_opt(ctx, opts, "concurrency", &v)) < 0)
            return JS_EXCEPTION;
        if (k) {
            int ok = as_pos_int(ctx, v, &conc);
            JS_FreeValue(ctx, v);
            if (!ok)
                return JS_ThrowTypeError(ctx, "pmap: concurrency must be a positive integer");
        }
    }
    {
        JSValue l = JS_GetPropertyStr(ctx, items, "length");
        if (JS_ToInt64(ctx, &n, l)) {
            JS_FreeValue(ctx, l);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, l);
    }
    obj = as_alloc_obj(ctx, JS_UNDEFINED, AS_PMAP, sizeof(AsPmap), (void**)&m);
    if (JS_IsException(obj))
        return obj;
    m->items = JS_DupValue(ctx, items);
    m->fn = JS_DupValue(ctx, fn);
    m->n = (uint32_t)n;
    m->conc = (uint32_t)conc;
    m->outer = JS_NewPromisePending(ctx);
    m->results = JS_NewArray(ctx);
    m->resolved = as_resolved(ctx, JS_UNDEFINED);
    if (JS_IsException(m->outer) || JS_IsException(m->results) || JS_IsException(m->resolved)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (n > 0)
        JS_SetPropertyStr(ctx, m->results, "length", JS_NewInt64(ctx, n));
    r = JS_DupValue(ctx, m->outer);
    if (!n) {
        if (dyn_settle(ctx, m->outer, m->results, 0) < 0) {
            JS_FreeValue(ctx, r);
            r = JS_EXCEPTION;
        }
    } else
        as_pmap_launch(ctx, obj, m);
    JS_FreeValue(ctx, obj);
    return r;
}

static JSValue as_pool_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int32_t n;
    AsPool* p;
    JSValue obj;
    if (!as_pos_int(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &n))
        return JS_ThrowTypeError(ctx, "Pool: n must be a positive integer");
    obj = as_alloc_obj(ctx, new_target, AS_POOL, sizeof(AsPool), (void**)&p);
    if (!JS_IsException(obj))
        p->n = n;
    return obj;
}

static JSValue as_pool_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    AsPool* p = as_get(ctx, this_val, AS_POOL);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, magic == 0 ? p->n : magic == 1 ? p->active : (int32_t)(p->q.len / 2));
}

static void as_pool_pump(JSContext* ctx, JSValueConst pool, AsPool* p);

static JSValue as_pool_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue pool = JS_GetPropertyUint32(ctx, data, 0), prom = JS_GetPropertyUint32(ctx, data, 2);
    AsPool* p = JS_GetOpaque(pool, as_cid[AS_POOL]);
    (void)magic;
    p->active--;
    if (dyn_settle(ctx, prom, arg, is_reject) < 0) {
        JS_FreeValue(ctx, prom);
        JS_FreeValue(ctx, pool);
        return JS_EXCEPTION;
    }
    as_pool_pump(ctx, pool, p);
    JS_FreeValue(ctx, prom);
    JS_FreeValue(ctx, pool);
    return JS_UNDEFINED;
}

static JSValue as_pool_call(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue fn = JS_GetPropertyUint32(ctx, data, 1), v;
    (void)magic;
    (void)is_reject;
    (void)arg;
    v = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(v) || as_react_value(ctx, v, as_pool_done, data, 0) < 0) {
        JSValue e;
        if (JS_HasUncatchableException(ctx))
            return JS_EXCEPTION;
        e = JS_GetException(ctx);
        v = as_pool_done(ctx, data, 0, 1, e);
        JS_FreeValue(ctx, e);
        return v;
    }
    return JS_UNDEFINED;
}

static void as_pool_pump(JSContext* ctx, JSValueConst pool, AsPool* p)
{
    while (p->active < p->n && p->q.len) {
        JSValue fn = as_ring_shift(&p->q), prom = as_ring_shift(&p->q), job = JS_NewArray(ctx), res = JS_UNDEFINED;
        p->active++;
        if (JS_IsException(job) || JS_SetPropertyUint32(ctx, job, 0, JS_DupValue(ctx, pool)) < 0
            || JS_SetPropertyUint32(ctx, job, 1, JS_DupValue(ctx, fn)) < 0 || JS_SetPropertyUint32(ctx, job, 2, JS_DupValue(ctx, prom)) < 0
            || JS_IsException(res = as_resolved(ctx, JS_UNDEFINED)) || as_react(ctx, res, as_pool_call, job, 0) < 0) {
            p->active--;
            as_settle_exc(ctx, prom);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, job);
        JS_FreeValue(ctx, prom);
    }
}

static JSValue as_pool_submit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    AsPool* p = as_get(ctx, this_val, AS_POOL);
    JSValue prom;
    if (!p)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Pool.submit: fn must be a function");
    prom = JS_NewPromisePending(ctx);
    if (JS_IsException(prom))
        return prom;
    if (as_ring_push(ctx, &p->q, JS_DupValue(ctx, argv[0])) < 0 || as_ring_push(ctx, &p->q, JS_DupValue(ctx, prom)) < 0) {
        JS_FreeValue(ctx, prom);
        return JS_EXCEPTION;
    }
    as_pool_pump(ctx, this_val, p);
    return prom;
}

static double as_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

static void as_timed_drop_args(JSRuntime* rt, AsTimed* t)
{
    uint32_t i;
    for (i = 0; i < t->ac; i++)
        JS_FreeValueRT(rt, t->av[i]);
    js_free_rt(rt, t->av);
    t->av = NULL;
    t->ac = 0;
    JS_FreeValueRT(rt, t->self);
    t->self = JS_UNDEFINED;
}

static JSValue as_timed_fire(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    AsTimed* t = JS_GetOpaque(data[0], as_cid[AS_TIMED]);
    JSValue self, r;
    JSValue* av;
    uint32_t n, i;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    JS_FreeValue(ctx, t->timer);
    t->timer = JS_NULL;
    if (t->throttle)
        t->last = as_now_ms();
    self = t->self;
    av = t->av;
    n = t->ac;
    t->self = JS_UNDEFINED;
    t->av = NULL;
    t->ac = 0;
    r = JS_Call(ctx, t->fn, self, (int)n, (JSValueConst*)av);
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, av[i]);
    js_free(ctx, av);
    JS_FreeValue(ctx, self);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

static JSValue as_timed_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    AsTimed* t = JS_GetOpaque2(ctx, this_val, as_cid[AS_TIMED]);
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    if (magic)
        return JS_NewBool(ctx, !JS_IsNull(t->timer));
    as_clear_timeout(ctx, &t->timer);
    as_timed_drop_args(JS_GetRuntime(ctx), t);
    return JS_UNDEFINED;
}

static JSValue as_timed_call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_val, int argc, JSValueConst* argv, int flags)
{
    AsTimed* t = JS_GetOpaque(func_obj, as_cid[AS_TIMED]);
    double wait;
    int i;
    (void)flags;
    if (!t)
        return JS_ThrowTypeError(ctx, "not a function");
    wait = t->ms;
    if (t->throttle) {
        double now = as_now_ms();
        wait = t->ms - (now - t->last);
        if (wait <= 0) {
            JSValue r;
            t->last = now;
            r = JS_Call(ctx, t->fn, this_val, argc, argv);
            if (JS_IsException(r))
                return r;
            JS_FreeValue(ctx, r);
            return JS_UNDEFINED;
        }
    }
    as_clear_timeout(ctx, &t->timer);
    as_timed_drop_args(JS_GetRuntime(ctx), t);
    if (argc > 0) {
        t->av = js_malloc(ctx, (size_t)argc * sizeof(*t->av));
        if (!t->av)
            return JS_EXCEPTION;
        for (i = 0; i < argc; i++)
            t->av[i] = JS_DupValue(ctx, argv[i]);
        t->ac = (uint32_t)argc;
    }
    t->self = JS_DupValue(ctx, this_val);
    if (JS_IsUndefined(t->fire)) {
        JSValue me = JS_DupValue(ctx, func_obj);
        t->fire = JS_NewCFunctionData(ctx, as_timed_fire, 0, 0, 1, (JSValueConst*)&me);
        JS_FreeValue(ctx, me);
        if (JS_IsException(t->fire)) {
            t->fire = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
    }
    t->timer = as_set_timeout(ctx, t->fire, wait);
    if (JS_IsException(t->timer)) {
        t->timer = JS_NULL;
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue as_timed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    const char* who = magic ? "throttle" : "debounce";
    double ms;
    AsTimed* t;
    JSValue st;
    (void)this_val;
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "%s: fn must be a function", who);
    if (!as_ms(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &ms))
        return JS_ThrowTypeError(ctx, "%s: ms must be a non-negative finite number", who);
    st = as_alloc_obj(ctx, JS_UNDEFINED, AS_TIMED, sizeof(AsTimed), (void**)&t);
    if (JS_IsException(st))
        return st;
    t->fn = JS_DupValue(ctx, fn);
    t->self = JS_UNDEFINED;
    t->timer = JS_NULL;
    t->fire = JS_UNDEFINED;
    t->ms = ms;
    t->last = -(double)INFINITY;
    t->throttle = (uint8_t)magic;
    return st;
}

static void as_fin(JSRuntime* rt, JSValue v)
{
    JSClassID id = JS_GetClassID(v);
    void* o = JS_GetOpaque(v, id);
    if (!o)
        return;
    if (id == as_cid[AS_SEM]) {
        as_ring_free(rt, &((AsSem*)o)->waiters);
    } else if (id == as_cid[AS_REL]) {
        JS_FreeValueRT(rt, ((AsRel*)o)->sem);
    } else if (id == as_cid[AS_QUEUE] || id == as_cid[AS_CHAN]) {
        AsChan* c = o;
        as_ring_free(rt, &c->q);
        as_ring_free(rt, &c->blocked);
        JS_FreeValueRT(rt, c->receiver);
    } else if (id == as_cid[AS_CHIT]) {
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, o));
        return;
    } else if (id == as_cid[AS_POOL]) {
        as_ring_free(rt, &((AsPool*)o)->q);
    } else if (id == as_cid[AS_PMAP]) {
        AsPmap* m = o;
        JS_FreeValueRT(rt, m->items);
        JS_FreeValueRT(rt, m->fn);
        JS_FreeValueRT(rt, m->results);
        JS_FreeValueRT(rt, m->outer);
        JS_FreeValueRT(rt, m->resolved);
    } else if (id == as_cid[AS_RETRY]) {
        AsRetry* r = o;
        JS_FreeValueRT(rt, r->fn);
        JS_FreeValueRT(rt, r->on_retry);
        JS_FreeValueRT(rt, r->outer);
        JS_FreeValueRT(rt, r->run);
    } else if (id == as_cid[AS_TIMED]) {
        AsTimed* t = o;
        JS_FreeValueRT(rt, t->fn);
        as_timed_drop_args(rt, t);
        JS_FreeValueRT(rt, t->timer);
        JS_FreeValueRT(rt, t->fire);
    }
    js_free_rt(rt, o);
}

static void as_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    JSClassID id = JS_GetClassID(v);
    void* o = JS_GetOpaque(v, id);
    if (!o)
        return;
    if (id == as_cid[AS_SEM]) {
        as_ring_mark(rt, &((AsSem*)o)->waiters, mf);
    } else if (id == as_cid[AS_REL]) {
        JS_MarkValue(rt, ((AsRel*)o)->sem, mf);
    } else if (id == as_cid[AS_QUEUE] || id == as_cid[AS_CHAN]) {
        AsChan* c = o;
        as_ring_mark(rt, &c->q, mf);
        as_ring_mark(rt, &c->blocked, mf);
        JS_MarkValue(rt, c->receiver, mf);
    } else if (id == as_cid[AS_CHIT]) {
        JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, o), mf);
    } else if (id == as_cid[AS_POOL]) {
        as_ring_mark(rt, &((AsPool*)o)->q, mf);
    } else if (id == as_cid[AS_PMAP]) {
        AsPmap* m = o;
        JS_MarkValue(rt, m->items, mf);
        JS_MarkValue(rt, m->fn, mf);
        JS_MarkValue(rt, m->results, mf);
        JS_MarkValue(rt, m->outer, mf);
        JS_MarkValue(rt, m->resolved, mf);
    } else if (id == as_cid[AS_RETRY]) {
        AsRetry* r = o;
        JS_MarkValue(rt, r->fn, mf);
        JS_MarkValue(rt, r->on_retry, mf);
        JS_MarkValue(rt, r->outer, mf);
        JS_MarkValue(rt, r->run, mf);
    } else if (id == as_cid[AS_TIMED]) {
        AsTimed* t = o;
        JS_MarkValue(rt, t->fn, mf);
        JS_MarkValue(rt, t->self, mf);
        {
            uint32_t i;
            for (i = 0; i < t->ac; i++)
                JS_MarkValue(rt, t->av[i], mf);
        }
        JS_MarkValue(rt, t->timer, mf);
        JS_MarkValue(rt, t->fire, mf);
    }
}

static const char* const as_class_names[AS_NCLASS] = { "Semaphore", "SemaphoreRelease", "Queue", "Channel", "Channel Iterator", "Pool", "pmap", "retry", "timed" };

static const JSCFunctionListEntry as_sem_funcs[] = {
    JS_CGETSET_MAGIC_DEF("available", as_sem_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("waiting", as_sem_get, NULL, 1),
    JS_CFUNC_DEF("acquire", 0, as_sem_acquire),
    JS_CFUNC_DEF("run", 1, as_sem_run),
};

static const JSCFunctionListEntry as_queue_funcs[] = {
    JS_CGETSET_MAGIC_DEF("length", as_queue_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("closed", as_queue_get, NULL, 1),
    JS_CFUNC_MAGIC_DEF("push", 1, as_queue_push, 0),
    JS_CFUNC_MAGIC_DEF("tryPush", 1, as_queue_push, 1),
    JS_CFUNC_DEF("shift", 0, as_queue_shift),
    JS_CFUNC_DEF("close", 0, as_queue_close),
};

static const JSCFunctionListEntry as_chan_funcs[] = {
    JS_CGETSET_MAGIC_DEF("length", as_queue_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("closed", as_queue_get, NULL, 3),
    JS_CFUNC_DEF("send", 1, as_chan_send),
    JS_CFUNC_DEF("recv", 0, as_chan_recv),
    JS_CFUNC_DEF("close", 0, as_chan_close),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, as_chan_iter),
};

static const JSCFunctionListEntry as_chit_funcs[] = {
    JS_CFUNC_MAGIC_DEF("next", 0, as_chit_method, 0),
    JS_CFUNC_MAGIC_DEF("return", 0, as_chit_method, 1),
};

static const JSCFunctionListEntry as_timed_funcs[] = {
    JS_CFUNC_MAGIC_DEF("cancel", 0, as_timed_method, 0),
    JS_CFUNC_MAGIC_DEF("pending", 0, as_timed_method, 1),
};

static const JSCFunctionListEntry as_pool_funcs[] = {
    JS_CGETSET_MAGIC_DEF("size", as_pool_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("active", as_pool_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("pending", as_pool_get, NULL, 2),
    JS_CFUNC_DEF("submit", 1, as_pool_submit),
};

static int as_register(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    static const struct {
        const JSCFunctionListEntry* f;
        int n;
    } protos[AS_NCLASS] = {
        { as_sem_funcs, countof(as_sem_funcs) },
        { NULL, 0 },
        { as_queue_funcs, countof(as_queue_funcs) },
        { as_chan_funcs, countof(as_chan_funcs) },
        { as_chit_funcs, countof(as_chit_funcs) },
        { as_pool_funcs, countof(as_pool_funcs) },
        { NULL, 0 },
        { NULL, 0 },
        { as_timed_funcs, countof(as_timed_funcs) },
    };
    int k;
    for (k = 0; k < AS_NCLASS; k++) {
        JSClassDef def;
        JSValue proto;
        memset(&def, 0, sizeof def);
        def.class_name = as_class_names[k];
        def.finalizer = as_fin;
        def.gc_mark = as_mark;
        if (k == AS_REL)
            def.call = as_rel_call;
        if (k == AS_TIMED)
            def.call = as_timed_call;
        JS_NewClassID(&as_cid[k]);
        if (!JS_IsRegisteredClass(rt, as_cid[k]) && JS_NewClass(rt, as_cid[k], &def) < 0)
            return -1;
        if (k == AS_REL || k == AS_TIMED) {
            JSValue f = JS_NewCFunction(ctx, as_sem_ctor, "", 0);
            if (JS_IsException(f))
                return -1;
            proto = JS_GetPrototype(ctx, f);
            JS_FreeValue(ctx, f);
            if (JS_IsException(proto))
                return -1;
            if (k == AS_TIMED) {
                JSValue fp = proto;
                proto = JS_NewObjectProto(ctx, fp);
                JS_FreeValue(ctx, fp);
                if (JS_IsException(proto))
                    return -1;
                if (JS_SetPropertyFunctionList(ctx, proto, protos[k].f, protos[k].n) < 0) {
                    JS_FreeValue(ctx, proto);
                    return -1;
                }
            }
        } else {
            proto = JS_NewObject(ctx);
            if (JS_IsException(proto))
                return -1;
            if (protos[k].f && JS_SetPropertyFunctionList(ctx, proto, protos[k].f, protos[k].n) < 0) {
                JS_FreeValue(ctx, proto);
                return -1;
            }
        }
        JS_SetClassProto(ctx, as_cid[k], proto);
    }
    return 0;
}

static int as_export_class(JSContext* ctx, JSModuleDef* m, int k, JSCFunction* ctor)
{
    JSValue c = JS_NewCFunction2(ctx, ctor, as_class_names[k], 1, JS_CFUNC_constructor, 0), proto;
    int rc;
    if (JS_IsException(c))
        return -1;
    proto = JS_GetClassProto(ctx, as_cid[k]);
    rc = JS_SetConstructor(ctx, c, proto);
    JS_FreeValue(ctx, proto);
    if (rc < 0) {
        JS_FreeValue(ctx, c);
        return -1;
    }
    return JS_SetModuleExport(ctx, m, as_class_names[k], c);
}

static int as_export_fn(JSContext* ctx, JSModuleDef* m, const char* name, JSCFunction* fn, int length)
{
    JSValue f = JS_NewCFunction(ctx, fn, name, length);
    if (JS_IsException(f))
        return -1;
    return JS_SetModuleExport(ctx, m, name, f);
}

static int as_export_magic(JSContext* ctx, JSModuleDef* m, const char* name, JSCFunctionMagic* fn, int length, int magic)
{
    JSValue f = JS_NewCFunctionMagic(ctx, fn, name, length, JS_CFUNC_generic_magic, magic);
    if (JS_IsException(f))
        return -1;
    return JS_SetModuleExport(ctx, m, name, f);
}

static int as_export_all(JSContext* ctx, JSModuleDef* m)
{
    if (as_register(ctx) < 0)
        return -1;
    if (as_export_fn(ctx, m, "sleep", as_sleep, 1) < 0 || as_export_fn(ctx, m, "withTimeout", as_with_timeout, 3) < 0
        || as_export_fn(ctx, m, "retry", as_retry, 2) < 0 || as_export_fn(ctx, m, "pmap", as_pmap, 3) < 0
        || as_export_magic(ctx, m, "debounce", as_timed, 2, 0) < 0 || as_export_magic(ctx, m, "throttle", as_timed, 2, 1) < 0
        || as_export_class(ctx, m, AS_SEM, as_sem_ctor) < 0 || as_export_class(ctx, m, AS_QUEUE, as_queue_ctor) < 0
        || as_export_class(ctx, m, AS_CHAN, as_chan_ctor) < 0 || as_export_class(ctx, m, AS_POOL, as_pool_ctor) < 0)
        return -1;
    return 0;
}

static JSClassID dyn_async_cfunc_class;

typedef struct {
    JSValue fn;
    int argc;
    JSValue argv[];
} DynOffloadJob;

static JSValue dyn_async_offload_entry(JSContext* ctx, void* arg)
{
    DynOffloadJob* j = arg;
    JSValue r;
    int i;
    JS_SwapStackFrame(JS_GetRuntime(ctx), NULL);
    r = JS_Call(ctx, j->fn, JS_UNDEFINED, j->argc, (JSValueConst*)j->argv);
    for (i = 0; i < j->argc; i++)
        JS_FreeValue(ctx, j->argv[i]);
    JS_FreeValue(ctx, j->fn);
    free(j);
    return r;
}

static JSValue dyn_async_offload(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynOffloadJob* j;
    JSValue p;
    int i, n;
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]) || JS_GetClassID(argv[0]) != dyn_async_cfunc_class)
        return JS_ThrowTypeError(ctx, "offload: the first argument must be a native dyna:* function");
    n = argc - 1;
    j = malloc(sizeof(*j) + (size_t)n * sizeof(JSValue));
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->fn = JS_DupValue(ctx, argv[0]);
    j->argc = n;
    for (i = 0; i < n; i++)
        j->argv[i] = JS_DupValue(ctx, argv[i + 1]);
    p = dyn_fiber_start(ctx, dyn_async_offload_entry, j);
    return p;
}

static int dyn_async_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue off;
    if (as_export_all(ctx, m) < 0)
        return -1;
    off = JS_NewCFunction(ctx, dyn_async_offload, "offload", 1);
    if (JS_IsException(off))
        return -1;
    dyn_async_cfunc_class = JS_GetClassID(off);
    return JS_SetModuleExport(ctx, m, "offload", off);
}

int js_nat_init_async(JSContext* ctx)
{
    static const char* const names[] = { "sleep", "withTimeout", "retry", "Semaphore", "Queue", "Channel", "pmap", "Pool", "debounce", "throttle", "offload" };
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:async", dyn_async_init_module);
    size_t i;
    if (!m)
        return -1;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (JS_AddModuleExport(ctx, m, names[i]) < 0)
            return -1;
    return 0;
}
#endif
