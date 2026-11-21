#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#include "dyna-libc.h"
#include "core/dyn-pgwire.h"
#include <stddef.h>
#include <sys/socket.h>

#define PGC_FRAME (1u << 20)
#define PGC_HWM_DEFAULT (1u << 20)
#define PGC_HWM_MAX (1u << 30)
#define PGC_BURST 256

struct pg_copy {
    int refs, dir;
    int started, finished, stop, discard, awaiting, drain_wait, final_sent, yielding, returned;
    int settled, rejected;
    dyn_pg_t* g;
    dyn_pg_pending_t* p;
    uint8_t* buf;
    size_t blen;
    JSValue iter, next_fn;
    pg_cps_t cps;
    uint64_t hwm, qbytes;
    JSValue* q;
    uint32_t* qlen;
    size_t qh, qn, qcap;
    JSValue wres, wrej, result;
};

JSClassID pgco_class_id;

static pg_copy_t* copy_new(int dir)
{
    pg_copy_t* c = (pg_copy_t*)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->refs = 1;
    c->dir = dir;
    c->hwm = PGC_HWM_DEFAULT;
    c->iter = c->next_fn = c->wres = c->wrej = c->result = JS_UNDEFINED;
    return c;
}

static void copy_defuse_rt(JSRuntime* rt, pg_copy_t* c)
{
    pg_cps_ref_t* r = c->cps.refs;
    c->cps.refs = NULL;
    while (r) {
        pg_cps_ref_t* n = r->next;
        JS_SetOpaque(r->obj, NULL);
        JS_FreeValueRT(rt, r->obj);
        free(r);
        r = n;
    }
}

static void copy_unref_rt(JSRuntime* rt, pg_copy_t* c)
{
    size_t i;
    if (--c->refs > 0)
        return;
    free(c->buf);
    JS_FreeValueRT(rt, c->iter);
    JS_FreeValueRT(rt, c->next_fn);
    JS_FreeValueRT(rt, c->wres);
    JS_FreeValueRT(rt, c->wrej);
    JS_FreeValueRT(rt, c->result);
    for (i = 0; i < c->qn; i++)
        JS_FreeValueRT(rt, c->q[(c->qh + i) % c->qcap]);
    free(c->q);
    free(c->qlen);
    free(c);
}

static JSValue copy_take(JSContext* ctx)
{
    int unc = JS_HasUncatchableException(ctx);
    JSValue e = JS_GetException(ctx);
    if (unc)
        js_std_set_job_error_rt(JS_GetRuntime(ctx), JS_DupValue(ctx, e));
    return e;
}

int pg_sql_is_copy(const char* s)
{
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v' || *s == '(')
            s++;
        if (s[0] == '-' && s[1] == '-') {
            while (*s && *s != '\n')
                s++;
            continue;
        }
        if (s[0] == '/' && s[1] == '*') {
            int depth = 1;
            s += 2;
            while (*s && depth) {
                if (s[0] == '/' && s[1] == '*')
                    depth++, s++;
                else if (s[0] == '*' && s[1] == '/')
                    depth--, s++;
                s++;
            }
            continue;
        }
        break;
    }
    return (s[0] | 32) == 'c' && (s[1] | 32) == 'o' && (s[2] | 32) == 'p' && (s[3] | 32) == 'y'
        && !((s[4] >= 'a' && s[4] <= 'z') || (s[4] >= 'A' && s[4] <= 'Z') || (s[4] >= '0' && s[4] <= '9') || s[4] == '_' || s[4] == '$' || (unsigned char)s[4] >= 0x80);
}

