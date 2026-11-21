#include "redis-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_REDIS)
#define RD_ST_HANDSHAKE 1

static void redis_on_recv_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;
    (void)aio;

    if (redis_gone(r))
        return;
    if (res < 0) {
        redis_fail_all(r, "Redis: connection error");
        redis_gone(r);
        return;
    }
    if (res == 0 && len == 0) {
        redis_fail_all(r, "Redis: server closed the connection");
        redis_gone(r);
        return;
    }

    if (r->rlen - r->rpos + len > r->maxbulk + DYN_RESP_MAX_LINE) {
        redis_fail_all(r, "Redis: reply exceeds maxReplyBytes");
        return;
    }
    if (redis_buf_reserve(&r->rbuf, &r->rcap, r->rlen + len) < 0) {
        redis_fail_all(r, "Redis: out of memory");
        return;
    }
    memcpy(r->rbuf + r->rlen, buf, len);
    r->rlen += len;

    if (r->rpos == 0 && r->rlen >= 3 && dyn_resp_looks_like_tls(r->rbuf, r->rlen)) {
        redis_fail_all(r, "Redis: the endpoint answered with TLS; this client "
                          "is plaintext only");
        return;
    }

    for (;;) {
        size_t used = 0;
        int rc = dyn_resp_scan_resume(&r->rscan, r->rbuf + r->rpos,
            r->rlen - r->rpos, r->maxbulk, &used);
        if (rc == DYN_RESP_INCOMPLETE)
            break;
        if (rc != DYN_RESP_OK) {
            redis_fail_all(r, dyn_resp_strerror(rc));
            return;
        }
        if (r->rscan.streamed) {
            size_t cap = dyn_resp_canon_cap(used), clen = 0;
            uint8_t* cb = cap ? (uint8_t*)malloc(cap) : NULL;
            if (!cb || dyn_resp_canon(r->rbuf + r->rpos, used, cb, cap, &clen) != DYN_RESP_OK) {
                free(cb);
                redis_fail_all(r, cb ? "Redis: malformed streamed reply" : "Redis: out of memory");
                return;
            }
            redis_one_reply(r, cb, clen);
            free(cb);
        } else {
            redis_one_reply(r, r->rbuf + r->rpos, used);
        }
        if (redis_gone(r))
            return;
        r->rpos += used;
    }
    if (r->rpos) {
        memmove(r->rbuf, r->rbuf + r->rpos, r->rlen - r->rpos);
        r->rlen -= r->rpos;
        r->rpos = 0;
    }
}

static void redis_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;

    r->cb_depth++;
    redis_on_recv_inner(aio, res, buf, len, ud);
    redis_cb_exit(r);
}

#ifdef CONFIG_TLS
static void redis_tls_done_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    if (r->state == RD_ST_DEAD)
        return;
    if (res < 0) {
        redis_fail_all(r, "Redis: TLS handshake failed");
        redis_gone(r);
        return;
    }
    if (redis_queue_hello(r) < 0) {
        redis_fail_all(r, "Redis: out of memory during handshake");
        redis_gone(r);
    }
}

static void redis_tls_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;

    r->cb_depth++;
    redis_tls_done_inner(aio, res, buf, len, ud);
    redis_cb_exit(r);
}
#endif

static void redis_on_connect_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (r->state == RD_ST_DEAD)
        return;
    if (res < 0) {
        redis_fail_all(r, "Redis: connect failed");
        redis_gone(r);
        return;
    }
    r->state = RD_ST_HANDSHAKE;
#ifdef CONFIG_TLS
    if (r->use_tls) {
        dyn_tls_opts_t to;
        char terr[192];
        memset(&to, 0, sizeof to);
        to.ca_file = r->tls_ca;
        if (!r->tls_ctx)
            r->tls_ctx = dyn_tls_ctx_client(&to, terr, sizeof terr);
        if (!r->tls_ctx) {
            redis_fail_all(r, terr);
            redis_gone(r);
            return;
        }
        {
            dyn_tls_conn_t* t = dyn_tls_conn_new(r->tls_ctx, r->host,
                terr, sizeof terr);
            if (!t) {
                redis_fail_all(r, terr);
                redis_gone(r);
                return;
            }
            if (dyn_aio_tls_attach(r->aio, r->fd, t,
                    redis_tls_done, r)
                < 0) {
                dyn_tls_conn_free(t);
                redis_fail_all(r, "Redis: cannot attach TLS engine");
                redis_gone(r);
                return;
            }
            if (dyn_aio_recv(r->aio, r->fd, 0, 1,
                    redis_on_recv, r)
                    < 0
                || dyn_aio_tls_start(r->aio, r->fd) < 0) {
                redis_fail_all(r, "Redis: TLS handshake failed to start");
                redis_gone(r);
                return;
            }
        }
        return;
    }
#endif
    if (dyn_aio_recv(r->aio, r->fd, 0, 1, redis_on_recv, r) < 0) {
        redis_fail_all(r, "Redis: cannot read from the socket");
        redis_gone(r);
        return;
    }
    redis_queue_hello(r);
    if (redis_flush(r) < 0)
        redis_fail_all(r, "Redis: cannot write to the socket");
}

void redis_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_redis_t* r = (dyn_redis_t*)ud;

    r->cb_depth++;
    redis_on_connect_inner(aio, res, buf, len, ud);
    redis_cb_exit(r);
}

void dyn_redis_teardown(dyn_redis_t* r)
{
    dyn_redis_pending_t* p;
    redis_flush_drop(r);
    if (r->hooked)
        dyn_net_off_drain(r);
    if (r->state != RD_ST_DEAD) {
        r->state = RD_ST_DEAD;
        while ((p = pend_pop(&r->head, &r->tail)) != NULL) {
            if (p->internal || redis_in_final)
                pend_free_rt(r->rt, p);
            else
                redis_settle(r, p, 1,
                    redis_conn_error(r->ctx, "Redis: client closed"));
        }
        while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL) {
            if (redis_in_final)
                pend_free_rt(r->rt, p);
            else
                redis_settle(r, p, 1,
                    redis_conn_error(r->ctx, "Redis: client closed"));
        }
    }
    if (r->self_held) {
        JSValue s = r->self;
        r->self = JS_UNDEFINED;
        r->self_held = 0;
        JS_RemoveShutdownSweep(r->rt, redis_client_sweep, r);
        JS_FreeValueRT(r->rt, s);
    }
    if (r->aio && !r->released) {
        if (r->fd >= 0)
            dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release_rt(r->rt);
    }
    JS_FreeValueRT(r->rt, r->h_push);
    JS_FreeValueRT(r->rt, r->h_error);
#ifdef CONFIG_TLS
    if (r->tls_ctx)
        dyn_tls_ctx_free(r->tls_ctx);
    free(r->tls_ca);
#endif
    free(r->host);
    free(r->path);
    free(r->user);
    if (r->pass) {
        memset(r->pass, 0, strlen(r->pass));
        free(r->pass);
    }
    free(r->rbuf);
    free(r->obuf);
    free(r);
}
#endif
