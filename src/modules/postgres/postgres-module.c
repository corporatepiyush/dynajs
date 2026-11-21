#include "postgres-internal.h"
#include <limits.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#define PG_PROTO_30 196608u
#define PG_PROTO_32 196610u
#define PG_MAX_STARTUP 10000
#define PG_DEFAULT_MAXMSG (64u * 1024u * 1024u)
#define PG_ST_CONNECTING 0
#define PG_ST_AUTH 1
#define PG_ERRS_CAP 65536
#include <sys/socket.h>
#include <arpa/inet.h>
#define PG_MAX_FLUSH 256

static const char* const pg_ctor_keys[] = {
    "tls",
    "ca",
    "host",
    "path",
    "user",
    "password",
    "database",
    "applicationName",
    "port",
    "raw",
    "bytes",
    "textResults",
    "statementCacheSize",
    "prepareAfter",
    "bigint",
    "numeric",
    "dates",
    "json",
    "arrays",
    "ranges",
    "insecureAuth",
    "channelBinding",
    "maxProtocolVersion",
    "minProtocolVersion",
    "sslNegotiation",
    "maxMessageBytes",
    "maxResultBytes",
    "maxPending",
    "queryTimeoutMs",
    "connectTimeoutMs",
    "url",
    "env",
    "hostaddr",
    "targetSessionAttrs",
    "loadBalanceHosts",
};

static void dyn_pg_teardown(dyn_pg_t* g);

_Thread_local int pg_in_final;

static void dyn_pg_finalizer(JSRuntime* rt, JSValue val)
{
    pg_in_final++;
    dyn_res_finalizer(rt, val);
    pg_in_final--;
}

static JSClassID dyn_pg_class_id;

static _Thread_local dyn_pg_t* pg_flush_pending[PG_MAX_FLUSH];

static _Thread_local int pg_n_flush;

int pgbuf_reserve(uint8_t** p, size_t* cap, size_t need)
{
    size_t c = *cap;
    uint8_t* n;
    if (need <= c)
        return 0;
    if (c == 0)
        c = 1024;
    while (c < need)
        c = c < (1u << 20) ? c * 2 : c + (c / 4);
    n = (uint8_t*)realloc(*p, c);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void pgw_raw(pgw_t* w, const void* p, size_t n)
{
    if (w->bad || pgbuf_reserve(&w->b, &w->cap, w->len + n) < 0) {
        w->bad = 1;
        return;
    }
    if (n)
        memcpy(w->b + w->len, p, n);
    w->len += n;
}

void pgw_u8(pgw_t* w, uint8_t v) { pgw_raw(w, &v, 1); }

void pgw_u32(pgw_t* w, uint32_t v)
{
    uint8_t t[4];
    put32(t, v);
    pgw_raw(w, t, 4);
}

void pgw_str(pgw_t* w, const char* s) { pgw_raw(w, s, strlen(s) + 1); }

size_t pgw_begin(pgw_t* w, char type)
{
    pgw_u8(w, (uint8_t)type);
    pgw_u32(w, 0);
    return w->len - 4;
}

void pgw_end(pgw_t* w, size_t at)
{
    if (!w->bad)
        put32(w->b + at, (uint32_t)(w->len - at));
}

static void pg_client_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);
static void pg_drop_queued(dyn_pg_t* g);

static JSValue pg_release_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static void pg_client_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_pg_t* g = (dyn_pg_t*)opaque;
    JSValue s;

    (void)ctx;
    if (!g)
        return;
    pg_in_final++;
    pg_drop_queued(g);
    pg_in_final--;
    if (!g->self_held)
        return;
    s = g->self;
    g->self = JS_UNDEFINED;
    g->self_held = 0;
    JS_FreeValueRT(rt, s);
}

void pg_hold(JSContext* ctx, JSValueConst obj, dyn_pg_t* g)
{
    if (g->self_held)
        return;
    g->self = JS_DupValue(ctx, obj);
    g->self_held = 1;
    JS_AddShutdownSweep(g->rt, pg_client_sweep, g);
}

void pg_release_if_idle(dyn_pg_t* g)
{
    JSValue s;

    if (!g->self_held || g->npending != 0 || g->nwait != 0)
        return;
    s = g->self;
    g->self = JS_UNDEFINED;
    g->self_held = 0;
    JS_RemoveShutdownSweep(g->rt, pg_client_sweep, g);
    if (JS_EnqueueJob(g->ctx, pg_release_job, 1,
            (JSValueConst*)&s)
        < 0) {
        g->self = s;
        g->self_held = 1;
        JS_AddShutdownSweep(g->rt, pg_client_sweep, g);
        return;
    }
    JS_FreeValue(g->ctx, s);
}

JSValue pg_conn_error(JSContext* ctx, const char* msg)
{
    JSValue e = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, "CONNECTION"),
        JS_PROP_C_W_E);
    return e;
}