int pg_drain_wq(dyn_pg_t* g)
{
    dyn_pg_pending_t* p;
    while (!g->copy_gate && (p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
        g->nwait--;
        if (p->bytes && pg_write(g, p->bytes, p->nbytes) < 0) {
            pgp_push(&g->wq_head, &g->wq_tail, p);
            g->nwait++;
            return -1;
        }
        free(p->bytes);
        p->bytes = NULL;
        p->nbytes = 0;
        if (g->query_timeout_ms)
            p->deadline_ms = dyn_timer_now_ms() + g->query_timeout_ms;
        pgp_push(&g->head, &g->tail, p);
        g->npending++;
        if (p->gates) {
            g->copy_gate = 1;
            g->gate_p = p;
        }
    }
    return 0;
}

void pg_copy_gate_check(dyn_pg_t* g)
{
    dyn_pg_pending_t* p;
    for (p = g->head; p; p = p->next)
        if (p == g->gate_p)
            return;
    g->copy_gate = 0;
    g->gate_p = NULL;
    if (pg_drain_wq(g) < 0) {
        pg_fail_all(g, "PostgreSQL: out of memory");
        return;
    }
    if (pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot write to the socket");
}

static int copy_frame(dyn_pg_t* g, char t, const uint8_t* b, size_t n)
{
    uint8_t h[5];
    h[0] = (uint8_t)t;
    put32(h + 1, (uint32_t)(n + 4));
    if (pg_write(g, h, 5) < 0 || (n && pg_write(g, b, n) < 0))
        return -1;
    return 0;
}

static int copy_data(dyn_pg_t* g, const uint8_t* b, size_t n)
{
    while (n) {
        size_t k = n > PGC_FRAME ? PGC_FRAME : n;
        if (copy_frame(g, 'd', b, k) < 0)
            return -1;
        b += k;
        n -= k;
    }
    return 0;
}

static void copy_send_fail(dyn_pg_t* g, const char* msg)
{
    if (copy_frame(g, 'f', (const uint8_t*)msg, strlen(msg) + 1) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send CopyFail");
}

static void copy_close_iter(JSContext* ctx, pg_copy_t* c)
{
    JSValue fn, r;
    if (!JS_IsObject(c->iter))
        return;
    fn = JS_GetPropertyStr(ctx, c->iter, "return");
    if (JS_IsException(fn)) {
        dyn_exc_drop(ctx);
        return;
    }
    if (JS_IsFunction(ctx, fn)) {
        r = JS_Call(ctx, fn, c->iter, 0, NULL);
        if (JS_IsException(r))
            dyn_exc_drop(ctx);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
}

static void copy_failed(dyn_pg_t* g, pg_copy_t* c, JSValueConst e, int close_iter)
{
    JSContext* ctx = g->ctx;
    if (c->finished || c->stop)
        return;
    c->finished = 1;
    if (c->p && JS_IsUndefined(c->p->error))
        c->p->error = JS_DupValue(ctx, e);
    copy_send_fail(g, "copyFrom source failed; the client rejects with the source error");
    if (close_iter)
        copy_close_iter(ctx, c);
}

static uint8_t* copy_bytes_of(JSContext* ctx, JSValueConst v, size_t* n)
{
    static uint8_t empty[1];
    int k = JS_GetBufferKind(v);
    if (k == JS_BUFFER_KIND_BUFFER) {
        uint8_t* p = JS_GetArrayBuffer(ctx, n, v);
        return p ? p : (*n == 0 ? empty : NULL);
    }
    if (k == JS_BUFFER_KIND_VIEW) {
        size_t off = 0, len = 0, bpe = 0, bl = 0;
        JSValue b = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        uint8_t* p;
        if (JS_IsException(b))
            return NULL;
        p = JS_GetArrayBuffer(ctx, &bl, b);
        JS_FreeValue(ctx, b);
        if (!p || off + len > bl)
            return len == 0 ? empty : NULL;
        *n = len;
        return p + off;
    }
    return NULL;
}

static int copy_chunk(dyn_pg_t* g, JSValueConst v)
{
    JSContext* ctx = g->ctx;
    size_t n = 0;
    int rc;
    if (JS_IsString(v)) {
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        rc = copy_data(g, (const uint8_t*)s, n);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t* b = copy_bytes_of(ctx, v, &n);
        if (!b) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: the source yielded a value that is not a string, Uint8Array or ArrayBuffer");
            return -1;
        }
        rc = copy_data(g, b, n);
    }
    if (rc < 0)
        JS_ThrowOutOfMemory(ctx);
    return rc;
}

static void copy_pump(dyn_pg_t* g, pg_copy_t* c);

static void copy_result(dyn_pg_t* g, pg_copy_t* c, JSValueConst r)
{
    JSContext* ctx = g->ctx;
    JSValue v, e;
    int done;
    if (!JS_IsObject(r)) {
        JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: the iterator result is not an object");
        goto fail;
    }
    v = JS_GetPropertyStr(ctx, r, "done");
    if (JS_IsException(v))
        goto fail;
    done = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (done < 0)
        goto fail;
    if (done) {
        c->finished = 1;
        if (copy_frame(g, 'c', NULL, 0) < 0)
            pg_fail_all(g, "PostgreSQL: out of memory");
        return;
    }
    v = JS_GetPropertyStr(ctx, r, "value");
    if (JS_IsException(v))
        goto fail;
    if (copy_chunk(g, v) < 0) {
        JS_FreeValue(ctx, v);
        e = copy_take(ctx);
        copy_failed(g, c, e, 1);
        JS_FreeValue(ctx, e);
        return;
    }
    JS_FreeValue(ctx, v);
    return;
fail:
    e = copy_take(ctx);
    copy_failed(g, c, e, 0);
    JS_FreeValue(ctx, e);
}

static void copy_step(JSContext* ctx, pg_cps_t* cp, JSValueConst val, int failed)
{
    pg_copy_t* c = (pg_copy_t*)(void*)((char*)cp - offsetof(pg_copy_t, cps));
    dyn_pg_t* g = c->g;
    c->awaiting = 0;
    copy_defuse_rt(JS_GetRuntime(ctx), c);
    if (g && !c->stop && !c->finished) {
        g->cb_depth++;
        if (c->yielding)
            c->yielding = 0;
        else if (failed)
            copy_failed(g, c, val, 0);
        else
            copy_result(g, c, val);
        if (!pg_gone(g)) {
            if (!c->stop && !c->finished)
                copy_pump(g, c);
            else if (pg_flush(g) < 0)
                pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        }
        pg_cb_exit(g);
    }
    copy_unref_rt(JS_GetRuntime(ctx), c);
}

static int copy_rx_ready(dyn_pg_t* g)
{
#ifdef CONFIG_IO_URING
    (void)g;
    return 0;
#else
    uint8_t b;
    return g->fd >= 0 && recv(g->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
#endif
}

static void copy_pump(dyn_pg_t* g, pg_copy_t* c)
{
    JSContext* ctx = g->ctx;
    int burst = 0;
    c->drain_wait = 0;
    while (!c->stop && !c->finished && !c->awaiting && !pg_gone(g)) {
        JSValue r;
        if (g->olen && pg_flush(g) < 0) {
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
            return;
        }
        if ((uint64_t)dyn_aio_queued(g->aio, g->fd) > c->hwm
            || (burst >= PGC_BURST && (dyn_aio_queued(g->aio, g->fd) > 0 || copy_rx_ready(g)))) {
            c->drain_wait = 1;
            break;
        }
        if (burst++ >= PGC_BURST) {
            JSValue fr[2], pr = JS_NewPromiseCapability(ctx, fr);
            if (!JS_IsException(pr)) {
                JSValue x = JS_Call(ctx, fr[0], JS_UNDEFINED, 0, NULL);
                JS_FreeValue(ctx, x);
                JS_FreeValue(ctx, fr[0]);
                JS_FreeValue(ctx, fr[1]);
            }
            c->yielding = 1;
            c->awaiting = 1;
            c->refs++;
            pg_cps_await(ctx, &c->cps, copy_step, pr);
            break;
        }
        r = JS_Call(ctx, c->next_fn, c->iter, 0, NULL);
        if (JS_IsException(r)) {
            JSValue e = copy_take(ctx);
            copy_failed(g, c, e, 0);
            JS_FreeValue(ctx, e);
            break;
        }
        if ((int)JS_PromiseState(ctx, r) >= 0) {
            c->awaiting = 1;
            c->refs++;
            pg_cps_await(ctx, &c->cps, copy_step, r);
            break;
        }
        copy_result(g, c, r);
        JS_FreeValue(ctx, r);
    }
    if (!pg_gone(g) && pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot write to the socket");
}

static void pg_rx_resume(dyn_pg_t* g)
{
    g->cb_depth++;
    g->rx_paused = 0;
    pg_parse_buffered(g);
    pg_cb_exit(g);
}

void pg_copy_tick(dyn_pg_t* g)
{
    pg_copy_t* c = g->head ? g->head->copy : NULL;
    if (c && c->dir == 2 && c->discard && g->rx_paused) {
        pg_rx_resume(g);
        return;
    }
    if (c && c->dir == 1 && c->drain_wait && !c->awaiting && !c->stop && !c->finished
        && (uint64_t)dyn_aio_queued(g->aio, g->fd) <= c->hwm)
        copy_pump(g, c);
}

static JSValue iter_result(JSContext* ctx, JSValue v, int done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, v);
        return o;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "value", v, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int pg_can_pause(dyn_pg_t* g)
{
#ifdef CONFIG_IO_URING
    (void)g;
    return 0;
#else
#ifdef CONFIG_TLS
    if (g->use_tls)
        return 0;
#endif
    return g->fd >= 0;
#endif
}


static void copy_wake(JSContext* ctx, pg_copy_t* c)
{
    JSValue arg, fn, other, r;
    int rej = 0;
    if (JS_IsUndefined(c->wres))
        return;
    if (c->qn && !c->discard) {
        JSValue v = c->q[c->qh];
        c->qbytes -= c->qlen[c->qh];
        c->qh = (c->qh + 1) % c->qcap;
        c->qn--;
        arg = iter_result(ctx, v, 0);
    } else if (c->returned || c->final_sent) {
        arg = iter_result(ctx, JS_UNDEFINED, 1);
    } else if (c->settled) {
        c->final_sent = 1;
        if (c->rejected) {
            rej = 1;
            arg = JS_DupValue(ctx, c->result);
        } else {
            arg = iter_result(ctx, JS_DupValue(ctx, c->result), 1);
        }
    } else {
        return;
    }
    if (JS_IsException(arg)) {
        arg = copy_take(ctx);
        rej = 1;
    }
    fn = rej ? c->wrej : c->wres;
    other = rej ? c->wres : c->wrej;
    c->wres = c->wrej = JS_UNDEFINED;
    r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&arg);
    if (JS_IsException(r))
        dyn_exc_drop(ctx);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, arg);
    if (c->g && c->g->rx_paused && (c->discard || c->qbytes <= c->hwm / 2))
        pg_rx_resume(c->g);
}

static void copy_deliver(dyn_pg_t* g, pg_copy_t* c, const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    JSValue ab, u;
    JSValueConst a3[3];
    if (c->discard)
        return;
    ab = JS_NewArrayBufferCopy(ctx, b, len);
    if (JS_IsException(ab)) {
        dyn_exc_drop(ctx);
        pg_fail_all(g, "PostgreSQL: out of memory during COPY");
        return;
    }
    a3[0] = ab;
    a3[1] = JS_NewInt32(ctx, 0);
    a3[2] = JS_NewInt32(ctx, (int32_t)len);
    u = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u)) {
        dyn_exc_drop(ctx);
        pg_fail_all(g, "PostgreSQL: out of memory during COPY");
        return;
    }
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16, i;
        JSValue* nq = (JSValue*)malloc(nc * sizeof *nq);
        uint32_t* nl = (uint32_t*)malloc(nc * sizeof *nl);
        if (!nq || !nl) {
            free(nq);
            free(nl);
            JS_FreeValue(ctx, u);
            pg_fail_all(g, "PostgreSQL: out of memory during COPY");
            return;
        }
        for (i = 0; i < c->qn; i++) {
            nq[i] = c->q[(c->qh + i) % c->qcap];
            nl[i] = c->qlen[(c->qh + i) % c->qcap];
        }
        free(c->q);
        free(c->qlen);
        c->q = nq;
        c->qlen = nl;
        c->qh = 0;
        c->qcap = nc;
    }
    c->q[(c->qh + c->qn) % c->qcap] = u;
    c->qlen[(c->qh + c->qn) % c->qcap] = (uint32_t)len;
    c->qn++;
    c->qbytes += len;
    copy_wake(ctx, c);
    if (c->qbytes >= c->hwm && !g->rx_paused && pg_can_pause(g)) {
        if (!g->rx_off)
            dyn_aio_cancel(g->aio, pg_on_recv, g);
        g->rx_paused = 1;
        g->rx_off = 1;
    }
}

