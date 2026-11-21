#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)
#include <sys/socket.h>

JSClassID ai_stream_class_id;
JSClassID ai_accum_class_id;
JSClassID ai_chat_stream_class_id;
JSClassID ai_chat_accum_class_id;

static JSClassID ai_ms_class_of(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);
    return id == ai_chat_stream_class_id && id ? ai_chat_stream_class_id : ai_stream_class_id;
}

static JSClassID ai_acc_class_of(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);
    return id == ai_chat_accum_class_id && id ? ai_chat_accum_class_id : ai_accum_class_id;
}

typedef struct {
    int refs;
    ai_req_t* r;
    ai_accum_t acc;
    int connected;
    int eof;
    int closed;
    int busy;
    int done;
    int failed;
    int rd_code;
    char why[256];
    JSRuntime* rt;
} ai_ms_t;

static void ai_ms_unref(ai_ms_t* s)
{
    if (!s || --s->refs > 0)
        return;
    ai_accum_free(s->rt, &s->acc);
    ai_req_free(s->r);
    free(s);
}

static void ai_ms_shutdown(ai_ms_t* s)
{
    if (s->r && s->r->st && s->r->st->conn.fd >= 0)
        shutdown(s->r->st->conn.fd, SHUT_RDWR);
}

static void ai_ms_dispose(void* native)
{
    ai_ms_t* s = (ai_ms_t*)native;
    if (!s)
        return;
    s->closed = 1;
    if (s->busy)
        ai_ms_shutdown(s);
    ai_ms_unref(s);
}

JSValue ai_stream_new(JSContext* ctx, ai_req_t* r)
{
    ai_ms_t* s = (ai_ms_t*)calloc(1, sizeof *s);
    JSValue obj;
    if (!s) {
        ai_req_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->refs = 1;
    s->r = r;
    s->rt = JS_GetRuntime(ctx);
    ai_accum_init_kind(&s->acc, r->cl->kind);
    obj = dyn_res_wrap(ctx, JS_UNDEFINED,
        r->cl->kind == AI_KIND_OPENAI ? ai_chat_stream_class_id : ai_stream_class_id, s,
        ai_ms_dispose);
    if (JS_IsException(obj))
        ai_ms_unref(s);
    return obj;
}

static void ai_ms_read(ai_ms_t* s)
{
    char buf[16384];
    long long n = hc_stream_some(s->r->st, (uint8_t*)buf, sizeof buf, s->why, sizeof s->why);
    if (n < 0) {
        s->rd_code = HC_CALL_E_RECV;
        return;
    }
    if (n == 0) {
        s->eof = 1;
        return;
    }
    ai_accum_feed(&s->acc, buf, (size_t)n);
}

enum {
    AI_MS_NEXT = 0,
    AI_MS_FINAL = 1,
    AI_MS_TEXT = 2
};

typedef struct {
    ai_ms_t* s;
    int mode;
    JSValue self;
} ai_ms_job_t;

static JSValue ai_ms_fail(JSContext* ctx, ai_ms_t* s, JSValue exc)
{
    s->failed = 1;
    s->done = 1;
    if (s->r)
        ai_req_drop_stream(s->r);
    (void)exc;
    return JS_EXCEPTION;
}

static JSValue ai_ms_step(JSContext* ctx, ai_ms_t* s)
{
    for (;;) {
        JSValue ev;
        if (s->closed) {
            s->done = 1;
            return JS_UNDEFINED;
        }
        ev = ai_accum_next(ctx, &s->acc);
        if (JS_IsException(ev))
            return ai_ms_fail(ctx, s, ev);
        if (!JS_IsUndefined(ev))
            return ev;
        if (s->acc.stopped) {
            s->done = 1;
            ai_req_drop_stream(s->r);
            return JS_UNDEFINED;
        }
        if (s->eof && ai_req_cancelled(s->r)) {
            ai_abort_throw(ctx);
            return ai_ms_fail(ctx, s, JS_EXCEPTION);
        }
        if (s->eof) {
            ai_error_throw(ctx, s->acc.kind == AI_KIND_OPENAI ? "stream ended before [DONE]" : "stream ended before message_stop", 0, "connection_error");
            return ai_ms_fail(ctx, s, JS_EXCEPTION);
        }
        dyn_fiber_offload();
        ai_ms_read(s);
        dyn_fiber_onjs();
        if (s->rd_code < 0) {
            char msg[384];
            if (s->closed) {
                s->done = 1;
                return JS_UNDEFINED;
            }
            if (ai_req_cancelled(s->r)) {
                ai_abort_throw(ctx);
                return ai_ms_fail(ctx, s, JS_EXCEPTION);
            }
            snprintf(msg, sizeof msg, "stream read failed: %s", s->why[0] ? s->why : "receive failed");
            ai_error_throw(ctx, msg, 0, "connection_error");
            return ai_ms_fail(ctx, s, JS_EXCEPTION);
        }
    }
}

static JSValue ai_ms_connect(JSContext* ctx, ai_ms_t* s)
{
    dyn_fiber_offload();
    ai_req_run(s->r);
    dyn_fiber_onjs();
    s->connected = 1;
    if (s->r->code < 0 || s->r->status < 200 || s->r->status >= 300) {
        JSValue e = ai_req_error(ctx, s->r);
        if (!JS_IsException(e))
            JS_Throw(ctx, e);
        return ai_ms_fail(ctx, s, JS_EXCEPTION);
    }
    return JS_UNDEFINED;
}

JSValue ai_iter_result(JSContext* ctx, JSValue v, int done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, v);
        return o;
    }
    JS_SetPropertyStr(ctx, o, "value", v);
    JS_SetPropertyStr(ctx, o, "done", JS_NewBool(ctx, done));
    return o;
}