int pg_flush(dyn_pg_t* g)
{
    int rc;
    if (g->olen == 0 || g->state == PG_ST_CONNECTING || g->state == PG_ST_DEAD)
        return 0;
    rc = dyn_aio_send(g->aio, g->fd, g->obuf, g->olen, 0, NULL, NULL);
    if (rc < 0)
        return rc;
    g->olen = 0;
    return 0;
}

static void pg_flush_drop(dyn_pg_t* g)
{
    int i;
    g->flush_queued = 0;
    for (i = 0; i < pg_n_flush; i++)
        if (pg_flush_pending[i] == g) {
            pg_flush_pending[i] = pg_flush_pending[pg_n_flush - 1];
            pg_n_flush--;
            return;
        }
}

static JSValue pg_flush_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    while (pg_n_flush > 0) {
        dyn_pg_t* g = pg_flush_pending[0];
        g->cb_depth++;
        pg_flush_drop(g);
        if (g->state == PG_ST_READY && pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        pg_cb_exit(g);
    }
    return JS_UNDEFINED;
}

void pg_flush_soon(dyn_pg_t* g)
{
    if (g->flush_queued || g->olen == 0 || g->state != PG_ST_READY)
        return;
    if (pg_n_flush >= PG_MAX_FLUSH || JS_EnqueueJob(g->ctx, pg_flush_job, 0, NULL) < 0) {
        if (pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }
    g->flush_queued = 1;
    pg_flush_pending[pg_n_flush++] = g;
}

int pg_write(dyn_pg_t* g, const uint8_t* b, size_t n)
{
    if (pgbuf_reserve(&g->obuf, &g->ocap, g->olen + n) < 0)
        return -1;
    memcpy(g->obuf + g->olen, b, n);
    g->olen += n;
    return 0;
}

void pg_free_params(JSContext* ctx, const char** pv, const uint8_t* pf,
    uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        if (!pv[k])
            continue;
        if (pf[k] == PG_PF_CSTR)
            JS_FreeCString(ctx, pv[k]);
        else if (pf[k] == PG_PF_MALLOC)
            free(DYN_UNCONST(pv[k]));
    }
}

void pg_call1(dyn_pg_t* g, JSValue fn, JSValue arg)
{
    JSContext* ctx = g->ctx;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { arg };
        g->cb_depth++;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        g->cb_depth--;
        if (JS_IsException(r))
            dyn_exc_drop(ctx);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arg);
}

int pg_gone(dyn_pg_t* g)
{
    return g->closing || g->state == PG_ST_DEAD;
}

void pg_cb_exit(dyn_pg_t* g)
{
    if (g->cb_depth > 0)
        g->cb_depth--;
    if (g->cb_depth == 0 && g->closing)
        dyn_pg_teardown(g);
}

void pg_fail_all(dyn_pg_t* g, const char* msg)
{
    JSContext* ctx = g->ctx;
    dyn_pg_pending_t* p;

    if (g->state == PG_ST_DEAD)
        return;
    g->state = PG_ST_DEAD;
    if (g->fd >= 0) {
        dyn_aio_close(g->aio, g->fd);
        g->fd = -1;
    }
    while ((p = pgp_pop(&g->head, &g->tail)) != NULL) {
        g->npending--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    while ((p = pgp_pop(&g->dhead, &g->dtail)) != NULL) {
        g->npending--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
        g->nwait--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    pg_release_if_idle(g);
    if (JS_IsFunction(ctx, g->h_error))
        pg_call1(g, g->h_error, pg_conn_error(ctx, msg));
    if (g->hooked) {
        dyn_net_off_drain(g);
        g->hooked = 0;
    }
    if (g->aio && !g->released) {
        g->released = 1;
        dyn_net_reactor_release(g->ctx);
    }
}

int pg_send_startup(dyn_pg_t* g)
{
    pgw_t w;
    int rc;
    memset(&w, 0, sizeof(w));
    pgw_u32(&w, 0);
    pgw_u32(&w, g->max_minor >= 2 ? PG_PROTO_32 : PG_PROTO_30);
    pgw_str(&w, "user");
    pgw_str(&w, g->user ? g->user : "postgres");
    if (g->database) {
        pgw_str(&w, "database");
        pgw_str(&w, g->database);
    }
    pgw_str(&w, "client_encoding");
    pgw_str(&w, "UTF8");
    if (g->appname) {
        pgw_str(&w, "application_name");
        pgw_str(&w, g->appname);
    }
    pgw_u8(&w, 0);
    if (w.bad) {
        free(w.b);
        return -1;
    }
    if (w.len > PG_MAX_STARTUP) {
        free(w.b);
        return -1;
    }
    put32(w.b, (uint32_t)w.len);
    rc = pg_write(g, w.b, w.len);
    free(w.b);
    return rc;
}

static void pg_on_connect_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (g->state == PG_ST_DEAD)
        return;
    if (res < 0) {
        pg_try_next(g, res > -4096 ? strerror(-res) : "connect failed");
        pg_gone(g);
        return;
    }
    g->state = PG_ST_AUTH;
#ifdef CONFIG_TLS
    if (g->use_tls && g->ssl_direct) {
        if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0) {
            pg_fail_all(g, "PostgreSQL: cannot read from the socket");
            pg_gone(g);
        } else if (pg_start_tls(g) < 0) {
            pg_gone(g);
        }
        return;
    }
    if (g->use_tls) {
        static const uint8_t ssl_request[8] = { 0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f };
        g->ssl_wait = 1;
        if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0
            || dyn_aio_send(g->aio, g->fd, ssl_request, sizeof(ssl_request), 0, NULL, NULL) < 0) {
            pg_fail_all(g, "PostgreSQL: cannot send the SSLRequest");
            pg_gone(g);
        }
        return;
    }