void pg_copy_settled(JSContext* ctx, pg_copy_t* c, int reject, JSValueConst v)
{
    if (c->settled)
        return;
    c->settled = 1;
    if (c->dir == 2 && !reject && !c->started) {
        JS_ThrowTypeError(ctx, "PostgreSQL.copyTo: the statement completed without COPY TO STDOUT");
        c->rejected = 1;
        c->result = JS_GetException(ctx);
    } else {
        c->rejected = reject;
        c->result = JS_DupValue(ctx, v);
    }
    if (c->dir == 2)
        copy_wake(ctx, c);
}

void pg_copy_detach(JSContext* ctx, JSRuntime* rt, pg_copy_t* c)
{
    if (c->awaiting) {
        c->awaiting = 0;
        copy_defuse_rt(rt, c);
        c->refs--;
    }
    c->g = NULL;
    c->p = NULL;
    c->stop = 1;
    if (ctx && !c->settled && c->dir == 2) {
        JSValue e = pg_conn_error(ctx, "PostgreSQL: the connection closed during COPY");
        pg_copy_settled(ctx, c, 1, e);
        JS_FreeValue(ctx, e);
    }
    copy_unref_rt(rt, c);
}

void pg_copy_end(dyn_pg_t* g, char type)
{
    dyn_pg_pending_t* p = g->head;
    pg_copy_t* c = p ? p->copy : NULL;
    g->copy_mode = 0;
    if (c && c->dir == 1 && !c->finished && !c->stop) {
        c->stop = 1;
        if (type == 'E' && c->started)
            copy_close_iter(g->ctx, c);
    }
}

