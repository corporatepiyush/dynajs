#include "sys-internal.h"
#include "core/dyn-stdio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

#define DYN_SPAWN_MAXPIPE_CAP (1 << 30)

static void spawn_pipe_consume(dyn_spawn_pipe_t* p, size_t n)
{
    if (n < p->len) {
        p->head += n;
        p->len -= n;
    } else {
        p->head = 0;
        p->len = 0;
    }
}

static int spawn_pipe_room(dyn_spawn_pipe_t* p, size_t r)
{
    if (p->head + p->len + r <= p->cap)
        return 0;
    if (p->head && p->head >= p->len) {
        memmove(p->buf, p->buf + p->head, p->len);
        p->head = 0;
    }
    if (p->head + p->len + r > p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 4096;
        uint8_t* nb;
        while (nc < p->head + p->len + r)
            nc *= 2;
        nb = (uint8_t*)realloc(p->buf, nc);
        if (!nb)
            return -1;
        p->buf = nb;
        p->cap = nc;
    }
    return 0;
}

static JSValue spawn_promise_new(JSContext* ctx, JSValue* resolve,
    JSValue* reject)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    *resolve = funcs[0];
    *reject = funcs[1];
    return promise;
}

static JSValue spawn_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, val, 0);
    return promise;
}

static JSValue spawn_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, exc, 1);
    return promise;
}

static JSValue spawn_cap_error(JSContext* ctx, dyn_spawn_t* sp, int kind)
{
    (void)sp;
    if (kind == 2)
        JS_ThrowOutOfMemory(ctx);
    else
        JS_ThrowInternalError(ctx, "Spawn: pipe drain failed");
    return JS_GetException(ctx);
}

void spawn_kill_child(dyn_spawn_t* sp, int sig)
{
    if (!sp->exited && sp->pid > 0)
        dyn_kill_group(sp->pid, sig);
}

static void spawn_pipe_watch(dyn_spawn_pipe_t* p, int on)
{
    dyn_spawn_t* sp = p->sp;

    if (p->fd < 0 || p->eof || !sp->aio)
        return;
    if (on == p->watch)
        return;
    if (dyn_evloop_mod(dyn_aio_evloop(sp->aio), p->fd,
            on ? DYN_EV_READ : 0)
        == 0)
        p->watch = on;
}

JSValue spawn_wait_result(dyn_spawn_t* sp)
{
    JSContext* ctx = sp->ctx;
    JSValue o, v;
    const char* signame = sp->sig ? dyn_signal_name(sp->sig) : NULL;

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    v = signame ? JS_NULL : JS_NewInt32(ctx, sp->code);
    if (JS_DefinePropertyValueStr(ctx, o, "code", v, JS_PROP_C_W_E) < 0)
        goto fail;
    if (signame) {
        v = JS_NewString(ctx, signame);
        if (!JS_IsException(v) && JS_DefinePropertyValueStr(ctx, o, "signal", v, JS_PROP_C_W_E) >= 0) {
            v = JS_UNDEFINED;
        } else {
            JS_FreeValue(ctx, v);
            goto fail;
        }
    } else {
        if (JS_DefinePropertyValueStr(ctx, o, "signal", JS_NULL, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "timedOut",
            JS_NewBool(ctx, sp->timed_out), JS_PROP_C_W_E)
        < 0)
        goto fail;
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static void spawn_ref(dyn_spawn_t* sp)
{
    sp->refs++;
}

static void spawn_sweep_pipe(JSContext* ctx, JSRuntime* rt, dyn_spawn_pipe_t* p)
{
    if (!p->pending)
        return;
    if (ctx) {
        spawn_pipe_fail_pending(p, "read() aborted at engine shutdown");
        return;
    }
    p->pending = 0;
    JS_FreeValueRT(rt, p->rresolve);
    JS_FreeValueRT(rt, p->rreject);
    JS_FreeValueRT(rt, p->rbuf);
    JS_FreeValueRT(rt, p->promise);
    p->rresolve = p->rreject = p->rbuf = p->promise = JS_UNDEFINED;
    spawn_unpark(p->sp);
}

static void spawn_sweep_flush(JSContext* ctx, JSRuntime* rt, dyn_spawn_t* sp)
{
    if (JS_IsUndefined(sp->in.flush_resolve))
        return;
    if (ctx) {
        JS_ThrowTypeError(ctx, "Spawn: flush() aborted at engine shutdown");
        spawn_settle(ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            JS_GetException(ctx), 1);
    } else {
        JS_FreeValueRT(rt, sp->in.flush_resolve);
        JS_FreeValueRT(rt, sp->in.flush_reject);
        sp->in.flush_resolve = sp->in.flush_reject = JS_UNDEFINED;
    }
    spawn_unpark(sp);
}

static void spawn_sweep_wait(JSContext* ctx, JSRuntime* rt, dyn_spawn_t* sp)
{
    if (sp->wait_settled || JS_IsUndefined(sp->wait_resolve))
        return;
    sp->wait_settled = 1;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc))
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, "Spawn.wait: aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        else
            exc = JS_GetException(ctx);
        r = JS_Call(ctx, sp->wait_reject, JS_UNDEFINED, 1,
            (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValueRT(rt, sp->wait_resolve);
    JS_FreeValueRT(rt, sp->wait_reject);
    sp->wait_resolve = JS_UNDEFINED;
    sp->wait_reject = JS_UNDEFINED;
    spawn_unpark(sp);
}

void spawn_shutdown_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)opaque;

    spawn_ref(sp);
    spawn_sweep_wait(ctx, rt, sp);
    spawn_sweep_pipe(ctx, rt, &sp->out);
    spawn_sweep_pipe(ctx, rt, &sp->err);
    spawn_sweep_flush(ctx, rt, sp);
    spawn_release(sp);
}
void spawn_pipe_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spawn_pipe_t* p = (dyn_spawn_pipe_t*)JS_GetOpaque(val,
        dyn_spawn_pipe_class_id);
    if (p && p->sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(p->sp);
    }
}