#endif
    if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0) {
        pg_fail_all(g, "PostgreSQL: cannot read from the socket");
        return;
    }
    if (pg_send_startup(g) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the startup message");
}

static void pg_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;

    g->cb_depth++;
    pg_on_connect_inner(aio, res, buf, len, ud);
    pg_cb_exit(g);
}

static void pg_connect_now(dyn_pg_t* g);

static void pg_attempt(dyn_pg_t* g)
{
    int k;
    g->state = PG_ST_CONNECTING;
    g->connect_deadline_ms = g->connect_ms ? dyn_timer_now_ms() + g->connect_ms : 0;
    k = pg_pgpass_prep(g);
    if (k < 0)
        pg_fail_all(g, "PostgreSQL: out of memory");
    else if (k > 0)
        pg_pgpass_start(g);
    else
        pg_connect_now(g);
}

static void pg_note(dyn_pg_t* g, const char* why)
{
    char head[400];
    size_t wl = strlen(why), add, at, pre;
    char* e;
    int n = g->path ? snprintf(head, sizeof head, "connection to server on socket \"%.300s\" failed: ", g->path)
                    : snprintf(head, sizeof head, "connection to server at \"%.300s\", port %u failed: ",
                          g->addr_cur ? g->addr_cur : g->host ? g->host : "127.0.0.1", (unsigned)g->port);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof head)
        n = (int)sizeof head - 1;
    pre = g->errs ? 1 : 12;
    add = pre + (size_t)n + wl;
    if (g->errs_len + add > PG_ERRS_CAP)
        return;
    e = (char*)realloc(g->errs, g->errs_len + add + 1);
    if (!e)
        return;
    at = g->errs_len;
    memcpy(e + at, pre == 1 ? "\n" : "PostgreSQL: ", pre);
    at += pre;
    memcpy(e + at, head, (size_t)n);
    memcpy(e + at + (size_t)n, why, wl);
    g->errs_len += add;
    e[g->errs_len] = 0;
    g->errs = e;
}

static void pg_reset_attempt(dyn_pg_t* g)
{
    JSValue o;
    if (g->fd >= 0) {
        dyn_aio_close(g->aio, g->fd);
        g->fd = -1;
    }
#ifdef CONFIG_TLS
    g->tls_conn = NULL;
#endif
    pg_scram_job_orphan(g);
    dyn_scram_free(&g->scram);
    g->scram_live = g->scram_done = 0;
    pg_pgpass_job_orphan(g);
    g->rlen = g->rpos = g->olen = 0;
    g->ssl_wait = g->npv_seen = g->auth_seen = g->auth_ok_seen = g->ep_len = 0;
    g->proto_minor = 0;
    g->backend_pid = 0;
    g->cancel_key_len = 0;
    g->tx_status = 'I';
    g->gen++;
    g->chk = g->chk_ok = g->chk_err = g->chk_vn = 0;
    g->chk_rows = 0;
    g->ro_on = g->hs_on = 0;
    g->sv_major = 0;
    o = JS_NewObject(g->ctx);
    if (!JS_IsException(o)) {
        JS_FreeValue(g->ctx, g->params);
        g->params = o;
        g->nparams = 0;
    }
}

void pg_try_next(dyn_pg_t* g, const char* why)
{
    pg_note(g, why);
    if (g->state != PG_ST_DEAD && g->hi + 1 >= g->nh && g->tsa == PG_TSA_PREFER && !g->tsa_pass2) {
        g->tsa_pass2 = 1;
        g->hi = -1;
    }
    if (g->state == PG_ST_DEAD || g->hi + 1 >= g->nh) {
        if (g->errs)
            pg_fail_all(g, g->errs);
        else {
            char m[512];
            snprintf(m, sizeof m, "PostgreSQL: %s", why);
            pg_fail_all(g, m);
        }
        return;
    }
    pg_reset_attempt(g);
    if (pg_host_load(g, g->hi + 1) < 0) {
        pg_fail_all(g, "PostgreSQL: out of memory");
        return;
    }
    pg_attempt(g);
}