static void copy_in_begin(dyn_pg_t* g, pg_copy_t* c)
{
    c->started = 1;
    if (!JS_IsObject(c->iter)) {
        c->finished = 1;
        if (copy_data(g, c->buf, c->blen) < 0 || copy_frame(g, 'c', NULL, 0) < 0) {
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
        }
        free(c->buf);
        c->buf = NULL;
        c->blen = 0;
        if (pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }
    copy_pump(g, c);
}

void pg_copy_message(dyn_pg_t* g, char type, const uint8_t* b, size_t len)
{
    dyn_pg_pending_t* p = g->head;
    pg_copy_t* c = p ? p->copy : NULL;
    switch (type) {
    case 'W':
        pg_fail_all(g, "PostgreSQL: CopyBothResponse (replication streaming) is not supported by this client");
        return;
    case 'G':
    case 'H':
        if (g->copy_mode || !p || dyn_pgw_copy_response(b, len, NULL, NULL) < 0) {
            pg_fail_all(g, "PostgreSQL: unexpected or malformed COPY response from the server");
            return;
        }
        if (type == 'G') {
            g->copy_mode = 1;
            if (c && c->dir == 1)
                copy_in_begin(g, c);
            else
                copy_send_fail(g, c ? "copyTo() was given COPY FROM STDIN; use copyFrom(sql, source)" : "COPY FROM STDIN needs PostgreSQL.copyFrom(sql, source)");
            return;
        }
        g->copy_mode = c && c->dir == 2 ? 2 : 3;
        if (g->copy_mode == 2) {
            c->started = 1;
        } else if (JS_IsUndefined(p->error)) {
            p->error = pg_conn_error(g->ctx, c ? "PostgreSQL: copyFrom() was given COPY TO STDOUT; use copyTo(sql)" : "PostgreSQL: COPY TO STDOUT needs PostgreSQL.copyTo(sql); query() does not stream it");
            if (c)
                c->stop = 1;
        }
        return;
    case 'd':
        if (g->copy_mode == 2)
            copy_deliver(g, c, b, len);
        else if (g->copy_mode != 3)
            pg_fail_all(g, "PostgreSQL: the server sent CopyData outside COPY TO STDOUT");
        return;
    default:
        if (g->copy_mode == 2 || g->copy_mode == 3)
            g->copy_mode = 4;
        else
            pg_fail_all(g, "PostgreSQL: the server sent CopyDone outside COPY TO STDOUT");
        return;
    }
}

static int copy_hwm(JSContext* ctx, JSValueConst o, pg_copy_t* c, const char* fn)
{
    static const char* const keys[] = { "highWaterMark" };
    JSValue v;
    double d;
    if (JS_IsUndefined(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "PostgreSQL.%s: options must be an object", fn);
        return -1;
    }
    if (dyn_opts_strict(ctx, o, keys, 1))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "highWaterMark");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= 1 && d <= PGC_HWM_MAX) || d != (double)(int64_t)d) {
        JS_ThrowRangeError(ctx, "PostgreSQL.%s: highWaterMark must be an integer in 1..1073741824", fn);
        return -1;
    }
    c->hwm = (uint64_t)d;
    return 0;
}

