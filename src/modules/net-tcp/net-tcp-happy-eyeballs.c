#include "net-tcp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

int eb_resolve(const char* host, uint16_t port,
    char out[2][INET6_ADDRSTRLEN], int numeric_only)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (numeric_only)
        hints.ai_flags = AI_NUMERICHOST;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        if (out[0][0] == 0 && ai->ai_family == AF_INET) {
            const struct sockaddr_in* sa = (const struct sockaddr_in*)ai->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, out[0], INET6_ADDRSTRLEN);
        } else if (out[1][0] == 0 && ai->ai_family == AF_INET6) {
            const struct sockaddr_in6* sa = (const struct sockaddr_in6*)ai->ai_addr;
            inet_ntop(AF_INET6, &sa->sin6_addr, out[1], INET6_ADDRSTRLEN);
        }
    }
    freeaddrinfo(res);
    return 0;
}

static void eb_fail(dyn_tcp_t* t, const char* msg)
{
    JSContext* ctx = t->ctx;
    dyn_aio_t* aio = t->aio;
    dyn_tcp_conn_t* c = t->pending_conn;
    int i;

    t->pending_conn = NULL;
    t->eb_done = 1;
    t->eb_deadline_ms = 0;
    for (i = 0; i < 2; i++)
        if (t->eb[i].fd >= 0) {
            dyn_aio_close(aio, t->eb[i].fd);
            t->eb[i].fd = -1;
        }
    if (c && !c->closed) {
        c->closed = 1;
        if (c->fd >= 0)
            dyn_aio_close(aio, c->fd);
        c->fd = -1;
        tcp_conn_drop(JS_GetRuntime(ctx), c);
    }
    tcp_report_connect(t, JS_UNDEFINED, msg);
}

void eb_sweep(void* udata)
{
    dyn_tcp_t* t = (dyn_tcp_t*)udata;
    if (t->eb_done || t->eb_deadline_ms == 0)
        return;
    if (dyn_timer_now_ms() < t->eb_deadline_ms)
        return;
    eb_fail(t, "connectHappy: timed out");
}

static void eb_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    eb_attempt_t* att = (eb_attempt_t*)ud;
    dyn_tcp_t* t = att->owner;
    int fd = att->fd;
    int idx = att->idx;
    (void)buf;
    (void)len;

    att->fd = -1;
    if (t->eb_done) {
        if (fd >= 0)
            dyn_aio_close(aio, fd);
        return;
    }
    if (res != 0) {
        char msg[192];
        if (fd >= 0)
            dyn_aio_close(aio, fd);
        t->eb_err = -res;
        if (t->eb[1 - idx].fd >= 0)
            return;
        snprintf(msg, sizeof(msg), "connectHappy: connect failed: %s",
            strerror(t->eb_err));
        eb_fail(t, msg);
        return;
    }
    t->eb_done = 1;
    t->eb_deadline_ms = 0;
    if (t->eb[1 - idx].fd >= 0) {
        dyn_aio_close(aio, t->eb[1 - idx].fd);
        t->eb[1 - idx].fd = -1;
    }
    if (t->pending_conn) {
        t->pending_conn->fd = fd;
        tcp_on_connect(aio, 0, NULL, 0, t->pending_conn);
    } else if (fd >= 0) {
        dyn_aio_close(aio, fd);
    }
}

int eb_arm(dyn_tcp_t* t, char ips[2][INET6_ADDRSTRLEN], uint16_t port,
    uint64_t fallback)
{
    int fd, n_armed = 0;

    t->eb_deadline_ms = dyn_timer_now_ms() + fallback;
    if (ips[0][0]) {
        fd = dyn_aio_connect(t->aio, ips[0], port, eb_on_connect, &t->eb[0]);
        if (fd >= 0) {
            t->eb[0].fd = fd;
            n_armed++;
        }
    }
    if (ips[1][0]) {
        fd = dyn_aio_connect(t->aio, ips[1], port, eb_on_connect, &t->eb[1]);
        if (fd >= 0) {
            t->eb[1].fd = fd;
            n_armed++;
        }
    }
    return n_armed;
}

void eb_dns_free(eb_dns_t* d)
{
    free(d->host);
    free(d);
}

void eb_dns_work(void* p)
{
    eb_dns_t* d = (eb_dns_t*)p;
    if (eb_resolve(d->host, d->port, d->ips, 0) < 0
        || (!d->ips[0][0] && !d->ips[1][0]))
        d->failed = 1;
}

void eb_dns_done(void* p)
{
    eb_dns_t* d = (eb_dns_t*)p;
    dyn_tcp_t* t = d->owner;

    if (d->sync)
        return;
    if (!t) {
        JSRuntime* rt = d->rt;
        int held = d->held;
        eb_dns_free(d);
        if (held)
            dyn_net_reactor_release_rt(rt);
        return;
    }
    t->eb_dns = NULL;
    if (!t->eb_done) {
        if (d->failed)
            eb_fail(t, "connectHappy: DNS resolution failed");
        else if (eb_arm(t, d->ips, d->port, d->fallback) == 0)
            eb_fail(t, "connectHappy: connect failed");
    }
    eb_dns_free(d);
}

#endif