static void pg_connect_now(dyn_pg_t* g)
{
    const char* h = g->addr_cur ? g->addr_cur : g->host ? g->host : "127.0.0.1";
    unsigned char ab[16];
    char m[128];
    if (g->addr_cur && inet_pton(AF_INET, g->addr_cur, ab) != 1 && inet_pton(AF_INET6, g->addr_cur, ab) != 1) {
        snprintf(m, sizeof m, "could not parse network address \"%.64s\"", g->addr_cur);
        pg_try_next(g, m);
        return;
    }
    g->fd = g->path
        ? dyn_aio_unix_connect(g->aio, g->path, pg_on_connect, g)
        : dyn_aio_connect(g->aio, h, g->port, pg_on_connect, g);
    if (g->fd < 0)
        pg_try_next(g, strerror(errno));
}

void pg_after_pgpass(dyn_pg_t* g, int rc)
{
    g->cb_depth++;
    if (g->state == PG_ST_DEAD) {
    } else if (rc < 0) {
        pg_fail_all(g, "PostgreSQL: out of memory");
    } else {
        pg_connect_now(g);
    }
    pg_cb_exit(g);
}

static void pg_tick_inner(void* udata)
{
    dyn_pg_t* g = (dyn_pg_t*)udata;
    uint64_t now;
    if (pg_gone(g))
        return;
    now = dyn_timer_now_ms();
    if (g->connect_deadline_ms && now >= g->connect_deadline_ms) {
        pg_try_next(g, "connect timed out");
        return;
    }
    if (g->head && g->head->deadline_ms && now >= g->head->deadline_ms)
        pg_fail_all(g, "PostgreSQL: query timed out");
    else if (g->head && g->head->copy)
        pg_copy_tick(g);
}

static void pg_tick(void* udata)
{
    dyn_pg_t* g = (dyn_pg_t*)udata;

    g->cb_depth++;
    pg_tick_inner(udata);
    pg_cb_exit(g);
}

JSClassID pgcps_class_id;

JSClassID pgq_class_id;

static const JSClassDef pgq_class = {
    "PgRowStream",
    .finalizer = pgq_iter_finalizer,
    .gc_mark = pgq_iter_mark,
};

static const JSClassDef pgcps_class = {
    "PgContinuation",
};

static const JSCFunctionListEntry pgq_proto[] = {
    JS_CFUNC_DEF("next", 0, pgq_next),
    JS_CFUNC_DEF("return", 0, pgq_return),
    JS_CFUNC_DEF("throw", 1, pgq_throw),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, pgq_async_iterator),
};

dyn_pg_t* pg_this(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_pg_t*)dyn_res_native(ctx, this_val, dyn_pg_class_id);
}

int pg_submit(dyn_pg_t* g, dyn_pg_pending_t* p, uint8_t* bytes,
    size_t nbytes, uint64_t esync, int sync_end)
{
    p->bytes = bytes;
    p->nbytes = nbytes;
    p->esync = esync;
    p->sync_end = sync_end;
    if (g->state == PG_ST_READY && !g->copy_gate) {
        if (pg_write(g, bytes, nbytes) < 0)
            return -1;
        free(bytes);
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
    } else {
        pgp_push(&g->wq_head, &g->wq_tail, p);
        g->nwait++;
    }
    return 0;
}

static JSValue dyn_pg_on(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    const char* ev;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, handler)");
    ev = JS_ToCString(ctx, argv[0]);
    if (!ev)
        return JS_EXCEPTION;
    g = pg_this(ctx, this_val);
    if (!g) {
        JS_FreeCString(ctx, ev);
        return JS_EXCEPTION;
    }
    if (strcmp(ev, "notice") == 0) {
        JS_FreeValue(ctx, g->h_notice);
        g->h_notice = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "notification") == 0) {
        JS_FreeValue(ctx, g->h_notify);
        g->h_notify = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "error") == 0) {
        JS_FreeValue(ctx, g->h_error);
        g->h_error = JS_DupValue(ctx, argv[1]);
    } else {
        JS_FreeCString(ctx, ev);
        return JS_ThrowRangeError(ctx,
            "on: want 'notice', 'notification' or 'error'");
    }
    JS_FreeCString(ctx, ev);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_pg_get_pending(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->npending + g->nwait) : JS_EXCEPTION;
}

static JSValue dyn_pg_get_pid(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewUint32(ctx, g->backend_pid) : JS_EXCEPTION;
}

static JSValue dyn_pg_get_tx(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    char t[2];
    if (!g)
        return JS_EXCEPTION;
    t[0] = g->tx_status ? g->tx_status : 'I';
    t[1] = '\0';
    return JS_NewString(ctx, t);
}

