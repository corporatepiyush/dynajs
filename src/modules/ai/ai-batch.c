#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)
#include <sys/socket.h>

JSClassID ai_results_class_id;

static JSValue ai_reject(JSContext* ctx)
{
    return hc_promise_rejected(ctx, JS_EXCEPTION);
}

static int ai_batch_check(JSContext* ctx, JSValueConst body)
{
    JSValue reqs;
    int64_t n = 0, i;
    if (!JS_IsObject(body) || JS_IsArray(ctx, body) || JS_IsFunction(ctx, body)) {
        JS_ThrowTypeError(ctx, "messages.batches.create({ requests })");
        return -1;
    }
    reqs = JS_GetPropertyStr(ctx, body, "requests");
    if (JS_IsException(reqs))
        return -1;
    if (!JS_IsArray(ctx, reqs) || ai_arr_len(ctx, reqs, &n) < 0) {
        JS_FreeValue(ctx, reqs);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "messages.batches.create: requests must be an array");
        return -1;
    }
    if (n < 1 || n > AI_BATCH_MAX_REQUESTS) {
        JS_FreeValue(ctx, reqs);
        JS_ThrowRangeError(ctx, "messages.batches.create: requests must hold 1..%d entries (got %lld)",
            AI_BATCH_MAX_REQUESTS, (long long)n);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, reqs, (uint32_t)i), id, pr;
        int ok;
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, reqs);
            return -1;
        }
        ok = JS_IsObject(e) && !JS_IsArray(ctx, e);
        id = ok ? JS_GetPropertyStr(ctx, e, "custom_id") : JS_UNDEFINED;
        pr = ok && !JS_IsException(id) ? JS_GetPropertyStr(ctx, e, "params") : JS_UNDEFINED;
        if (JS_IsException(id) || JS_IsException(pr)) {
            ok = 0;
        } else if (ok && JS_IsString(id)) {
            size_t l = 0;
            const char* s = JS_ToCStringLen(ctx, &l, id);
            ok = s && l > 0;
            JS_FreeCString(ctx, s);
        } else {
            ok = 0;
        }
        ok = ok && JS_IsObject(pr) && !JS_IsArray(ctx, pr) && !JS_IsFunction(ctx, pr);
        JS_FreeValue(ctx, id);
        JS_FreeValue(ctx, pr);
        JS_FreeValue(ctx, e);
        if (!ok) {
            JS_FreeValue(ctx, reqs);
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "messages.batches.create: requests[%lld] needs a non-empty string custom_id and an object params",
                    (long long)i);
            return -1;
        }
    }
    JS_FreeValue(ctx, reqs);
    return 0;
}