static JSValue ai_final_text(JSContext* ctx, JSValueConst msg)
{
    JSValue c = JS_GetPropertyStr(ctx, msg, "content");
    int64_t n = 0, i;
    dyn_sb_t sb;
    JSValue out;
    int bad = 0;
    if (JS_IsException(c))
        return c;
    dyn_sb_init(&sb);
    if (JS_IsArray(ctx, c) && ai_arr_len(ctx, c, &n) == 0) {
        for (i = 0; i < n && !bad; i++) {
            JSValue b = JS_GetPropertyUint32(ctx, c, (uint32_t)i), t = JS_IsException(b) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, b, "type");
            const char* ts;
            if (JS_IsException(t))
                bad = 1;
            ts = JS_IsString(t) ? JS_ToCString(ctx, t) : NULL;
            if (ts && strcmp(ts, "text") == 0) {
                JSValue tx = JS_GetPropertyStr(ctx, b, "text");
                size_t l;
                const char* s;
                if (JS_IsException(tx))
                    bad = 1;
                s = JS_IsString(tx) ? JS_ToCStringLen(ctx, &l, tx) : NULL;
                if (s)
                    dyn_sb_put(&sb, s, l);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, tx);
            }
            JS_FreeCString(ctx, ts);
            JS_FreeValue(ctx, t);
            JS_FreeValue(ctx, b);
        }
    }
    JS_FreeValue(ctx, c);
    if (bad || sb.oom) {
        dyn_sb_free(&sb);
        return bad ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, sb.p ? sb.p : "", sb.n);
    dyn_sb_free(&sb);
    return out;
}

static JSValue ai_chat_content(JSContext* ctx, JSValueConst done)
{
    JSValue ch = JS_GetPropertyStr(ctx, done, "choices"), c0, m, out = JS_NULL;
    if (JS_IsException(ch))
        return ch;
    c0 = JS_IsArray(ctx, ch) ? JS_GetPropertyUint32(ctx, ch, 0) : JS_UNDEFINED;
    m = JS_IsException(c0) ? JS_EXCEPTION : JS_IsObject(c0) ? JS_GetPropertyStr(ctx, c0, "message") : JS_UNDEFINED;
    if (JS_IsException(m)) {
        out = JS_EXCEPTION;
    } else if (JS_IsObject(m)) {
        out = JS_GetPropertyStr(ctx, m, "content");
        if (!JS_IsException(out) && !JS_IsString(out)) {
            JS_FreeValue(ctx, out);
            out = JS_NULL;
        }
    }
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, c0);
    JS_FreeValue(ctx, ch);
    return out;
}