void spawn_stdin_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spawn_stdin_t* in = (dyn_spawn_stdin_t*)JS_GetOpaque(val,
        dyn_spawn_stdin_class_id);
    if (in && in->sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(in->sp);
    }
}

static void spawn_reap_hook(void* ud)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)ud;
    int64_t now;

    spawn_ref(sp);
    if (sp->dead)
        goto out;
    if (spawn_try_reap(sp)) {
        spawn_after_reap(sp);
        goto out;
    }
    now = dyn_now_ms();
    if (sp->deadline && !sp->term_sent && now >= sp->deadline) {
        sp->term_sent = 1;
        sp->timed_out = 1;
        dyn_kill_group(sp->pid, SIGTERM);
        sp->kill_at = now + DYN_EXEC_GRACE_MS;
    } else if (sp->term_sent && sp->kill_at && now >= sp->kill_at) {
        dyn_kill_group(sp->pid, SIGKILL);
        sp->kill_at = 0;
    }
out:
    spawn_release(sp);
}

static void spawn_pipe_feed(dyn_spawn_pipe_t* p)
{
    dyn_spawn_t* sp = p->sp;
    JSValue rr, rj;
    size_t n;

    if (!p->pending)
        return;
    p->pending = 0;
    rr = p->rresolve;
    rj = p->rreject;
    p->rresolve = p->rreject = JS_UNDEFINED;
    JS_FreeValue(sp->ctx, p->promise);
    p->promise = JS_UNDEFINED;
    if (p->err) {
        JS_FreeValue(sp->ctx, p->rbuf);
        p->rbuf = JS_UNDEFINED;
        spawn_settle(sp->ctx, &rr, &rj, spawn_cap_error(sp->ctx, sp, p->err),
            1);
        spawn_unpark(sp);
        return;
    }
    {
        size_t rlen = 0;
        uint8_t* rbase = spawn_view_bytes(sp->ctx, p->rbuf, &rlen);
        if (!rbase) {
            JSValue ex = JS_GetException(sp->ctx);
            if (JS_IsUndefined(ex)) {
                JS_FreeValue(sp->ctx, ex);
                ex = JS_ThrowTypeError(sp->ctx,
                    "Spawn: read buffer is detached or out of bounds");
                ex = JS_GetException(sp->ctx);
            }
            JS_FreeValue(sp->ctx, p->rbuf);
            p->rbuf = JS_UNDEFINED;
            spawn_settle(sp->ctx, &rr, &rj, ex, 1);
            spawn_unpark(sp);
            return;
        }
        n = p->len < rlen ? p->len : rlen;
        if (n)
            memmove(rbase, p->buf + p->head, n);
    }
    spawn_pipe_consume(p, n);
    JS_FreeValue(sp->ctx, p->rbuf);
    p->rbuf = JS_UNDEFINED;
    spawn_pipe_watch(p, 1);
    spawn_settle(sp->ctx, &rr, &rj, JS_NewInt64(sp->ctx, (int64_t)n), 0);
    spawn_unpark(sp);
}

void spawn_pipe_force_eof(dyn_spawn_pipe_t* p)
{
    dyn_spawn_t* sp = p->sp;

    if (p->fd >= 0) {
        for (;;) {
            uint8_t chunk[DYN_EXEC_CHUNK] DYN_IO_SCRATCH;
            size_t space = p->len < sp->maxpipe ? sp->maxpipe - p->len : 0;
            size_t want = space < sizeof(chunk) ? space : sizeof(chunk);
            ssize_t r;
            if (want == 0)
                break;
            r = read(p->fd, chunk, want);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (r == 0)
                break;
            if (spawn_pipe_room(p, (size_t)r) < 0)
                break;
            memcpy(p->buf + p->head + p->len, chunk, (size_t)r);
            p->len += (size_t)r;
        }
        spawn_close_fd(sp, &p->fd);
    }
    p->eof = 1;
    if (p->pending)
        spawn_pipe_feed(p);
}

static void spawn_pipe_fail(dyn_spawn_pipe_t* p, int kind)
{
    dyn_spawn_t* sp = p->sp;

    if (!p->err)
        p->err = kind;
    spawn_kill_child(sp, SIGKILL);
    spawn_close_fd(sp, &p->fd);
    p->eof = 1;
    if (p->pending)
        spawn_pipe_feed(p);
    spawn_try_reap(sp);
}