static JSValue ai_batch_create(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    ai_req_t* r;
    if (!cl || ai_batch_check(ctx, body) < 0)
        return ai_reject(ctx);
    r = ai_req_new(ctx, cl, "POST", "/v1/messages/batches", JS_UNDEFINED, body,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static ai_req_t* ai_batch_id_req(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, const char* who, const char* method,
    const char* post, int out)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    dyn_sb_t path;
    ai_req_t* r;
    if (!cl)
        return NULL;
    dyn_sb_init(&path);
    if (ai_id_path(ctx, who, "/v1/messages/batches/", argc > 0 ? argv[0] : JS_UNDEFINED,
            post, &path)
        < 0) {
        dyn_sb_free(&path);
        return NULL;
    }
    r = ai_req_new(ctx, cl, method, path.p, JS_UNDEFINED, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, out);
    dyn_sb_free(&path);
    return r;
}

static JSValue ai_batch_simple(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, const char* who, const char* method, const char* post)
{
    ai_req_t* r = ai_batch_id_req(ctx, this_val, argc, argv, who, method, post, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_batch_retrieve(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_batch_simple(ctx, this_val, argc, argv, "messages.batches.retrieve", "GET", NULL);
}

static JSValue ai_batch_cancel(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_batch_simple(ctx, this_val, argc, argv, "messages.batches.cancel", "POST", "/cancel");
}

static JSValue ai_batch_delete(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_batch_simple(ctx, this_val, argc, argv, "messages.batches.delete", "DELETE", NULL);
}

static JSValue ai_batch_list(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    ai_req_t* r;
    if (!cl)
        return ai_reject(ctx);
    r = ai_req_new(ctx, cl, "GET", "/v1/messages/batches", argc > 0 ? argv[0] : JS_UNDEFINED,
        JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

typedef struct {
    int refs;
    ai_req_t* r;
    dyn_sb_t buf;
    size_t pos;
    size_t scan;
    int connected;
    int eof;
    int closed;
    int busy;
    int done;
    int rd_code;
    char why[256];
} ai_br_t;

static void ai_br_unref(ai_br_t* s)
{
    if (!s || --s->refs > 0)
        return;
    dyn_sb_free(&s->buf);
    ai_req_free(s->r);
    free(s);
}

static void ai_br_shutdown(ai_br_t* s)
{
    if (s->r && s->r->st && s->r->st->conn.fd >= 0)
        shutdown(s->r->st->conn.fd, SHUT_RDWR);
}

static void ai_br_drop(ai_br_t* s)
{
    s->done = 1;
    if (s->r)
        ai_req_drop_stream(s->r);
}

static void ai_br_dispose(void* native)
{
    ai_br_t* s = (ai_br_t*)native;
    if (!s)
        return;
    s->closed = 1;
    if (s->busy)
        ai_br_shutdown(s);
    ai_br_unref(s);
}

static JSValue ai_results_new(JSContext* ctx, ai_req_t* r)
{
    ai_br_t* s = (ai_br_t*)calloc(1, sizeof *s);
    JSValue obj;
    if (!s) {
        ai_req_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->refs = 1;
    s->r = r;
    dyn_sb_init(&s->buf);
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, ai_results_class_id, s, ai_br_dispose);
    if (JS_IsException(obj))
        ai_br_unref(s);
    return obj;
}

static JSValue ai_batch_results(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_req_t* r = ai_batch_id_req(ctx, this_val, argc, argv, "messages.batches.results",
        "GET", "/results", AI_OUT_JSONL);
    if (!r)
        return JS_EXCEPTION;
    return ai_results_new(ctx, r);
}

const JSCFunctionListEntry ai_batches_fns[] = {
    JS_CFUNC_DEF("create", 2, ai_batch_create),
    JS_CFUNC_DEF("retrieve", 2, ai_batch_retrieve),
    JS_CFUNC_DEF("list", 2, ai_batch_list),
    JS_CFUNC_DEF("cancel", 2, ai_batch_cancel),
    JS_CFUNC_DEF("delete", 2, ai_batch_delete),
    JS_CFUNC_DEF("results", 2, ai_batch_results),
};
const int ai_batches_nfns = (int)countof(ai_batches_fns);

static void ai_br_read(ai_br_t* s)
{
    char chunk[16384];
    long long n = hc_stream_some(s->r->st, (uint8_t*)chunk, sizeof chunk, s->why, sizeof s->why);
    if (n < 0) {
        s->rd_code = HC_CALL_E_RECV;
        return;
    }
    if (n == 0) {
        s->eof = 1;
        return;
    }
    if (s->pos) {
        memmove(s->buf.p, s->buf.p + s->pos, s->buf.n - s->pos);
        s->buf.n -= s->pos;
        s->scan -= s->pos;
        s->pos = 0;
    }
    dyn_sb_put(&s->buf, chunk, (size_t)n);
}

static JSValue ai_br_fail(JSContext* ctx, ai_br_t* s, const char* type,
    const char* msg)
{
    ai_br_drop(s);
    return ai_error_throw(ctx, msg, 0, type);
}

static JSValue ai_br_parse(JSContext* ctx, ai_br_t* s, size_t a, size_t b)
{
    JSValue v;
    char save;
    if (b > a && s->buf.p[b - 1] == '\r')
        b--;
    save = s->buf.p[b];
    s->buf.p[b] = 0;
    v = JS_ParseJSON(ctx, s->buf.p + a, b - a, "<batch results>");
    s->buf.p[b] = save;
    if (JS_IsException(v))
        return dyn_take_syntax_error(ctx) ? JS_UNINITIALIZED : JS_EXCEPTION;
    if (!JS_IsObject(v) || JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        return JS_UNINITIALIZED;
    }
    return v;
}

static int ai_br_blank(const char* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r')
            return 0;
    return 1;
}

static JSValue ai_br_step(JSContext* ctx, ai_br_t* s)
{
    for (;;) {
        char* nl;
        if (s->closed) {
            s->done = 1;
            return JS_UNDEFINED;
        }
        if (s->buf.oom)
            return ai_br_fail(ctx, s, "stream_error", "out of memory buffering a result line");
        nl = s->scan < s->buf.n ? (char*)memchr(s->buf.p + s->scan, '\n', s->buf.n - s->scan) : NULL;
        if (nl) {
            size_t a = s->pos, b = (size_t)(nl - s->buf.p);
            JSValue v;
            s->pos = s->scan = b + 1;
            if (ai_br_blank(s->buf.p + a, b - a))
                continue;
            if (b - a > AI_MAX_EVENT)
                return ai_br_fail(ctx, s, "stream_error", "a result line exceeds the 64 MiB cap");
            v = ai_br_parse(ctx, s, a, b);
            if (JS_IsUninitialized(v))
                return ai_br_fail(ctx, s, "stream_error", "a result line is not a JSON object");
            return v;
        }
        s->scan = s->buf.n;
        if (s->buf.n - s->pos > AI_MAX_EVENT)
            return ai_br_fail(ctx, s, "stream_error", "a result line exceeds the 64 MiB cap");
        if (s->eof && ai_req_cancelled(s->r)) {
            ai_br_drop(s);
            return ai_abort_throw(ctx);
        }
        if (s->eof) {
            size_t a = s->pos, b = s->buf.n;
            JSValue v;
            s->pos = s->scan = b;
            if (ai_br_blank(s->buf.p + a, b - a)) {
                ai_br_drop(s);
                return JS_UNDEFINED;
            }
            dyn_sb_putc(&s->buf, 0);
            if (s->buf.oom)
                return ai_br_fail(ctx, s, "stream_error", "out of memory buffering a result line");
            s->buf.n--;
            v = ai_br_parse(ctx, s, a, b);
            if (JS_IsUninitialized(v))
                return ai_br_fail(ctx, s, "connection_error", "the results stream ended inside a line");
            return v;
        }
        dyn_fiber_offload();
        ai_br_read(s);
        dyn_fiber_onjs();
        if (s->rd_code < 0) {
            char msg[384];
            if (s->closed) {
                s->done = 1;
                return JS_UNDEFINED;
            }
            if (ai_req_cancelled(s->r)) {
                ai_br_drop(s);
                return ai_abort_throw(ctx);
            }
            snprintf(msg, sizeof msg, "results read failed: %s", s->why[0] ? s->why : "receive failed");
            return ai_br_fail(ctx, s, "connection_error", msg);
        }
    }
}

typedef struct {
    ai_br_t* s;
    JSValue self;
} ai_br_job_t;

static JSValue ai_br_entry(JSContext* ctx, void* arg)
{
    ai_br_job_t* j = (ai_br_job_t*)arg;
    ai_br_t* s = j->s;
    JSValue res, self = j->self;
    free(j);
    if (!s->connected && !s->done && !s->closed) {
        dyn_fiber_offload();
        ai_req_run(s->r);
        dyn_fiber_onjs();
        s->connected = 1;
        if (s->r->code < 0 || s->r->status < 200 || s->r->status >= 300) {
            JSValue e = ai_req_error(ctx, s->r);
            ai_br_drop(s);
            res = JS_IsException(e) ? e : JS_Throw(ctx, e);
            goto out;
        }
    }
    if (s->done || s->closed) {
        res = ai_iter_result(ctx, JS_UNDEFINED, 1);
        goto out;
    }
    res = ai_br_step(ctx, s);
    if (!JS_IsException(res))
        res = ai_iter_result(ctx, res, JS_IsUndefined(res));
out:
    s->busy = 0;
    if (s->done)
        ai_req_unlisten(ctx, s->r);
    ai_br_unref(s);
    JS_FreeValue(ctx, self);
    return res;
}

static JSValue ai_br_next(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_br_t* s = (ai_br_t*)dyn_res_native(ctx, this_val, ai_results_class_id);
    ai_br_job_t* j;
    JSValue p;
    (void)argc;
    (void)argv;
    if (!s)
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    if (s->busy)
        return hc_promise_rejected(ctx,
            JS_ThrowTypeError(ctx, "BatchResults: one read at a time (await the pending next())"));
    j = (ai_br_job_t*)malloc(sizeof *j);
    if (!j)
        return hc_promise_rejected(ctx, JS_ThrowOutOfMemory(ctx));
    j->s = s;
    j->self = JS_DupValue(ctx, this_val);
    s->refs++;
    s->busy = 1;
    p = dyn_fiber_start(ctx, ai_br_entry, j);
    if (JS_IsException(p)) {
        s->busy = 0;
        s->refs--;
        JS_FreeValue(ctx, j->self);
        free(j);
    }
    return p;
}

static JSValue ai_br_return(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    DynResource* r = dyn_res_get(ctx, this_val, ai_results_class_id);
    (void)argc;
    (void)argv;
    if (!r)
        JS_FreeValue(ctx, JS_GetException(ctx));
    else if (r->native && !r->closed) {
        ai_br_t* s = (ai_br_t*)r->native;
        s->closed = 1;
        if (s->busy)
            ai_br_shutdown(s);
        else
            ai_br_drop(s);
    }
    return hc_promise_resolved(ctx, ai_iter_result(ctx, JS_UNDEFINED, 1));
}

static JSValue ai_br_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static ai_br_t* ai_br_peek(JSValueConst this_val)
{
    DynResource* r = (DynResource*)JS_GetOpaque(this_val, ai_results_class_id);
    return r && !r->closed ? (ai_br_t*)r->native : NULL;
}

static JSValue ai_br_get_status(JSContext* ctx, JSValueConst this_val)
{
    ai_br_t* s = ai_br_peek(this_val);
    if (!s || !s->connected || !s->r || s->r->code < 0)
        return JS_NULL;
    return JS_NewInt32(ctx, s->r->status);
}

static JSValue ai_br_get_request_id(JSContext* ctx, JSValueConst this_val)
{
    ai_br_t* s = ai_br_peek(this_val);
    size_t l = 0;
    const char* v;
    if (!s || !s->connected || !s->r || s->r->code < 0 || !s->r->hd.raw)
        return JS_NULL;
    v = hc_head_get(&s->r->hd, "request-id", &l);
    return v ? JS_NewStringLen(ctx, v, l) : JS_NULL;
}

static const JSCFunctionListEntry ai_results_proto[] = {
    JS_CFUNC_DEF("next", 0, ai_br_next),
    JS_CFUNC_DEF("return", 0, ai_br_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, ai_br_self),
    JS_CGETSET_DEF("status", ai_br_get_status, NULL),
    JS_CGETSET_DEF("requestId", ai_br_get_request_id, NULL),
};

static JSValue ai_br_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "BatchResults: use client.messages.batches.results(id)");
}

static JSClassDef ai_results_class = { "BatchResults", dyn_res_finalizer, NULL, NULL, NULL };

int ai_results_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &ai_results_class_id, &ai_results_class,
        ai_results_proto, (int)countof(ai_results_proto),
        (JSCFunction*)ai_br_ctor, "BatchResults");
}
#endif