static int copy_iter_of(JSContext* ctx, JSValueConst src, pg_copy_t* c)
{
    static const char* const names[2] = { "asyncIterator", "iterator" };
    JSValue gl = JS_GetGlobalObject(ctx), sym = JS_GetPropertyStr(ctx, gl, "Symbol");
    int k;
    JS_FreeValue(ctx, gl);
    if (JS_IsException(sym))
        return -1;
    for (k = 0; k < 2; k++) {
        JSValue key = JS_GetPropertyStr(ctx, sym, names[k]), m, it, nf;
        JSAtom a;
        if (JS_IsException(key))
            goto fail;
        a = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (a == JS_ATOM_NULL)
            goto fail;
        m = JS_GetProperty(ctx, src, a);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(m))
            goto fail;
        if (JS_IsUndefined(m) || JS_IsNull(m))
            continue;
        if (!JS_IsFunction(ctx, m)) {
            JS_FreeValue(ctx, m);
            JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: the source's Symbol.%s is not a function", names[k]);
            goto fail;
        }
        it = JS_Call(ctx, m, src, 0, NULL);
        JS_FreeValue(ctx, m);
        if (JS_IsException(it))
            goto fail;
        if (!JS_IsObject(it)) {
            JS_FreeValue(ctx, it);
            JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: the source's iterator is not an object");
            goto fail;
        }
        nf = JS_GetPropertyStr(ctx, it, "next");
        if (JS_IsException(nf) || !JS_IsFunction(ctx, nf)) {
            if (!JS_IsException(nf))
                JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: the source's iterator has no next()");
            JS_FreeValue(ctx, nf);
            JS_FreeValue(ctx, it);
            goto fail;
        }
        c->iter = it;
        c->next_fn = nf;
        JS_FreeValue(ctx, sym);
        return 0;
    }
    JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: source must be a string, Uint8Array, ArrayBuffer or (async) iterable");