static JSValue ai_ms_entry(JSContext* ctx, void* arg)
{
    ai_ms_job_t* j = (ai_ms_job_t*)arg;
    ai_ms_t* s = j->s;
    int mode = j->mode;
    JSValue res = JS_UNDEFINED, self = j->self;
    free(j);
    if (!s->connected && !s->done && !s->closed) {
        res = ai_ms_connect(ctx, s);
        if (JS_IsException(res))
            goto out;
    }
    if (mode == AI_MS_NEXT) {
        if (s->done || s->closed) {
            res = ai_iter_result(ctx, JS_UNDEFINED, 1);
            goto out;
        }
        res = ai_ms_step(ctx, s);
        if (!JS_IsException(res))
            res = ai_iter_result(ctx, res, JS_IsUndefined(res));
        goto out;
    }
    while (!s->done && !s->closed) {
        res = ai_ms_step(ctx, s);
        if (JS_IsException(res))
            goto out;
        JS_FreeValue(ctx, res);
    }
    if (s->failed) {
        res = ai_error_throw(ctx, "the stream failed earlier", 0, "connection_error");
        goto out;
    }
    if (!s->acc.stopped) {
        res = ai_error_throw(ctx, s->acc.kind == AI_KIND_OPENAI ? "the stream was closed before [DONE]" : "the stream was closed before message_stop", 0, "connection_error");
        goto out;
    }
    res = ai_accum_snapshot(ctx, &s->acc);
    if (mode == AI_MS_TEXT && !JS_IsException(res)) {
        JSValue t = s->acc.kind == AI_KIND_OPENAI ? ai_chat_content(ctx, res) : ai_final_text(ctx, res);
        JS_FreeValue(ctx, res);
        res = t;
    }
out:
    s->busy = 0;
    if (s->done)
        ai_req_unlisten(ctx, s->r);
    ai_ms_unref(s);
    JS_FreeValue(ctx, self);
    return res;
}

static JSValue ai_ms_start(JSContext* ctx, JSValueConst this_val, int mode)
{
    ai_ms_t* s = (ai_ms_t*)dyn_res_native(ctx, this_val, ai_ms_class_of(this_val));
    ai_ms_job_t* j;
    JSValue p;
    if (!s)
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    if (s->busy)
        return hc_promise_rejected(ctx,
            ai_ms_class_of(this_val) == ai_chat_stream_class_id
                ? JS_ThrowTypeError(ctx, "ChatCompletionStream: one read at a time (await the pending next()/finalChatCompletion())")
                : JS_ThrowTypeError(ctx, "MessageStream: one read at a time (await the pending next()/finalMessage())"));
    j = (ai_ms_job_t*)malloc(sizeof *j);
    if (!j)
        return hc_promise_rejected(ctx, JS_ThrowOutOfMemory(ctx));
    j->s = s;
    j->mode = mode;
    j->self = JS_DupValue(ctx, this_val);
    s->refs++;
    s->busy = 1;
    p = dyn_fiber_start(ctx, ai_ms_entry, j);
    if (JS_IsException(p)) {
        s->busy = 0;
        s->refs--;
        JS_FreeValue(ctx, j->self);
        free(j);
    }
    return p;
}

static JSValue ai_ms_next(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ai_ms_start(ctx, this_val, AI_MS_NEXT);
}

static JSValue ai_ms_final(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ai_ms_start(ctx, this_val, AI_MS_FINAL);
}

static JSValue ai_ms_final_text(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ai_ms_start(ctx, this_val, AI_MS_TEXT);
}

static JSValue ai_ms_return(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    DynResource* r = dyn_res_get(ctx, this_val, ai_ms_class_of(this_val));
    (void)argc;
    (void)argv;
    if (!r)
        JS_FreeValue(ctx, JS_GetException(ctx));
    else if (r->native && !r->closed) {
        ai_ms_t* s = (ai_ms_t*)r->native;
        s->closed = 1;
        if (s->busy)
            ai_ms_shutdown(s);
        else if (s->r)
            ai_req_drop_stream(s->r);
    }
    return hc_promise_resolved(ctx, ai_iter_result(ctx, JS_UNDEFINED, 1));
}

static JSValue ai_ms_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static ai_ms_t* ai_ms_peek(JSContext* ctx, JSValueConst this_val)
{
    DynResource* r = (DynResource*)JS_GetOpaque(this_val, ai_ms_class_of(this_val));
    (void)ctx;
    return r && !r->closed ? (ai_ms_t*)r->native : NULL;
}