static void spawn_pipe_cb(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    dyn_spawn_pipe_t* p = (dyn_spawn_pipe_t*)ud;
    dyn_spawn_t* sp = p->sp;
    uint8_t chunk[DYN_EXEC_CHUNK] DYN_IO_SCRATCH;

    (void)lp;
    (void)fd;
    (void)events;
    spawn_ref(sp);
    if (sp->dead || p->fd < 0)
        goto out;
    if (p->len >= sp->maxpipe) {
        spawn_pipe_watch(p, 0);
        goto out;
    }
    for (;;) {
        size_t space, want;
        ssize_t r;

        space = sp->maxpipe - p->len;
        want = sizeof chunk;
        if (space < want)
            want = space;
        r = read(p->fd, chunk, want);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                goto out;
            r = 0;
        }
        if (r == 0) {
            spawn_close_fd(sp, &p->fd);
            p->eof = 1;
            if (p->pending)
                spawn_pipe_feed(p);
            if (spawn_try_reap(sp))
                spawn_after_reap(sp);
            else
                spawn_aio_release_if_idle(sp);
            goto out;
        }
        if (spawn_pipe_room(p, (size_t)r) < 0) {
            spawn_pipe_fail(p, 1);
            goto out;
        }
        memcpy(p->buf + p->head + p->len, chunk, (size_t)r);
        p->len += (size_t)r;
        if (p->pending)
            spawn_pipe_feed(p);
        else if (p->len >= sp->maxpipe)
            spawn_pipe_watch(p, 0);
        goto out;
    }
out:
    spawn_release(sp);
}

void spawn_pipe_fail_pending(dyn_spawn_pipe_t* p, const char* msg)
{
    dyn_spawn_t* sp = p->sp;

    if (!p->pending)
        return;
    p->pending = 0;
    JS_ThrowTypeError(sp->ctx, "Spawn: %s", msg);
    spawn_settle(sp->ctx, &p->rresolve, &p->rreject, JS_GetException(sp->ctx),
        1);
    JS_FreeValue(sp->ctx, p->rbuf);
    JS_FreeValue(sp->ctx, p->promise);
    p->rbuf = JS_UNDEFINED;
    p->promise = JS_UNDEFINED;
    spawn_unpark(sp);
}

JSValue spawn_pipe_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_pipe_t* p;
    uint8_t* base;
    size_t len = 0;
    JSValue promise, resolve, reject;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: read(buf) needs a Uint8Array");
    base = spawn_view_bytes(ctx, argv[0], &len);
    if (!base)
        return JS_EXCEPTION;
    p = (dyn_spawn_pipe_t*)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
    if (!p || !p->sp || p->sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: read on a closed stdio");
    if (p->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (len == 0)
        return spawn_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (p->pending)
        return JS_ThrowTypeError(ctx,
            "Spawn: a read() is already pending on this pipe");
    if (p->err)
        return spawn_promise_rejected(ctx,
            spawn_cap_error(ctx, p->sp, p->err));
    if (p->len > 0) {
        size_t n = p->len < len ? p->len : len;
        memcpy(base, p->buf + p->head, n);
        spawn_pipe_consume(p, n);
        spawn_pipe_watch(p, 1);
        return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
    if (p->eof)
        return spawn_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    p->pending = 1;
    p->rbuf = JS_DupValue(ctx, argv[0]);
    p->rresolve = resolve;
    p->rreject = reject;
    p->promise = JS_DupValue(ctx, promise);
    JS_AddShutdownSweep(p->sp->rt, spawn_shutdown_sweep, p->sp);
    spawn_ref(p->sp);
    return promise;
}

JSValue spawn_pipe_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_pipe_t* p;
    (void)argc;
    (void)argv;

    p = (dyn_spawn_pipe_t*)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
    if (!p || !p->sp || p->sp->dead)
        return JS_UNDEFINED;
    if (p->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    spawn_pipe_fail_pending(p, "read() aborted by close()");
    spawn_pipe_watch(p, 0);
    spawn_close_fd(p->sp, &p->fd);
    spawn_pipe_consume(p, p->len);
    p->eof = 1;
    return JS_UNDEFINED;
}

#define SPAWN_PIPE_ITER_CHUNK 65536

static JSValue spawn_pipe_iter_step(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    int64_t n = 0;
    size_t len = 0;
    uint8_t* base;
    JSValue r;
    (void)this_val;
    (void)magic;

    if (argc > 0 && JS_ToInt64(ctx, &n, argv[0]) < 0)
        return JS_EXCEPTION;
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    if (n > 0) {
        JSValue v;
        base = spawn_view_bytes(ctx, data[0], &len);
        if (!base) {
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
        v = dyn_nat_u8_copy(ctx, base, (size_t)n < len ? (size_t)n : len);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, r);
            return v;
        }
        JS_DefinePropertyValueStr(ctx, r, "value", v, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, r, "done", JS_FALSE, JS_PROP_C_W_E);
    } else {
        JS_DefinePropertyValueStr(ctx, r, "value", JS_UNDEFINED, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, r, "done", JS_TRUE, JS_PROP_C_W_E);
    }
    return r;
}

static JSValue spawn_pipe_iter_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    JSValue p, then, r;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;

    p = spawn_pipe_read(ctx, data[0], 1, &data[1]);
    if (JS_IsException(p))
        return spawn_promise_rejected(ctx, JS_GetException(ctx));
    then = JS_GetPropertyStr(ctx, p, "then");
    if (JS_IsException(then)) {
        JS_FreeValue(ctx, p);
        return then;
    }
    r = JS_Call(ctx, then, p, 1, &data[2]);
    JS_FreeValue(ctx, then);
    JS_FreeValue(ctx, p);
    return r;
}