fail:
    JS_FreeValue(ctx, sym);
    return -1;
}

static int copy_source(JSContext* ctx, JSValueConst src, pg_copy_t* c)
{
    size_t n = 0;
    const uint8_t* b;
    if (JS_IsString(src)) {
        const char* s = JS_ToCStringLen(ctx, &n, src);
        if (!s)
            return -1;
        c->buf = (uint8_t*)malloc(n ? n : 1);
        if (c->buf)
            memcpy(c->buf, s, n);
        JS_FreeCString(ctx, s);
    } else if ((b = copy_bytes_of(ctx, src, &n)) != NULL) {
        c->buf = (uint8_t*)malloc(n ? n : 1);
        if (c->buf)
            memcpy(c->buf, b, n);
    } else {
        if (JS_HasException(ctx))
            return -1;
        if (!JS_IsObject(src)) {
            JS_ThrowTypeError(ctx, "PostgreSQL.copyFrom: source must be a string, Uint8Array, ArrayBuffer or (async) iterable");
            return -1;
        }
        return copy_iter_of(ctx, src, c);
    }
    if (!c->buf) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    c->blen = n;
    return 0;
}

static int copy_submit(JSContext* ctx, JSValueConst this_val, JSValueConst sqlv, pg_copy_t* c, JSValue* funcs)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    dyn_pg_pending_t* p;
    const char* sql;
    size_t n;
    pgw_t w;
    size_t at;
    if (!g)
        return -1;
    if (g->state == PG_ST_DEAD) {
        JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed");
        return -1;
    }
    if (g->npending + g->nwait >= g->maxpending) {
        JS_ThrowInternalError(ctx, "PostgreSQL: %d queries already in flight (maxPending)", g->maxpending);
        return -1;
    }
    if (!JS_IsString(sqlv)) {
        JS_ThrowTypeError(ctx, "PostgreSQL.%s: sql must be a string", c->dir == 1 ? "copyFrom" : "copyTo");
        return -1;
    }
    sql = JS_ToCStringLen(ctx, &n, sqlv);
    if (!sql)
        return -1;
    if (memchr(sql, 0, n)) {
        JS_FreeCString(ctx, sql);
        JS_ThrowTypeError(ctx, "PostgreSQL.%s: sql contains NUL", c->dir == 1 ? "copyFrom" : "copyTo");
        return -1;
    }
    if (c->dir == 2 && !pg_sql_is_copy(sql)) {
        JS_FreeCString(ctx, sql);
        JS_ThrowTypeError(ctx, "PostgreSQL.copyTo: sql must be a COPY ... TO STDOUT statement");
        return -1;
    }
    memset(&w, 0, sizeof w);
    at = pgw_begin(&w, 'Q');
    pgw_str(&w, sql);
    pgw_end(&w, at);
    at = pgw_begin(&w, 'S');
    pgw_end(&w, at);
    JS_FreeCString(ctx, sql);
    p = w.bad ? NULL : pgp_new();
    if (!p) {
        free(w.b);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    p->gates = 1;
    if (funcs) {
        p->resolve = funcs[0];
        p->reject = funcs[1];
    }
    c->refs++;
    p->copy = c;
    c->p = p;
    c->g = g;
    if (pg_submit(g, p, w.b, w.len, ++g->sync_seq, 1) < 0) {
        p->resolve = p->reject = JS_UNDEFINED;
        c->stop = 1;
        pgp_free(ctx, p);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (g->state == PG_ST_READY)
        pg_flush_soon(g);
    pg_hold(ctx, this_val, g);
    return 0;
}

JSValue dyn_pg_copy_from(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    pg_copy_t* c = copy_new(1);
    JSValue funcs[2], promise;
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    if (copy_hwm(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, c, "copyFrom") < 0
        || copy_source(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, c) < 0) {
        copy_unref_rt(JS_GetRuntime(ctx), c);
        return JS_EXCEPTION;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        copy_unref_rt(JS_GetRuntime(ctx), c);
        return promise;
    }
    if (copy_submit(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, c, funcs) < 0) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, promise);
        copy_unref_rt(JS_GetRuntime(ctx), c);
        return JS_EXCEPTION;
    }
    copy_unref_rt(JS_GetRuntime(ctx), c);
    return promise;
}