static JSValue ai_ms_get_message(JSContext* ctx, JSValueConst this_val)
{
    ai_ms_t* s = ai_ms_peek(ctx, this_val);
    if (!s)
        return JS_NULL;
    return ai_accum_snapshot(ctx, &s->acc);
}

static JSValue ai_ms_get_status(JSContext* ctx, JSValueConst this_val)
{
    ai_ms_t* s = ai_ms_peek(ctx, this_val);
    if (!s || !s->connected || !s->r || s->r->code < 0)
        return JS_NULL;
    return JS_NewInt32(ctx, s->r->status);
}

static JSValue ai_ms_get_request_id(JSContext* ctx, JSValueConst this_val)
{
    ai_ms_t* s = ai_ms_peek(ctx, this_val);
    size_t l = 0;
    const char* v;
    if (!s || !s->connected || !s->r || s->r->code < 0 || !s->r->hd.raw)
        return JS_NULL;
    v = hc_head_get(&s->r->hd, "request-id", &l);
    if (!v)
        v = hc_head_get(&s->r->hd, "x-request-id", &l);
    return v ? JS_NewStringLen(ctx, v, l) : JS_NULL;
}

static const JSCFunctionListEntry ai_stream_proto[] = {
    JS_CFUNC_DEF("next", 0, ai_ms_next),
    JS_CFUNC_DEF("return", 0, ai_ms_return),
    JS_CFUNC_DEF("finalMessage", 0, ai_ms_final),
    JS_CFUNC_DEF("finalText", 0, ai_ms_final_text),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, ai_ms_self),
    JS_CGETSET_DEF("message", ai_ms_get_message, NULL),
    JS_CGETSET_DEF("status", ai_ms_get_status, NULL),
    JS_CGETSET_DEF("requestId", ai_ms_get_request_id, NULL),
};

static const JSCFunctionListEntry ai_chat_stream_proto[] = {
    JS_CFUNC_DEF("next", 0, ai_ms_next),
    JS_CFUNC_DEF("return", 0, ai_ms_return),
    JS_CFUNC_DEF("finalChatCompletion", 0, ai_ms_final),
    JS_CFUNC_DEF("finalContent", 0, ai_ms_final_text),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, ai_ms_self),
    JS_CGETSET_DEF("completion", ai_ms_get_message, NULL),
    JS_CGETSET_DEF("status", ai_ms_get_status, NULL),
    JS_CGETSET_DEF("requestId", ai_ms_get_request_id, NULL),
};

static JSValue ai_ms_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "MessageStream: use client.messages.stream(body)");
}

static JSValue ai_chat_ms_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "ChatCompletionStream: use client.chat.completions.stream(body)");
}

static void ai_ms_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, ai_ms_class_of(val));
    if (r && r->native)
        JS_MarkValue(rt, ((ai_ms_t*)r->native)->acc.msg, mark);
}

static JSClassDef ai_stream_class = { "MessageStream", dyn_res_finalizer, ai_ms_mark, NULL, NULL };
static JSClassDef ai_chat_stream_class = { "ChatCompletionStream", dyn_res_finalizer, ai_ms_mark, NULL, NULL };

typedef struct {
    ai_accum_t acc;
    JSRuntime* rt;
} ai_acc_t;

static void ai_acc_finalizer(JSRuntime* rt, JSValue val)
{
    ai_acc_t* a = (ai_acc_t*)JS_GetOpaque(val, ai_acc_class_of(val));
    if (!a)
        return;
    ai_accum_free(rt, &a->acc);
    free(a);
}

static void ai_acc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    ai_acc_t* a = (ai_acc_t*)JS_GetOpaque(val, ai_acc_class_of(val));
    if (a)
        JS_MarkValue(rt, a->acc.msg, mark);
}

static JSClassDef ai_acc_class = { "MessageAccumulator", ai_acc_finalizer, ai_acc_mark, NULL, NULL };
static JSClassDef ai_chat_acc_class = { "ChatCompletionAccumulator", ai_acc_finalizer, ai_acc_mark, NULL, NULL };

static void ai_acc_dispose(void* native)
{
    ai_acc_t* a = (ai_acc_t*)native;
    ai_accum_free(a->rt, &a->acc);
    free(a);
}

