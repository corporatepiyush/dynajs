#include "stream-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

static void pipe_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void pipe_state_free(JSContext* ctx, pipe_state_t* ps)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (ps->cps.dead)
        return;
    dyn_cps_arm_kill(ctx, rt, &ps->cps);
    JS_RemoveShutdownSweep(rt, pipe_op_sweep, ps);
    JS_ShutdownUndeferFree(rt, ps);
    JS_FreeValue(ctx, ps->src);
    JS_FreeValue(ctx, ps->dst);
    JS_FreeValue(ctx, ps->read_fn);
    JS_FreeValue(ctx, ps->write_fn);
    JS_FreeValue(ctx, ps->on_chunk);
    JS_FreeValue(ctx, ps->chunk);
    JS_FreeValue(ctx, ps->jresolve);
    JS_FreeValue(ctx, ps->jreject);
    JS_FreeValue(ctx, ps->exc);
    free(ps);
}

static void pipe_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    pipe_state_t* ps = (pipe_state_t*)opaque;
    stream_park_fail(ctx, rt, &ps->jresolve, &ps->jreject,
        "stream.pipe: aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &ps->cps, &ps->src, 9);
}

static void pipe_finish(JSContext* ctx, pipe_state_t* ps, int failed)
{
    JSValue r;

    if (ps->cps.dead)
        return;
    if (ps->terminal) {
        pipe_state_free(ctx, ps);
        return;
    }
    ps->terminal = 1;
    if (failed) {
        r = JS_IsException(ps->exc) ? JS_EXCEPTION : JS_Call(ctx, ps->jreject, JS_UNDEFINED, 1, (JSValueConst*)&ps->exc);
    } else {
        JSValue total = JS_NewInt64(ctx, (int64_t)ps->total);
        r = JS_Call(ctx, ps->jresolve, JS_UNDEFINED, 1, &total);
        JS_FreeValue(ctx, total);
    }
    if (JS_IsException(r))
        stream_drop_exc(ctx);
    else
        JS_FreeValue(ctx, r);
    pipe_state_free(ctx, ps);
}

static void pipe_fail(JSContext* ctx, pipe_state_t* ps, JSValue exc)
{
    JS_FreeValue(ctx, ps->exc);
    ps->exc = exc;
    pipe_finish(ctx, ps, 1);
}

static JSValue pipe_chunk_view(JSContext* ctx, pipe_state_t* ps, size_t base,
    size_t len)
{
    JSValue ab;
    size_t boff = 0, blen = 0, bpe = 0;
    JSValue av[3];
    JSValueConst a[3];
    JSValue sub;

    ab = JS_GetTypedArrayBuffer(ctx, ps->chunk, &boff, &blen, &bpe);
    if (JS_IsException(ab))
        return ab;
    av[0] = ab;
    av[1] = JS_NewInt64(ctx, (int64_t)(boff + base));
    av[2] = JS_NewInt64(ctx, (int64_t)len);
    a[0] = av[0];
    a[1] = av[1];
    a[2] = av[2];
    sub = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, av[1]);
    JS_FreeValue(ctx, av[2]);
    JS_FreeValue(ctx, ab);
    return sub;
}

static void pipe_step_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);

static void pipe_step_write_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);

static void pipe_issue_write(JSContext* ctx, pipe_state_t* ps)
{
    JSValue view, p;

    view = pipe_chunk_view(ctx, ps, ps->chunk_written,
        ps->chunk_read - ps->chunk_written);
    if (JS_IsException(view)) {
        pipe_fail(ctx, ps, stream_take_exc(ctx));
        return;
    }
    p = JS_Call(ctx, ps->write_fn, ps->dst, 1, (JSValueConst*)&view);
    JS_FreeValue(ctx, view);
    dyn_cps_await(ctx, &ps->cps, pipe_step_write_done, p);
}