JSValue dyn_pg_copy_to(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    pg_copy_t* c = copy_new(2);
    JSValue obj;
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    if (copy_hwm(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, c, "copyTo") < 0) {
        copy_unref_rt(JS_GetRuntime(ctx), c);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectClass(ctx, (int)pgco_class_id);
    if (JS_IsException(obj)) {
        copy_unref_rt(JS_GetRuntime(ctx), c);
        return obj;
    }
    JS_SetOpaque(obj, c);
    if (copy_submit(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, c, NULL) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static void pgco_finalizer(JSRuntime* rt, JSValue val)
{
    pg_copy_t* c = (pg_copy_t*)JS_GetOpaque(val, pgco_class_id);
    if (c) {
        c->discard = 1;
        copy_unref_rt(rt, c);
    }
}

static void pgco_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    pg_copy_t* c = (pg_copy_t*)JS_GetOpaque(val, pgco_class_id);
    size_t i;
    if (!c)
        return;
    JS_MarkValue(rt, c->wres, mark);
    JS_MarkValue(rt, c->wrej, mark);
    JS_MarkValue(rt, c->result, mark);
    for (i = 0; i < c->qn; i++)
        JS_MarkValue(rt, c->q[(c->qh + i) % c->qcap], mark);
}

static JSValue pgco_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    pg_copy_t* c = (pg_copy_t*)JS_GetOpaque2(ctx, this_val, pgco_class_id);
    JSValue funcs[2], pr;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(c->wres))
        return JS_ThrowTypeError(ctx, "PostgreSQL.copyTo: next() called while an earlier next() is pending");
    pr = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(pr))
        return pr;
    c->wres = funcs[0];
    c->wrej = funcs[1];
    copy_wake(ctx, c);
    return pr;
}