static JSValue spawn_pipe_iter_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    JSValue c, r;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;

    c = spawn_pipe_close(ctx, data[0], 0, NULL);
    if (JS_IsException(c))
        return spawn_promise_rejected(ctx, JS_GetException(ctx));
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    JS_DefinePropertyValueStr(ctx, r, "value", JS_UNDEFINED, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "done", JS_TRUE, JS_PROP_C_W_E);
    return spawn_promise_resolved(ctx, r);
}

static JSValue spawn_pipe_iter_self(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry spawn_pipe_iter_funcs[] = {
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, spawn_pipe_iter_self),
};

JSValue spawn_pipe_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue data[3], it = JS_UNDEFINED, fn, len;
    int i;
    (void)argc;
    (void)argv;

    if (!JS_GetOpaque2(ctx, this_val, dyn_spawn_pipe_class_id))
        return JS_EXCEPTION;
    data[0] = JS_DupValue(ctx, this_val);
    len = JS_NewInt32(ctx, SPAWN_PIPE_ITER_CHUNK);
    data[1] = JS_NewTypedArray(ctx, 1, &len, JS_TYPED_ARRAY_UINT8);
    data[2] = JS_UNDEFINED;
    if (JS_IsException(data[1]))
        goto fail;
    data[2] = JS_NewCFunctionData(ctx, spawn_pipe_iter_step, 1, 0, 1, &data[1]);
    if (JS_IsException(data[2]))
        goto fail;
    it = JS_NewObject(ctx);
    if (JS_IsException(it))
        goto fail;
    fn = JS_NewCFunctionData(ctx, spawn_pipe_iter_next, 0, 0, 3, data);
    if (JS_IsException(fn) || JS_DefinePropertyValueStr(ctx, it, "next", fn, JS_PROP_C_W_E) < 0)
        goto fail;
    fn = JS_NewCFunctionData(ctx, spawn_pipe_iter_return, 0, 0, 1, data);
    if (JS_IsException(fn) || JS_DefinePropertyValueStr(ctx, it, "return", fn, JS_PROP_C_W_E) < 0)
        goto fail;
    if (JS_SetPropertyFunctionList(ctx, it, spawn_pipe_iter_funcs, 1) < 0)
        goto fail;
    for (i = 0; i < 3; i++)
        JS_FreeValue(ctx, data[i]);
    return it;
fail:
    for (i = 0; i < 3; i++)
        JS_FreeValue(ctx, data[i]);
    JS_FreeValue(ctx, it);
    return JS_EXCEPTION;
}

static void spawn_stdin_unwatch(dyn_spawn_t* sp)
{
    if (sp->in.fd >= 0 && sp->aio)
        dyn_evloop_del(dyn_aio_evloop(sp->aio), sp->in.fd);
}

static void spawn_stdin_close_now(dyn_spawn_t* sp)
{
    spawn_stdin_unwatch(sp);
    if (sp->in.fd >= 0) {
        close(sp->in.fd);
        sp->in.fd = -1;
    }
    spawn_aio_release_if_idle(sp);
}

static JSValue spawn_stdin_error(JSContext* ctx, dyn_spawn_stdin_t* in)
{
    JS_ThrowInternalError(ctx, "Spawn: stdin write failed (%s)",
        strerror(in->err_errno ? in->err_errno : EPIPE));
    return JS_GetException(ctx);
}

static void spawn_stdin_reject_flush(dyn_spawn_t* sp)
{
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            spawn_stdin_error(sp->ctx, &sp->in), 1);
        spawn_unpark(sp);
    }
}

static void spawn_stdin_cb(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    dyn_spawn_stdin_t* in = (dyn_spawn_stdin_t*)ud;
    dyn_spawn_t* sp = in->sp;

    (void)lp;
    (void)fd;
    (void)events;
    spawn_ref(sp);
    if (sp->dead || in->fd < 0)
        goto out;
    while (sp->woff < sp->wlen) {
        ssize_t w = write(in->fd, sp->wq + sp->woff, sp->wlen - sp->woff);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                goto out;
            in->err = 1;
            in->err_errno = errno;
            spawn_stdin_close_now(sp);
            spawn_stdin_reject_flush(sp);
            goto out;
        }
        sp->woff += (size_t)w;
    }
    sp->woff = sp->wlen = 0;
    spawn_stdin_unwatch(sp);
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            JS_UNDEFINED, 0);
        spawn_unpark(sp);
    }
    if (in->close_after_drain)
        spawn_stdin_close_now(sp);
out:
    spawn_release(sp);
}