static JSValue ai_acc_new(JSContext* ctx, JSValueConst new_target, int kind)
{
    ai_acc_t* a;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "%s: use new", kind == AI_KIND_OPENAI ? "ChatCompletionAccumulator" : "MessageAccumulator");
    a = (ai_acc_t*)calloc(1, sizeof *a);
    if (!a)
        return JS_ThrowOutOfMemory(ctx);
    ai_accum_init_kind(&a->acc, kind);
    a->rt = JS_GetRuntime(ctx);
    return dyn_plain_wrap(ctx, new_target,
        kind == AI_KIND_OPENAI ? ai_chat_accum_class_id : ai_accum_class_id, a, ai_acc_dispose);
}

static JSValue ai_acc_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ai_acc_new(ctx, new_target, AI_KIND_ANTHROPIC);
}

static JSValue ai_chat_acc_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ai_acc_new(ctx, new_target, AI_KIND_OPENAI);
}

static JSValue ai_acc_feed(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_acc_t* a = (ai_acc_t*)JS_GetOpaque2(ctx, this_val, ai_acc_class_of(this_val));
    const char* p;
    size_t n = 0;
    int is_str;
    JSValue arr;
    uint32_t k = 0;
    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s.feed(chunk: string | Uint8Array)",
            a->acc.kind == AI_KIND_OPENAI ? "ChatCompletionAccumulator" : "MessageAccumulator");
    is_str = JS_IsString(argv[0]);
    if (is_str)
        p = JS_ToCStringLen(ctx, &n, argv[0]);
    else
        p = (const char*)hc_view_bytes(ctx, argv[0], &n,
            a->acc.kind == AI_KIND_OPENAI ? "ChatCompletionAccumulator.feed" : "MessageAccumulator.feed");
    if (!p)
        return JS_EXCEPTION;
    ai_accum_feed(&a->acc, p, n);
    if (is_str)
        JS_FreeCString(ctx, p);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (;;) {
        JSValue ev = ai_accum_next(ctx, &a->acc);
        if (JS_IsException(ev)) {
            JS_FreeValue(ctx, arr);
            return ev;
        }
        if (JS_IsUndefined(ev))
            break;
        JS_SetPropertyUint32(ctx, arr, k++, ev);
    }
    return arr;
}

static JSValue ai_acc_get_message(JSContext* ctx, JSValueConst this_val)
{
    ai_acc_t* a = (ai_acc_t*)JS_GetOpaque2(ctx, this_val, ai_acc_class_of(this_val));
    if (!a)
        return JS_EXCEPTION;
    return ai_accum_snapshot(ctx, &a->acc);
}

static JSValue ai_acc_get_done(JSContext* ctx, JSValueConst this_val)
{
    ai_acc_t* a = (ai_acc_t*)JS_GetOpaque2(ctx, this_val, ai_acc_class_of(this_val));
    if (!a)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, a->acc.stopped);
}

static const JSCFunctionListEntry ai_acc_proto[] = {
    JS_CFUNC_DEF("feed", 1, ai_acc_feed),
    JS_CGETSET_DEF("message", ai_acc_get_message, NULL),
    JS_CGETSET_DEF("done", ai_acc_get_done, NULL),
};

static const JSCFunctionListEntry ai_chat_acc_proto[] = {
    JS_CFUNC_DEF("feed", 1, ai_acc_feed),
    JS_CGETSET_DEF("completion", ai_acc_get_message, NULL),
    JS_CGETSET_DEF("done", ai_acc_get_done, NULL),
};

int ai_stream_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &ai_stream_class_id, &ai_stream_class,
            ai_stream_proto, (int)countof(ai_stream_proto),
            (JSCFunction*)ai_ms_ctor, "MessageStream")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &ai_chat_stream_class_id, &ai_chat_stream_class,
            ai_chat_stream_proto, (int)countof(ai_chat_stream_proto),
            (JSCFunction*)ai_chat_ms_ctor, "ChatCompletionStream")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &ai_accum_class_id, &ai_acc_class,
            ai_acc_proto, (int)countof(ai_acc_proto),
            (JSCFunction*)ai_acc_ctor, "MessageAccumulator")
        < 0)
        return -1;
    return dyn_register_plain_class(ctx, m, &ai_chat_accum_class_id, &ai_chat_acc_class,
        ai_chat_acc_proto, (int)countof(ai_chat_acc_proto),
        (JSCFunction*)ai_chat_acc_ctor, "ChatCompletionAccumulator");
}
#endif