static JSValue dyn_pg_get_params(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_DupValue(ctx, g->params) : JS_EXCEPTION;
}

static void dyn_pg_teardown(dyn_pg_t* g);

static void dyn_pg_dispose(void* native)
{
    dyn_pg_t* g = (dyn_pg_t*)native;

    if (!g)
        return;
    if (g->cb_depth) {
        g->closing = 1;
        return;
    }
    dyn_pg_teardown(g);
}

static void pg_close_out(dyn_pg_t* g, dyn_pg_pending_t* p)
{
    if (pg_in_final) {
        pgp_free_rt(g->rt, p);
        return;
    }
    pg_settle(g, p, 1, pg_conn_error(g->ctx, "PostgreSQL: client closed"));
}

static void pg_drop_queued(dyn_pg_t* g)
{
    dyn_pg_pending_t* p;
    while ((p = pgp_pop(&g->head, &g->tail)) != NULL) {
        if (p->batch)
            pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
        pg_close_out(g, p);
    }
    while ((p = pgp_pop(&g->dhead, &g->dtail)) != NULL) {
        if (p->batch)
            pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
        pg_close_out(g, p);
    }
    while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
        if (p->batch)
            pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
        pg_close_out(g, p);
    }
}

static void dyn_pg_teardown(dyn_pg_t* g)
{
    pg_flush_drop(g);
    pg_stmt_clear(g);
    if (g->hooked)
        dyn_net_off_drain(g);
    if (g->state == PG_ST_READY && g->fd >= 0) {
        static const uint8_t bye[5] = { 'X', 0, 0, 0, 4 };
        (void)dyn_aio_send(g->aio, g->fd, bye, sizeof(bye), 0, NULL, NULL);
    }
    if (g->state != PG_ST_DEAD) {
        g->state = PG_ST_DEAD;
        pg_drop_queued(g);
    }
    if (g->aio && !g->released) {
        if (g->fd >= 0)
            dyn_aio_close(g->aio, g->fd);
        dyn_net_reactor_release_rt(g->rt);
    }
    if (g->self_held) {
        JSValue s = g->self;
        g->self = JS_UNDEFINED;
        g->self_held = 0;
        JS_RemoveShutdownSweep(g->rt, pg_client_sweep, g);
        JS_FreeValueRT(g->rt, s);
    }
    pg_scram_job_orphan(g);
    pg_pgpass_job_orphan(g);
    dyn_scram_free(&g->scram);
    JS_FreeValueRT(g->rt, g->params);
    JS_FreeValueRT(g->rt, g->h_notice);
    JS_FreeValueRT(g->rt, g->h_notify);
    JS_FreeValueRT(g->rt, g->h_error);
    JS_FreeValueRT(g->rt, g->numeric_ctor);
    g->numeric_ctor = JS_UNDEFINED;
    if (g->pass) {
        memset(g->pass, 0, strlen(g->pass));
        free(g->pass);
    }
    free(g->host);
    free(g->path);
    pg_hosts_free(g);
    free(g->hostaddr);
    free(g->pp_file);
    free(g->tsa_s);
    free(g->lb_s);
    free(g->errs);
    free(g->user);
    free(g->database);
    free(g->appname);
    free(g->rbuf);
    free(g->obuf);
#ifdef CONFIG_TLS
    if (g->tls_ctx)
        dyn_tls_ctx_free(g->tls_ctx);
    free(g->tls_ca);
#endif
    memset(g, 0, sizeof(*g));
    free(g);
}