static int spawn_stdin_enqueue(dyn_spawn_t* sp, const uint8_t* src, size_t n)
{
    if (n > sp->maxpipe || sp->wlen > sp->maxpipe - n) {
        JS_ThrowRangeError(sp->ctx,
            "Spawn: stdin queue exceeds maxPipe (%u bytes)",
            (unsigned)sp->maxpipe);
        return -1;
    }
    if (sp->wlen + n > sp->wcap) {
        size_t nc = sp->wcap ? sp->wcap : 4096;
        uint8_t* nw;
        while (nc < sp->wlen + n)
            nc *= 2;
        nw = (uint8_t*)realloc(sp->wq, nc);
        if (!nw) {
            JS_ThrowOutOfMemory(sp->ctx);
            return -1;
        }
        sp->wq = nw;
        sp->wcap = nc;
    }
    memcpy(sp->wq + sp->wlen, src, n);
    sp->wlen += n;
    if (sp->in.fd >= 0 && sp->aio)
        dyn_evloop_mod(dyn_aio_evloop(sp->aio), sp->in.fd, DYN_EV_WRITE);
    return 0;
}

JSValue spawn_stdin_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    const uint8_t* src;
    size_t n = 0;
    int is_str;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: stdin.write(buf) needs bytes");
    is_str = JS_IsString(argv[0]);
    if (is_str) {
        const char* cs;
        n = 0;
        cs = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!cs)
            return JS_EXCEPTION;
        src = (const uint8_t*)cs;
    } else {
        src = spawn_view_bytes(ctx, argv[0], &n);
        if (!src)
            return JS_EXCEPTION;
    }
    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_ThrowTypeError(ctx, "Spawn: write on a closed stdin");
    }
    if (in->sp->user_closed) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    }
    if (in->err || in->fd < 0) {
        JSValue r;
        if (in->err)
            r = spawn_promise_rejected(ctx, spawn_stdin_error(ctx, in));
        else if (!in->piped)
            r = (JSValue)JS_ThrowTypeError(ctx,
                "Spawn: stdin is not piped (pass opts.stdin: \"pipe\")");
        else
            r = (JSValue)JS_ThrowTypeError(ctx,
                "Spawn: stdin is closed (EOF already sent)");
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return r;
    }
    if (n && spawn_stdin_enqueue(in->sp, src, n) < 0) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_EXCEPTION;
    }
    if (is_str)
        JS_FreeCString(ctx, (const char*)src);
    return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
}

JSValue spawn_stdin_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    JSValue promise, resolve, reject;
    (void)argc;
    (void)argv;

    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: flush on a closed stdin");
    if (in->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (in->err)
        return spawn_promise_rejected(ctx, spawn_stdin_error(ctx, in));
    if (in->sp->woff >= in->sp->wlen)
        return spawn_promise_resolved(ctx, JS_UNDEFINED);
    if (!JS_IsUndefined(in->flush_resolve))
        return JS_ThrowTypeError(ctx,
            "Spawn: a flush() is already pending on stdin");
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    in->flush_resolve = resolve;
    in->flush_reject = reject;
    JS_AddShutdownSweep(in->sp->rt, spawn_shutdown_sweep, in->sp);
    spawn_ref(in->sp);
    return promise;
}

JSValue spawn_stdin_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    (void)argc;
    (void)argv;

    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead)
        return JS_UNDEFINED;
    if (in->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (in->fd < 0)
        return JS_UNDEFINED;
    in->close_after_drain = 1;
    if (in->sp->woff >= in->sp->wlen)
        spawn_stdin_close_now(in->sp);
    return JS_UNDEFINED;
}

JSValue dyn_spawn_wait(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    JSValue promise, resolve, reject;
    (void)argc;
    (void)argv;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        return spawn_promise_resolved(ctx, spawn_wait_result(sp));
    if (!sp->exited && spawn_try_reap(sp))
        spawn_after_reap(sp);
    if (sp->exited)
        return spawn_promise_resolved(ctx, spawn_wait_result(sp));
    if (!JS_IsUndefined(sp->wait_resolve))
        return JS_ThrowTypeError(ctx, "Spawn: wait() is already pending");
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    sp->wait_resolve = resolve;
    sp->wait_reject = reject;
    JS_AddShutdownSweep(sp->rt, spawn_shutdown_sweep, sp);
    spawn_ref(sp);
    if (spawn_try_reap(sp))
        spawn_after_reap(sp);
    return promise;
}

static int spawn_sig_from_name(const char* s)
{
    static const struct {
        const char* nm;
        int sig;
    } tab[] = {
        { "HUP", SIGHUP },
        { "INT", SIGINT },
        { "QUIT", SIGQUIT },
        { "ILL", SIGILL },
        { "ABRT", SIGABRT },
        { "FPE", SIGFPE },
        { "KILL", SIGKILL },
        { "SEGV", SIGSEGV },
        { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM },
        { "TERM", SIGTERM },
        { "BUS", SIGBUS },
        { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 },
        { "CHLD", SIGCHLD },
        { "CONT", SIGCONT },
        { "STOP", SIGSTOP },
        { "TSTP", SIGTSTP },
        { "TTIN", SIGTTIN },
        { "TTOU", SIGTTOU },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcasecmp(tab[i].nm, s) == 0)
            return tab[i].sig;
    return 0;
}