static JSValue pgco_return(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    pg_copy_t* c = (pg_copy_t*)JS_GetOpaque2(ctx, this_val, pgco_class_id);
    JSValue funcs[2], pr, r, v;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    pr = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(pr))
        return pr;
    c->discard = 1;
    c->returned = 1;
    while (c->qn) {
        JS_FreeValue(ctx, c->q[c->qh]);
        c->qh = (c->qh + 1) % c->qcap;
        c->qn--;
    }
    c->qbytes = 0;
    copy_wake(ctx, c);
    if (c->g && c->g->rx_paused)
        pg_rx_resume(c->g);
    v = iter_result(ctx, JS_UNDEFINED, 1);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, pr);
        return JS_EXCEPTION;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, pr);
        return r;
    }
    JS_FreeValue(ctx, r);
    return pr;
}

static JSValue pgco_self(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSClassDef pgco_class = {
    "PostgreSQLCopyStream",
    .finalizer = pgco_finalizer,
    .gc_mark = pgco_mark,
};

static const JSCFunctionListEntry pgco_proto[] = {
    JS_CFUNC_DEF("next", 0, pgco_next),
    JS_CFUNC_DEF("return", 0, pgco_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, pgco_self),
};

int pg_copy_init(JSContext* ctx)
{
    JSValue cp;
    JS_NewClassID(&pgco_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), pgco_class_id, &pgco_class) < 0)
        return -1;
    cp = JS_NewObject(ctx);
    if (JS_IsException(cp))
        return -1;
    JS_SetPropertyFunctionList(ctx, cp, pgco_proto, countof(pgco_proto));
    JS_SetClassProto(ctx, pgco_class_id, cp);
    return 0;
}
#endif