static int pg_opt_str(JSContext* ctx, JSValueConst o, const char* k, char** out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char* s;
    size_t n = 0;
    *out = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    s = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (memchr(s, 0, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "PostgreSQL: %s contains a NUL byte", k);
        return -1;
    }
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int pg_opt_int(JSContext* ctx, JSValueConst o, const char* k, int dflt, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n;
    int rc;
    *out = dflt;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    rc = JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (rc < 0)
        return -1;
    *out = (int)n;
    return 0;
}

static int pg_opt_bool(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    *out = dyn_get_truthy(ctx, o, k);
    return *out < 0 ? -1 : 0;
}

static int pg_opt_enum(JSContext* ctx, JSValueConst o, const char* key, const char* const* names,
    int n, const char* msg, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    const char* s;
    size_t sl = 0;
    int i;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s", msg);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &sl, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < n; i++)
        if (sl == strlen(names[i]) && !memcmp(s, names[i], sl))
            break;
    JS_FreeCString(ctx, s);
    if (i == n) {
        JS_ThrowTypeError(ctx, "%s", msg);
        return -1;
    }
    *out = i;
    return 0;
}

static int pg_opt_decode(JSContext* ctx, JSValueConst o, dyn_pg_t* g)
{
    static const char* const dn[2] = { "string", "date" };
    static const char* const nn[1] = { "string" };
    int ns = 0;
    JSValue v = JS_GetPropertyStr(ctx, o, "numeric");
    if (JS_IsException(v))
        return -1;
    if (JS_IsFunction(ctx, v) && JS_IsConstructor(ctx, v)) {
        JS_FreeValue(ctx, g->numeric_ctor);
        g->numeric_ctor = v;
    } else {
        JS_FreeValue(ctx, v);
        if (pg_opt_enum(ctx, o, "numeric", nn, 1, "PostgreSQL: numeric must be 'string' or a constructor such as Decimal from dyna:decimal", &ns) < 0)
            return -1;
    }
    static const char* const jn[2] = { "string", "parse" };
    static const char* const an[2] = { "string", "array" };
    static const char* const rn[2] = { "string", "object" };
    if (pg_opt_enum(ctx, o, "dates", dn, 2, "PostgreSQL: dates must be 'string' or 'date'", &g->dates_native) < 0
        || pg_opt_enum(ctx, o, "json", jn, 2, "PostgreSQL: json must be 'string' or 'parse'", &g->json_parse) < 0)
        return -1;
    if (pg_opt_enum(ctx, o, "arrays", an, 2, "PostgreSQL: arrays must be 'string' or 'array'", &g->arrays_native) < 0)
        return -1;
    return pg_opt_enum(ctx, o, "ranges", rn, 2, "PostgreSQL: ranges must be 'string' or 'object'", &g->ranges_obj);
}

static int pg_opt_proto(JSContext* ctx, JSValueConst o, dyn_pg_t* g)
{
    static const char* const cb[3] = { "prefer", "disable", "require" };
    static const char* const pv[3] = { "3.0", "3.1", "3.2" };
    static const char* const sn[2] = { "postgres", "direct" };
    int mx = -1, mn = -1;
    if (pg_opt_enum(ctx, o, "channelBinding", cb, 3, "PostgreSQL: channelBinding must be 'disable', 'prefer' or 'require'", &g->chan_bind) < 0
        || pg_opt_enum(ctx, o, "maxProtocolVersion", pv, 3, "PostgreSQL: maxProtocolVersion must be '3.0' or '3.2'", &mx) < 0
        || pg_opt_enum(ctx, o, "minProtocolVersion", pv, 3, "PostgreSQL: minProtocolVersion must be '3.0' or '3.2'", &mn) < 0
        || pg_opt_enum(ctx, o, "sslNegotiation", sn, 2, "PostgreSQL: sslNegotiation must be 'postgres' or 'direct'", &g->ssl_direct) < 0)
        return -1;
    if (mx == 1 || mn == 1) {
        JS_ThrowTypeError(ctx, "PostgreSQL: %s must be '3.0' or '3.2'", mx == 1 ? "maxProtocolVersion" : "minProtocolVersion");
        return -1;
    }
    if (mx >= 0)
        g->max_minor = mx;
    if (mn >= 0)
        g->min_minor = mn;
    if (mx < 0 && g->min_minor > g->max_minor)
        g->max_minor = g->min_minor;
    if (g->min_minor > g->max_minor) {
        JS_ThrowRangeError(ctx, "PostgreSQL: minProtocolVersion is above maxProtocolVersion");
        return -1;
    }
    g->proto_minor = g->max_minor;
    return 0;
}

static int pg_ctor_nums(JSContext* ctx, JSValueConst opt, dyn_pg_t* g, pg_ci_t* ci)
{
    int pi, text, sc, pa, mr, mm, mp, qt, ct;
    if (pg_opt_int(ctx, opt, "port", INT_MIN, &pi) < 0)
        return -1;
    if (pi != INT_MIN) {
        if ((pi < 1 || pi > 65535) && !g->path) {
            JS_ThrowRangeError(ctx, "PostgreSQL: port must be 1..65535");
            return -1;
        }
        g->port = (uint16_t)(pi > 0 ? pi : 0);
        ci->port = g->port;
    }
    if (pg_opt_bool(ctx, opt, "raw", &g->raw) < 0 || pg_opt_bool(ctx, opt, "bytes", &g->bytes_out) < 0
        || pg_opt_bool(ctx, opt, "textResults", &text) < 0
        || pg_opt_int(ctx, opt, "statementCacheSize", -1, &sc) < 0
        || pg_opt_int(ctx, opt, "prepareAfter", 0, &pa) < 0
        || pg_opt_bool(ctx, opt, "bigint", &g->bigint) < 0
        || pg_opt_bool(ctx, opt, "insecureAuth", &g->insecure_auth) < 0
        || pg_opt_proto(ctx, opt, g) < 0
        || pg_opt_decode(ctx, opt, g) < 0
        || pg_opt_int(ctx, opt, "maxResultBytes", 0, &mr) < 0
        || pg_opt_int(ctx, opt, "maxMessageBytes", 0, &mm) < 0
        || pg_opt_int(ctx, opt, "maxPending", 0, &mp) < 0
        || pg_opt_int(ctx, opt, "queryTimeoutMs", 0, &qt) < 0
        || pg_opt_int(ctx, opt, "connectTimeoutMs", INT_MIN, &ct) < 0)
        return -1;
    g->binary_results = !text;
    if (sc >= 0)
        g->stmt_cache_max = sc;
    if (pa > 0)
        g->prepare_after = pa;
    if (mr > 0)
        g->max_result = (uint64_t)mr;
    if (mm > 0)
        g->maxmsg = (size_t)mm;
    if (mp > 0)
        g->maxpending = mp;
    if (qt > 0)
        g->query_timeout_ms = (uint64_t)qt;
    if (ct != INT_MIN)
        ci->ct = ct > 0 ? ct : 0;
    if (ct > 0)
        g->connect_deadline_ms = dyn_timer_now_ms() + (g->connect_ms = (uint64_t)ct);
    return 0;
}

static JSValue dyn_pg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    JSValueConst opt = argc > 0 ? argv[0] : JS_UNDEFINED, url = JS_UNDEFINED;
    JSValue urlv = JS_UNDEFINED;
    pg_ci_t ci = { -1, -1, -1 };
    int tls_b = 0;
    int use_env = 1;

    if (argc > 0 && JS_IsString(argv[0])) {
        url = argv[0];
        opt = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (!JS_IsUndefined(opt) && !JS_IsObject(opt))
            return JS_ThrowTypeError(ctx, "PostgreSQL: expects an options object after the url");
    } else if (argc > 0 && !JS_IsObject(opt)) {
        return JS_ThrowTypeError(ctx, "PostgreSQL: expects an options object or a url");
    }
    if (JS_IsObject(opt)) {
        JSValue tv = JS_GetPropertyStr(ctx, opt, "tls");
        if (JS_IsException(tv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) {
            tls_b = JS_ToBool(ctx, tv);
            ci.tls = tls_b;
        }
        JS_FreeValue(ctx, tv);
        if (tls_b < 0)
            return JS_EXCEPTION;
    }
#ifndef CONFIG_TLS
    if (tls_b)
        return dyn_tls_missing(ctx, "PostgreSQL", "tls");
#endif

    g = (dyn_pg_t*)calloc(1, sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->ctx = ctx;
    g->rt = JS_GetRuntime(ctx);
    g->fd = -1;
    g->state = PG_ST_CONNECTING;
#ifdef CONFIG_TLS
    if (JS_IsObject(opt)) {
        g->use_tls = tls_b;
        if (g->use_tls) {
            if (pg_opt_str(ctx, opt, "ca", &g->tls_ca)) {
                free(g);
                return JS_EXCEPTION;
            }
        }
    }
#endif
    g->maxmsg = PG_DEFAULT_MAXMSG;
    g->maxpending = 1024;
    g->stmt_cache_max = 64;
    g->max_result = (uint64_t)1 << 30;
    g->prepare_after = 2;
    g->binary_results = 1;
    g->tx_status = 'I';
    g->params = JS_NewObject(ctx);
    g->h_notice = g->h_notify = g->h_error = JS_UNDEFINED;
    g->numeric_ctor = JS_UNDEFINED;
    g->self = JS_UNDEFINED;
    g->port = 5432;
    g->ssl_direct = -1;

    if (JS_IsObject(opt)) {
        if (dyn_opts_strict(ctx, opt, pg_ctor_keys, (int)(sizeof(pg_ctor_keys) / sizeof(pg_ctor_keys[0])))) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
        }
        if (pg_opt_str(ctx, opt, "host", &g->host) || pg_opt_str(ctx, opt, "path", &g->path) || pg_opt_str(ctx, opt, "hostaddr", &g->hostaddr)
            || pg_opt_str(ctx, opt, "targetSessionAttrs", &g->tsa_s) || pg_opt_str(ctx, opt, "loadBalanceHosts", &g->lb_s)
            || pg_opt_str(ctx, opt, "user", &g->user) || pg_opt_str(ctx, opt, "password", &g->pass)
            || pg_opt_str(ctx, opt, "database", &g->database)
            || pg_opt_str(ctx, opt, "applicationName", &g->appname)) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
        }
        if (pg_ctor_nums(ctx, opt, g, &ci) < 0 || JS_HasException(ctx)) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
        }
        urlv = JS_GetPropertyStr(ctx, opt, "url");
        if (JS_IsException(urlv)) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(urlv) && !JS_IsNull(urlv) && !JS_IsUndefined(url)) {
            JS_FreeValue(ctx, urlv);
            dyn_pg_dispose(g);
            return JS_ThrowTypeError(ctx, "PostgreSQL: url given twice");
        }
        if (JS_IsUndefined(url))
            url = urlv;
        {
            JSValue ev = JS_GetPropertyStr(ctx, opt, "env");
            if (JS_IsException(ev)) {
                JS_FreeValue(ctx, urlv);
                dyn_pg_dispose(g);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(ev) && !JS_IsNull(ev))
                use_env = JS_ToBool(ctx, ev) > 0;
            JS_FreeValue(ctx, ev);
        }
    }
    if (pg_conninfo(ctx, g, url, use_env, &ci) < 0) {
        JS_FreeValue(ctx, urlv);
        dyn_pg_dispose(g);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, urlv);

    g->aio = dyn_net_reactor_acquire(ctx);
    if (!g->aio) {
        dyn_pg_dispose(g);
        return dyn_net_throw_no_reactor(ctx, "PostgreSQL");
    }
    if (g->pgpass_job || g->nh > 1 || g->addr_cur) {
        if (dyn_net_on_drain(pg_tick, g) == 0)
            g->hooked = 1;
        if (g->pgpass_job)
            pg_pgpass_start(g);
        else
            pg_connect_now(g);
        goto wrap;
    }
    g->fd = g->path
        ? dyn_aio_unix_connect(g->aio, g->path, pg_on_connect, g)
        : dyn_aio_connect(g->aio, g->host ? g->host : "127.0.0.1",
              g->port, pg_on_connect, g);
    if (g->fd < 0) {
        JSValue e = JS_ThrowInternalError(ctx, "PostgreSQL: connect: %s",
            strerror(errno));
        dyn_pg_dispose(g);
        return e;
    }
    if (dyn_net_on_drain(pg_tick, g) == 0)
        g->hooked = 1;