JSValue dyn_spawn_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    int sig = SIGTERM;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        return JS_FALSE;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
            const char* s = JS_ToCString(ctx, argv[0]);
            const char* nm;
            if (!s)
                return JS_EXCEPTION;
            nm = !strncasecmp(s, "SIG", 3) ? s + 3 : s;
            sig = spawn_sig_from_name(nm);
            JS_FreeCString(ctx, s);
            if (sig <= 0)
                return JS_ThrowRangeError(ctx, "Spawn: unknown signal name");
        } else if (JS_IsNumber(argv[0])) {
            int32_t n;
            if (JS_ToInt32(ctx, &n, argv[0]))
                return JS_EXCEPTION;
            if (n <= 0 || n > 31)
                return JS_ThrowRangeError(ctx,
                    "Spawn: signal number out of range");
            sig = n;
        } else {
            return JS_ThrowTypeError(ctx,
                "Spawn: kill(signal) takes a number or a \"SIGxxx\" string");
        }
    }
    if (sp->exited)
        return JS_FALSE;
    if (sig == SIGTERM || sig == SIGKILL || sig == SIGINT || sig == SIGHUP || sig == SIGQUIT)
        sp->kill_fatal = 1;
    dyn_kill_group(sp->pid, sig);
    spawn_ref(sp);
    if (spawn_try_reap(sp))
        spawn_after_reap(sp);
    spawn_release(sp);
    return JS_TRUE;
}

JSValue spawn_get_stdin(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_stdin_class_id, &sp->in);
}

static int spawn_strv_at(JSContext* ctx, JSValueConst arr, uint32_t i,
    char** slot)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    const char* s;

    if (JS_IsException(e))
        return -1;
    if (!JS_IsString(e)) {
        JS_FreeValue(ctx, e);
        JS_ThrowTypeError(ctx, "Spawn: every argument must be a string");
        return -1;
    }
    s = dyn_sys_cstr(ctx, e, "Spawn: args");
    JS_FreeValue(ctx, e);
    if (!s)
        return -1;
    *slot = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*slot) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static char** spawn_strv(JSContext* ctx, JSValueConst arr, const char* first)
{
    uint32_t n = 0, i, k = 0;
    char** v;
    JSValue lv;

    if (JS_IsArray(ctx, arr) == 1) {
        lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv) < 0) {
            JS_FreeValue(ctx, lv);
            return NULL;
        }
        JS_FreeValue(ctx, lv);
        if (n > 65535) {
            JS_ThrowRangeError(ctx, "Spawn: too many arguments");
            return NULL;
        }
    }
    v = (char**)calloc((size_t)n + 2, sizeof *v);
    if (!v) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    if (first) {
        v[k] = strdup(first);
        if (!v[k]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        k++;
    }
    for (i = 0; i < n; i++) {
        if (spawn_strv_at(ctx, arr, i, &v[k]) < 0)
            goto fail;
        k++;
    }
    return v;
fail:
    for (i = 0; i < k; i++)
        free(v[i]);
    free(v);
    return NULL;
}

static char** spawn_envv(JSContext* ctx, JSValueConst obj)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i, k = 0;
    char** v;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return NULL;
    v = (char**)calloc((size_t)len + 1, sizeof *v);
    if (!v) {
        JS_FreePropertyEnum(ctx, tab, len);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if (spawn_env_at(ctx, obj, tab[i].atom, &v[k]) < 0) {
            uint32_t j;
            JS_FreePropertyEnum(ctx, tab, len);
            for (j = 0; j < k; j++)
                free(v[j]);
            free(v);
            return NULL;
        }
        k++;
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return v;
}