static void pipe_step_write_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    pipe_state_t* ps = (pipe_state_t*)cp;
    int64_t accepted = 0;
    size_t remaining = ps->chunk_read - ps->chunk_written;

    if (failed) {
        pipe_fail(ctx, ps, JS_DupValue(ctx, val));
        return;
    }
    if (JS_ToInt64(ctx, &accepted, val)) {
        if (JS_HasUncatchableException(ctx))
            return;
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSink.write must resolve to a non-negative byte count");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (accepted < 0 || (uint64_t)accepted > (uint64_t)remaining) {
        JS_ThrowRangeError(ctx,
            "ByteSink.write accepted a byte count outside [0, buf.length]");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (accepted == 0 && remaining > 0) {
        JS_ThrowTypeError(ctx,
            "ByteSink.write accepted 0 bytes of a non-empty chunk");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    ps->chunk_written += (size_t)accepted;
    ps->total += (uint64_t)accepted;
    if (ps->chunk_written < ps->chunk_read) {
        pipe_issue_write(ctx, ps);
        return;
    }
    if (JS_IsFunction(ctx, ps->on_chunk)) {
        JSValue nb = JS_NewInt64(ctx, (int64_t)ps->chunk_read);
        JSValue r = JS_Call(ctx, ps->on_chunk, JS_UNDEFINED, 1, &nb);
        JS_FreeValue(ctx, nb);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, r);
            pipe_fail(ctx, ps, stream_take_exc(ctx));
            return;
        }
        JS_FreeValue(ctx, r);
    }
    {
        JSValue p = JS_Call(ctx, ps->read_fn, ps->src, 1,
            (JSValueConst*)&ps->chunk);
        dyn_cps_await(ctx, &ps->cps, pipe_step_read_done, p);
    }
}

static void pipe_step_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    pipe_state_t* ps = (pipe_state_t*)cp;
    int64_t n = 0;

    if (failed) {
        pipe_fail(ctx, ps, JS_DupValue(ctx, val));
        return;
    }
    if (JS_ToInt64(ctx, &n, val)) {
        if (JS_HasUncatchableException(ctx))
            return;
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSource.read must resolve to a non-negative byte count");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (n < 0 || n > (int64_t)DYN_STREAM_CHUNK) {
        JS_ThrowRangeError(ctx,
            "ByteSource.read returned a byte count outside [0, buf.length]");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (n == 0) {
        pipe_finish(ctx, ps, 0);
        return;
    }
    ps->chunk_read = (size_t)n;
    ps->chunk_written = 0;
    pipe_issue_write(ctx, ps);
}

JSValue stream_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pipe_state_t* ps;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise, opts;
    JSValue rf, wf, oc, chunk, p;

    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "pipe(src, dst[, {onChunk}]) requires a ByteSource and a ByteSink");
    opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts) && !JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "pipe: options must be an object");

    rf = JS_GetPropertyStr(ctx, argv[0], "read");
    if (!JS_IsFunction(ctx, rf)) {
        JS_FreeValue(ctx, rf);
        return JS_ThrowTypeError(ctx,
            "pipe: src.read(buf) is not a function");
    }
    wf = JS_GetPropertyStr(ctx, argv[1], "write");
    if (!JS_IsFunction(ctx, wf)) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        return JS_ThrowTypeError(ctx,
            "pipe: dst.write(buf) is not a function");
    }
    oc = JS_UNDEFINED;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts)) {
        oc = JS_GetPropertyStr(ctx, opts, "onChunk");
        if (JS_IsException(oc)) {
            JS_FreeValue(ctx, rf);
            JS_FreeValue(ctx, wf);
            return oc;
        }
        if (JS_IsUndefined(oc) || JS_IsNull(oc)) {
            JS_FreeValue(ctx, oc);
            oc = JS_UNDEFINED;
        } else if (!JS_IsFunction(ctx, oc)) {
            JS_FreeValue(ctx, rf);
            JS_FreeValue(ctx, wf);
            JS_FreeValue(ctx, oc);
            return JS_ThrowTypeError(ctx, "pipe: onChunk must be a function");
        }
    }
    {
        JSValue len = JS_NewInt64(ctx, DYN_STREAM_CHUNK);
        chunk = JS_NewTypedArray(ctx, 1, &len, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, len);
    }
    if (JS_IsException(chunk)) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        JS_FreeValue(ctx, oc);
        return chunk;
    }

    ps = (pipe_state_t*)calloc(1, sizeof(*ps));
    if (!ps) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        JS_FreeValue(ctx, oc);
        JS_FreeValue(ctx, chunk);
        return JS_ThrowOutOfMemory(ctx);
    }
    ps->src = JS_DupValue(ctx, argv[0]);
    ps->dst = JS_DupValue(ctx, argv[1]);
    ps->read_fn = rf;
    ps->write_fn = wf;
    ps->on_chunk = oc;
    ps->chunk = chunk;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        pipe_state_free(ctx, ps);
        return promise;
    }
    ps->jresolve = funcs[0];
    ps->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), pipe_op_sweep, ps);

    p = JS_Call(ctx, ps->read_fn, ps->src, 1, (JSValueConst*)&ps->chunk);
    dyn_cps_await(ctx, &ps->cps, pipe_step_read_done, p);
    return promise;
}

#endif