wrap:
    {
        JSValue obj = dyn_res_wrap(ctx, new_target, dyn_pg_class_id, g,
            dyn_pg_dispose);
        return obj;
    }
}

static void dyn_pg_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_pg_class_id);
    dyn_pg_t* g;

    if (!res || res->closed || !res->native)
        return;
    g = (dyn_pg_t*)res->native;
    JS_MarkValue(rt, g->params, mark_func);
    JS_MarkValue(rt, g->h_notice, mark_func);
    JS_MarkValue(rt, g->numeric_ctor, mark_func);
    JS_MarkValue(rt, g->h_notify, mark_func);
    JS_MarkValue(rt, g->h_error, mark_func);
}

static const JSClassDef dyn_pg_class = {
    "PostgreSQL",
    .finalizer = dyn_pg_finalizer,
    .gc_mark = dyn_pg_gc_mark,
};

static const JSCFunctionListEntry dyn_pg_proto[] = {
    JS_CFUNC_DEF("query", 1, dyn_pg_query),
    JS_CFUNC_DEF("copyFrom", 2, dyn_pg_copy_from),
    JS_CFUNC_DEF("copyTo", 1, dyn_pg_copy_to),
    JS_CFUNC_DEF("queryIter", 1, dyn_pg_queryiter),
    JS_CFUNC_DEF("pipeline", 1, dyn_pg_pipeline),
    JS_CFUNC_DEF("cancel", 0, dyn_pg_cancel),
    JS_CFUNC_DEF("on", 2, dyn_pg_on),
    JS_CGETSET_DEF("ready", dyn_pg_get_ready, NULL),
    JS_CGETSET_DEF("protocolVersion", dyn_pg_get_proto, NULL),
    JS_CGETSET_DEF("statementCache", dyn_pg_get_stmt_stats, NULL),
    JS_CGETSET_DEF("pending", dyn_pg_get_pending, NULL),
    JS_CGETSET_DEF("backendPid", dyn_pg_get_pid, NULL),
    JS_CGETSET_DEF("transactionStatus", dyn_pg_get_tx, NULL),
    JS_CGETSET_DEF("parameters", dyn_pg_get_params, NULL),
};

static int dyn_pg_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_pg_class_id, &dyn_pg_class,
            dyn_pg_proto, countof(dyn_pg_proto),
            dyn_pg_ctor, "PostgreSQL")
        < 0)
        return -1;
    JS_NewClassID(&pgq_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), pgq_class_id, &pgq_class) < 0)
        return -1;
    {
        JSValue cp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, cp, pgq_proto, countof(pgq_proto));
        JS_SetClassProto(ctx, pgq_class_id, cp);
    }
    if (pg_copy_init(ctx) < 0)
        return -1;
    JS_NewClassID(&pgcps_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), pgcps_class_id, &pgcps_class) < 0)
        return -1;
    {
        JSValue cp = JS_NewObject(ctx);
        JS_SetClassProto(ctx, pgcps_class_id, cp);
    }
    return 0;
}

static void dyn_pg_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "PostgreSQL");
}

int js_nat_init_postgres(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:postgres", dyn_pg_register);
    if (!m)
        return -1;
    dyn_pg_add_exports(ctx, m);
    return 0;
}

#endif