static void spawn_sigpipe_ignore(void)
{
    static int done;
    struct sigaction sa, old;

    if (done)
        return;
    done = 1;
    if (sigaction(SIGPIPE, NULL, &old) != 0)
        return;
    if (old.sa_handler != SIG_DFL)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

static void spawn_ctor_free_exec(dyn_exec_t* e, JSContext* ctx,
    JSValue input_ref)
{
    dyn_strv_free(e->argv);
    dyn_strv_free(e->envp);
    free(e->cwd);
    free(DYN_UNCONST(e->input));
    e->input = NULL;
    JS_FreeValue(ctx, input_ref);
}

JSValue dyn_spawn_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_exec_t e;
    JSValue obj = JS_UNDEFINED, input_ref = JS_UNDEFINED;
    JSValue proto;
    dyn_spawn_t* sp = NULL;
    const char *cmd, *path = NULL;
    struct dyn_aio* aio;
    size_t i;
    int64_t timeout_ms = 0, maxpipe = (int64_t)DYN_EXEC_MAXBUF;
    int stdin_pipe = 0, stdin_inherit = 0;
    int64_t uid = -1, gid = -1;
    pid_t pid;

    memset(&e, 0, sizeof e);
    e.in[0] = e.in[1] = e.out[0] = e.out[1] = e.err[0] = e.err[1] = -1;
    e.uid = (uid_t)-1;
    e.gid = (gid_t)-1;

    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx,
            "sys.Spawn: command must be a string");
        goto fail_noobj;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
        && JS_IsArray(ctx, argv[1]) != 1) {
        JS_ThrowTypeError(ctx,
            "sys.Spawn: args must be an array -- there is no "
            "shell, so a command line is not a string here");
        goto fail_noobj;
    }
    cmd = dyn_sys_cstr(ctx, argv[0], "Spawn: command");
    if (!cmd)
        goto fail_noobj;
    e.argv = spawn_strv(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, cmd);
    JS_FreeCString(ctx, cmd);
    if (!e.argv)
        goto fail_noobj;

    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        JSValue v;
        if (!JS_IsObject(argv[2])) {
            JS_ThrowTypeError(ctx,
                "sys.Spawn: options must be an object");
            goto fail;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "cwd");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* s = dyn_sys_cstr(ctx, v, "Spawn: cwd");
            struct stat st;
            JS_FreeValue(ctx, v);
            if (!s)
                goto fail;
            if (stat(s, &st) < 0 || !S_ISDIR(st.st_mode)) {
                JS_ThrowInternalError(ctx,
                    "Spawn: cwd is not a directory: %s", s);
                JS_FreeCString(ctx, s);
                goto fail;
            }
            e.cwd = strdup(s);
            JS_FreeCString(ctx, s);
            if (!e.cwd) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
        } else {
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "env");
        if (JS_IsException(v))
            goto fail;
        if (JS_IsObject(v)) {
            e.envp = spawn_envv(ctx, v);
            if (!e.envp) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
        }
        JS_FreeValue(ctx, v);
        if (dyn_env_extend_opt(ctx, argv[2], "Spawn", &e.envp) < 0)
            goto fail;
        if (spawn_opt_int(ctx, argv[2], "timeoutMs", 0,
                "timeoutMs must not be negative", &timeout_ms)
                < 0
            || spawn_opt_int(ctx, argv[2], "maxPipe", 1,
                   "maxPipe must be positive", &maxpipe)
                < 0
            || spawn_opt_id(ctx, argv[2], "uid", &uid) < 0
            || spawn_opt_id(ctx, argv[2], "gid", &gid) < 0)
            goto fail;
        if (maxpipe > DYN_SPAWN_MAXPIPE_CAP) {
            JS_ThrowRangeError(ctx,
                "Spawn: maxPipe must be 1 to %d",
                DYN_SPAWN_MAXPIPE_CAP);
            goto fail;
        }
        e.uid = (uid_t)uid;
        e.gid = (gid_t)gid;
        v = JS_GetPropertyStr(ctx, argv[2], "stdin");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* sm = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!sm)
                goto fail;
            if (strcmp(sm, "pipe") == 0)
                stdin_pipe = 1;
            else if (strcmp(sm, "inherit") == 0)
                stdin_inherit = 1;
            else if (strcmp(sm, "ignore") != 0) {
                JS_FreeCString(ctx, sm);
                JS_ThrowRangeError(ctx,
                    "Spawn: stdin must be \"pipe\", \"ignore\" or \"inherit\"");
                goto fail;
            }
            JS_FreeCString(ctx, sm);
        } else {
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "allowPathCwd");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                goto fail;
            e.no_path_cwd = !b;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    e.timeout_ms = timeout_ms;

    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "input");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const uint8_t* src;
            size_t n = 0;
            int is_str = JS_IsString(v);
            n = 0;
            if (is_str) {
                const char* cs = JS_ToCStringLen(ctx, &n, v);
                src = (const uint8_t*)cs;
            } else {
                src = spawn_view_bytes(ctx, v, &n);
            }
            if (!src) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            stdin_pipe = 1;
            e.input = (const uint8_t*)malloc(n ? n : 1);
            if (!e.input) {
                if (is_str)
                    JS_FreeCString(ctx, (const char*)src);
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            memcpy(DYN_UNCONST(e.input), src, n);
            if (is_str)
                JS_FreeCString(ctx, (const char*)src);
            e.inlen = n;
        }
        JS_FreeValue(ctx, v);
    }

    for (i = 0; e.envp && e.envp[i]; i++)
        if (strncmp(e.envp[i], "PATH=", 5) == 0) {
            path = e.envp[i] + 5;
            break;
        }
    if (!path && !e.envp)
        path = getenv("PATH");
    if (dyn_path_lookup(e.argv[0], path, e.path, sizeof e.path,
            e.no_path_cwd)
        < 0) {
        JS_ThrowInternalError(ctx, "Spawn: command not found: %s",
            e.argv[0]);
        goto fail;
    }
    if ((stdin_pipe && dyn_pipe_cloexec(e.in) < 0) || dyn_pipe_cloexec(e.out) < 0 || dyn_pipe_cloexec(e.err) < 0) {
        JS_ThrowInternalError(ctx, "Spawn: pipe failed (%s)", strerror(errno));
        goto fail;
    }
    e.maxfd = dyn_exec_fd_limit();

    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) {
        JS_ThrowInternalError(ctx,
            "Spawn: the async reactor is unavailable in this build");
        goto fail;
    }
    spawn_sigpipe_ignore();

    dyn_stdio_sync();
    dyn_childreg_block(&e.sigmask);
    pid = fork();
    if (pid > 0)
        dyn_childreg_add(pid);
    if (pid != 0)
        dyn_childreg_unblock(&e.sigmask);
    if (pid < 0) {
        JS_ThrowInternalError(ctx, "Spawn: fork failed (%s)",
            strerror(errno));
        dyn_net_reactor_release(ctx);
        goto fail;
    }
    if (pid == 0) {
        if (e.in[1] >= 0)
            close(e.in[1]);
        close(e.out[0]);
        close(e.err[0]);
        if (!stdin_pipe && !stdin_inherit) {
            int nul = open("/dev/null", O_RDONLY);
            if (nul < 0)
                _exit(127);
            if (nul != 0) {
                if (dup2(nul, 0) < 0)
                    _exit(127);
                close(nul);
            }
        }
        dyn_exec_child(&e);
        _exit(127);
    }
    if (e.in[0] >= 0) {
        close(e.in[0]);
        e.in[0] = -1;
    }
    close(e.out[1]);
    e.out[1] = -1;
    close(e.err[1]);
    e.err[1] = -1;
    dyn_fd_set_nonblock(e.out[0]);
    dyn_fd_set_nonblock(e.err[0]);
    if (stdin_pipe)
        dyn_fd_set_nonblock(e.in[1]);

    sp = (dyn_spawn_t*)calloc(1, sizeof *sp);
    if (!sp) {
        JS_ThrowOutOfMemory(ctx);
        dyn_net_reactor_release(ctx);
        goto fail_parent;
    }
    sp->refs = 1;
    sp->rt = JS_GetRuntime(ctx);
    sp->ctx = ctx;
    sp->pid = pid;
    sp->aio = aio;
    sp->maxpipe = (size_t)maxpipe;
    sp->out.sp = sp;
    sp->err.sp = sp;
    sp->in.sp = sp;
    sp->wait_resolve = sp->wait_reject = JS_UNDEFINED;
    sp->out.rbuf = sp->out.rresolve = sp->out.rreject = JS_UNDEFINED;
    sp->err.rbuf = sp->err.rresolve = sp->err.rreject = JS_UNDEFINED;
    sp->out.promise = sp->err.promise = JS_UNDEFINED;
    sp->in.flush_resolve = sp->in.flush_reject = JS_UNDEFINED;
    sp->out.fd = e.out[0];
    sp->err.fd = e.err[0];
    sp->in.fd = stdin_pipe ? e.in[1] : -1;
    sp->in.piped = stdin_pipe;
    if (timeout_ms > 0) {
        int64_t now0 = dyn_now_ms();
        sp->deadline = timeout_ms > INT64_MAX - now0 ? INT64_MAX
                                                     : now0 + timeout_ms;
    }

    proto = dyn_ctor_proto(ctx, new_target, dyn_spawn_class_id);
    if (JS_IsException(proto))
        goto fail_spawn;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_spawn_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        obj = JS_UNDEFINED;
        goto fail_spawn;
    }
    JS_SetOpaque(obj, sp);

    sp->out.watch = sp->err.watch = 1;
    if (dyn_evloop_add(dyn_aio_evloop(aio), sp->out.fd, DYN_EV_READ,
            spawn_pipe_cb, &sp->out)
            < 0
        || dyn_evloop_add(dyn_aio_evloop(aio), sp->err.fd, DYN_EV_READ,
               spawn_pipe_cb, &sp->err)
            < 0
        || (sp->in.fd >= 0
            && dyn_evloop_add(dyn_aio_evloop(aio), sp->in.fd, 0,
                   spawn_stdin_cb, &sp->in)
                < 0)) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not watch the child's pipes (%s)", strerror(errno));
        goto fail_spawn;
    }
    if (dyn_net_on_drain(spawn_reap_hook, sp) < 0) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not register the reaper; wait() may never settle");
        goto fail_spawn;
    }
    sp->hooked = 1;

    if (e.input) {
        if (spawn_stdin_enqueue(sp, e.input, e.inlen) < 0)
            goto fail_spawn;
        sp->in.close_after_drain = 1;
    }

    spawn_ctor_free_exec(&e, ctx, input_ref);
    return obj;

fail_spawn:
    if (!JS_IsUndefined(obj)) {
        JS_SetOpaque(obj, NULL);
        JS_FreeValue(ctx, obj);
        obj = JS_UNDEFINED;
    }
    if (sp && !sp->dead)
        spawn_release(sp);
    sp = NULL;
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail_parent:
    {
        int status;
        dyn_kill_group(pid, SIGKILL);
        dyn_waitpid_eintr(pid, &status, 0);
        dyn_childreg_del(pid);
    }
    {
        int k;
        for (k = 0; k < 2; k++) {
            if (e.in[k] >= 0)
                close(e.in[k]);
            if (e.out[k] >= 0)
                close(e.out[k]);
            if (e.err[k] >= 0)
                close(e.err[k]);
        }
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail:
    for (i = 0; i < 2; i++) {
        if (e.in[i] >= 0)
            close(e.in[i]);
        if (e.out[i] >= 0)
            close(e.out[i]);
        if (e.err[i] >= 0)
            close(e.err[i]);
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail_noobj:
    return JS_EXCEPTION;
}

#endif
