#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#define PG_CANCEL_CODE 80877102u

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    int fd, done, hooked, tls, direct, ssl_wait, via_path;
    uint64_t deadline_ms;
    char* host;
    char* ca;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tctx;
    dyn_tls_conn_t* t;
#endif
    uint8_t msg[12 + PG_MAX_CANCEL_KEY];
    size_t len;
} pg_cancel_t;

static void pg_cancel_tick(void* udata);

static void pg_cancel_finish(pg_cancel_t* c)
{
    if (c->done)
        return;
    c->done = 1;
    if (c->hooked)
        dyn_net_off_drain(c);
    if (c->fd >= 0)
        dyn_aio_close(c->aio, c->fd);
#ifdef CONFIG_TLS
    if (c->tctx)
        dyn_tls_ctx_free(c->tctx);
#endif
    free(c->host);
    free(c->ca);
    dyn_net_reactor_release(c->ctx);
    free(c);
}

#ifdef CONFIG_TLS
static void pg_cancel_hs(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    pg_cancel_t* c = (pg_cancel_t*)ud;
    char ab[64];
    const char* sel;
    int has;
    (void)aio;
    (void)buf;
    (void)len;
    if (res < 0) {
        pg_cancel_finish(c);
        return;
    }
    sel = dyn_tls_alpn_selected(c->t, ab, sizeof ab);
    has = sel && *sel;
    c->t = NULL;
    if ((has ? strcmp(sel, "postgresql") != 0 : c->direct)
        || dyn_aio_send(c->aio, c->fd, c->msg, c->len, 0, NULL, NULL) < 0)
        pg_cancel_finish(c);
}

static int pg_cancel_tls(pg_cancel_t* c)
{
    dyn_tls_opts_t to;
    char terr[192];
    memset(&to, 0, sizeof to);
    to.ca_file = c->ca;
    to.alpn = "postgresql";
    to.insecure = c->via_path;
    c->tctx = dyn_tls_ctx_client(&to, terr, sizeof terr);
    if (!c->tctx)
        return -1;
    c->t = dyn_tls_conn_new(c->tctx, c->via_path ? NULL : c->host, terr, sizeof terr);
    if (!c->t)
        return -1;
    if (dyn_aio_tls_attach(c->aio, c->fd, c->t, pg_cancel_hs, c) < 0) {
        dyn_tls_conn_free(c->t);
        c->t = NULL;
        return -1;
    }
    return dyn_aio_tls_start(c->aio, c->fd) < 0 ? -1 : 0;
}
#endif

static void pg_cancel_rx(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    pg_cancel_t* c = (pg_cancel_t*)ud;
    (void)aio;
#ifdef CONFIG_TLS
    if (c->ssl_wait) {
        c->ssl_wait = 0;
        if (res < 0 || len != 1 || buf[0] != 'S' || pg_cancel_tls(c) < 0)
            pg_cancel_finish(c);
        return;
    }
#endif
    (void)res;
    (void)buf;
    (void)len;
    pg_cancel_finish(c);
}

static void pg_cancel_tick(void* udata)
{
    pg_cancel_t* c = (pg_cancel_t*)udata;
    if (!c->done && c->deadline_ms && dyn_timer_now_ms() >= c->deadline_ms)
        pg_cancel_finish(c);
}

static void pg_cancel_connected(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    pg_cancel_t* c = (pg_cancel_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (res < 0) {
        pg_cancel_finish(c);
        return;
    }
#ifdef CONFIG_TLS
    if (c->tls) {
        static const uint8_t ssl_request[8] = { 0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f };
        if (dyn_aio_recv(c->aio, c->fd, 0, 1, pg_cancel_rx, c) < 0) {
            pg_cancel_finish(c);
            return;
        }
        if (c->direct) {
            if (pg_cancel_tls(c) < 0)
                pg_cancel_finish(c);
            return;
        }
        c->ssl_wait = 1;
        if (dyn_aio_send(c->aio, c->fd, ssl_request, sizeof ssl_request, 0, NULL, NULL) < 0)
            pg_cancel_finish(c);
        return;
    }
#endif
    if (dyn_aio_recv(c->aio, c->fd, 0, 0, pg_cancel_rx, c) < 0) {
        pg_cancel_finish(c);
        return;
    }
    if (dyn_aio_send(c->aio, c->fd, c->msg, c->len, 0, NULL, NULL) < 0)
        pg_cancel_finish(c);
}

JSValue dyn_pg_cancel(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    pg_cancel_t* c;
    (void)argc;
    (void)argv;

    if (!g)
        return JS_EXCEPTION;
    if (!g->cancel_key_len)
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: no cancel key yet; the server sends it during startup");
    c = (pg_cancel_t*)calloc(1, sizeof(*c));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->ctx = ctx;
    c->fd = -1;
    c->len = 12 + g->cancel_key_len;
    put32(c->msg, (uint32_t)c->len);
    put32(c->msg + 4, PG_CANCEL_CODE);
    put32(c->msg + 8, g->backend_pid);
    memcpy(c->msg + 12, g->cancel_key, g->cancel_key_len);
#ifdef CONFIG_TLS
    c->tls = g->use_tls;
    c->direct = g->ssl_direct;
    c->via_path = (!g->host || !g->host[0]) && g->path;
    if (c->tls && ((g->host && !(c->host = strdup(g->host))) || (g->tls_ca && !(c->ca = strdup(g->tls_ca))))) {
        free(c->host);
        free(c->ca);
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
#endif

    c->aio = dyn_net_reactor_acquire(ctx);
    if (!c->aio) {
#ifdef CONFIG_TLS
        free(c->host);
        free(c->ca);
#endif
        free(c);
        return dyn_net_throw_no_reactor(ctx, "PostgreSQL");
    }
    c->deadline_ms = dyn_timer_now_ms() + (g->query_timeout_ms ? g->query_timeout_ms : PG_CONNECT_TIMEOUT);
    if (dyn_net_on_drain(pg_cancel_tick, c) == 0)
        c->hooked = 1;
    c->fd = g->path
        ? dyn_aio_unix_connect(c->aio, g->path, pg_cancel_connected, c)
        : dyn_aio_connect(c->aio, g->addr_cur ? g->addr_cur : g->host ? g->host : "127.0.0.1", g->port,
              pg_cancel_connected, c);
    if (c->fd < 0) {
        int e = errno;
        pg_cancel_finish(c);
        return JS_ThrowInternalError(ctx, "PostgreSQL: cancel: %s", strerror(e));
    }
    return JS_UNDEFINED;
}
#endif
