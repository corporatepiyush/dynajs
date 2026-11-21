#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_PROXY_HEAD_MAX 8192

typedef struct {
    size_t head_len;
    int have_te;
    int have_cl;
    int cl_ok;
    int64_t clen;
} dyn_proxy_frame_t;

static const char* const http_proxy_keys[] = { "host", "port" };

static int dyn_proxy_hop_by_hop(const char* n, size_t len)
{
    static const char* const hop[] = {
        "connection",
        "keep-alive",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
        "proxy-authenticate",
        "proxy-authorization",
        "content-length",
        "host",
        "x-forwarded-for",
        "x-forwarded-proto",
        "x-forwarded-host",
        "x-real-ip",
        "via",
    };
    size_t i;
    for (i = 0; i < countof(hop); i++)
        if (strlen(hop[i]) == len && dyn_ci_eq(n, hop[i], len))
            return 1;
    return 0;
}

void dyn_proxy_fail(dyn_app_proxy_t* p, int status, const char* msg)
{
    dyn_app_conn_t* c = p->c;
    c->proxy = NULL;
    if (p->fd >= 0) {
        int fd = p->fd;
        p->fd = -1;
        dyn_aio_close(c->app->aio, fd);
    }
    if (!p->sent) {
        p->sent = 1;
        dyn_app_send_err(c, status, msg);
    }
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_unref(c);
    free(p);
}

static int dyn_proxy_scan_frame(const char* buf, size_t len,
    dyn_proxy_frame_t* f)
{
    size_t i, ls, first = 1;

    f->head_len = 0;
    f->have_te = 0;
    f->have_cl = 0;
    f->cl_ok = 0;
    f->clen = 0;
    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            f->head_len = i + 4;
            break;
        }
    if (!f->head_len)
        return 1;

    for (ls = 0; ls + 1 < f->head_len; first = 0) {
        size_t le = ls, colon, nlen;
        while (le + 1 < f->head_len && !(buf[le] == '\r' && buf[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        for (colon = ls; colon < le && buf[colon] != ':'; colon++)
            ;
        if (!first && colon < le) {
            const char* v = buf + colon + 1;
            size_t vlen = le - colon - 1;
            nlen = colon - ls;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vlen--;
            }
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
                vlen--;
            if (dyn_ci_equal(buf + ls, nlen, "transfer-encoding") && vlen) {
                f->have_te = 1;
                return 0;
            }
            if (dyn_ci_equal(buf + ls, nlen, "content-length")) {
                int64_t v2 = 0;
                f->have_cl = 1;
                if (vlen == 0)
                    return 0;
                for (i = 0; i < vlen; i++) {
                    if (v[i] < '0' || v[i] > '9')
                        return 0;
                    v2 = v2 * 10 + (v[i] - '0');
                    if (v2 > DYN_ACONN_MAX_REQ)
                        return 0;
                }
                if (f->cl_ok && v2 != f->clen) {
                    f->cl_ok = 0;
                    return 0;
                }
                f->clen = v2;
                f->cl_ok = 1;
            }
        }
        ls = le + 2;
    }
    return 0;
}

static int dyn_proxy_status_ok(const char* line, size_t len)
{
    if (len < 12)
        return 0;
    if (line[7] < '0' || line[7] > '9')
        return 0;
    if (line[8] != ' ')
        return 0;
    if (line[9] < '0' || line[9] > '9')
        return 0;
    if (line[10] < '0' || line[10] > '9')
        return 0;
    if (line[11] < '0' || line[11] > '9')
        return 0;
    if (len > 12 && line[12] != ' ')
        return 0;
    return 1;
}

static int dyn_proxy_status_digits(const char* buf, size_t len)
{
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0)
        return 0;
    if (buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' || buf[11] < '0' || buf[11] > '9')
        return 0;
    return (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
}

static int dyn_proxy_relay_response(dyn_app_proxy_t* p)
{
    dyn_app_conn_t* c = p->c;
    const char* buf = (const char*)p->resp.data;
    size_t len = p->resp.len, eol, body, n = 0;
    dyn_proxy_frame_t f;
    char out[DYN_PROXY_HEAD_MAX];
    int k, status, no_body;

    for (;;) {
        if (dyn_proxy_scan_frame(buf, len, &f))
            return 1;
        status = dyn_proxy_status_digits(buf, len);
        if (status >= 100 && status < 200 && status != 101) {
            memmove(p->resp.data, p->resp.data + f.head_len,
                p->resp.len - f.head_len);
            p->resp.len -= f.head_len;
            buf = (const char*)p->resp.data;
            len = p->resp.len;
            continue;
        }
        break;
    }

    if (f.have_te) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream Transfer-Encoding is "
                               "not proxied\"}");
        return 0;
    }
    if (f.have_cl && !f.cl_ok) {
        dyn_proxy_fail(p, 502, "{\"error\":\"bad upstream Content-Length\"}");
        return 0;
    }
    body = len - f.head_len;
    no_body = (status >= 100 && status < 200) || status == 204 || status == 304;
    if (!no_body && !c->head_only && f.have_cl && (int64_t)body != f.clen) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream body length does not "
                               "match Content-Length\"}");
        return 0;
    }

    for (eol = 0; eol + 1 < f.head_len; eol++)
        if (buf[eol] == '\r' && buf[eol + 1] == '\n')
            break;
    if (eol + 1 >= f.head_len || eol < 12 || memcmp(buf, "HTTP/1.", 7) != 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"bad upstream response\"}");
        return 0;
    }
    if (eol + 2 > sizeof(out) || !httpk_value_gen_ok(buf, eol) || !dyn_proxy_status_ok(buf, eol)) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream status line rejected\"}");
        return 0;
    }
    memcpy(out, buf, eol);
    n = eol;
    n += (size_t)snprintf(out + n, sizeof(out) - n, "\r\n");

    {
        size_t ls = eol + 2;
        while (ls + 1 < f.head_len) {
            size_t le = ls, colon;
            while (le + 1 < f.head_len && !(buf[le] == '\r' && buf[le + 1] == '\n'))
                le++;
            if (le == ls)
                break;
            for (colon = ls; colon < le && buf[colon] != ':'; colon++)
                ;
            if (colon < le) {
                if (!httpk_name_ok(buf + ls, colon - ls) || !httpk_value_gen_ok(buf + colon, le - colon)) {
                    dyn_proxy_fail(p, 502, "{\"error\":\"upstream header "
                                           "rejected\"}");
                    return 0;
                }
            }
            if (colon < le && !dyn_proxy_hop_by_hop(buf + ls, colon - ls)) {
                if (n + (le - ls) + 2 >= sizeof(out)) {
                    dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
                    return 0;
                }
                memcpy(out + n, buf + ls, le - ls);
                n += le - ls;
                out[n++] = '\r';
                out[n++] = '\n';
            }
            ls = le + 2;
        }
    }
    if (no_body) {
        k = snprintf(out + n, sizeof(out) - n, "Connection: close\r\n\r\n");
    } else if (c->head_only) {
        if (f.have_cl && f.cl_ok)
            k = snprintf(out + n, sizeof(out) - n,
                "Content-Length: %llu\r\nConnection: close\r\n\r\n",
                (unsigned long long)f.clen);
        else
            k = snprintf(out + n, sizeof(out) - n,
                "Connection: close\r\n\r\n");
    } else {
        k = snprintf(out + n, sizeof(out) - n,
            "Content-Length: %llu\r\nConnection: close\r\n\r\n",
            (unsigned long long)body);
    }
    if (k < 0 || n + (size_t)k >= sizeof(out)) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
        return 0;
    }
    n += (size_t)k;
    p->sent = 1;
    dyn_aio_send(c->app->aio, c->fd, out, n, DYN_AIO_HOLD, NULL, NULL);
    if (body && !no_body && !c->head_only)
        dyn_aio_send(c->app->aio, c->fd, buf + f.head_len, body, DYN_AIO_HOLD, NULL, NULL);

    {
        int fd = p->fd;
        p->fd = -1;
        if (fd >= 0)
            dyn_aio_close(c->app->aio, fd);
    }
    c->proxy = NULL;
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_close(c);
    dyn_app_conn_unref(c);
    free(p);
    return 0;
}

static int dyn_proxy_response_complete(dyn_app_proxy_t* p)
{
    dyn_proxy_frame_t f;
    int status;
    if (dyn_proxy_scan_frame((const char*)p->resp.data, p->resp.len, &f))
        return 0;
    status = dyn_proxy_status_digits((const char*)p->resp.data, p->resp.len);
    if (status >= 100 && status < 200 && status != 101)
        return 0;
    if (p->c->head_only)
        return 1;
    if (f.have_te || (f.have_cl && !f.cl_ok))
        return 1;
    if (!f.have_cl)
        return 0;
    return p->resp.len >= f.head_len + (size_t)f.clen;
}

static void dyn_proxy_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_proxy_t* p = (dyn_app_proxy_t*)udata;
    (void)aio;

    if (p->fd < 0)
        return;
    if (res < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream read failed\"}");
        return;
    }
    if (res == 0) {
        if (dyn_proxy_relay_response(p))
            dyn_proxy_fail(p, 502, "{\"error\":\"truncated upstream response\"}");
        return;
    }
    if (p->resp.len + len > (size_t)DYN_ACONN_MAX_REQ) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream response too large\"}");
        return;
    }
    if (dyn_iobuf_append(&p->resp, buf, len) < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"out of memory\"}");
        return;
    }
    if (dyn_proxy_response_complete(p))
        dyn_proxy_relay_response(p);
}

static void dyn_proxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_proxy_t* p = (dyn_app_proxy_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;

    if (res < 0 || p->fd < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
        return;
    }
    if (dyn_aio_send(p->c->app->aio, p->fd, p->req, p->req_len, 0, NULL, NULL) < 0 || dyn_aio_recv(p->c->app->aio, p->fd, 0, 1, dyn_proxy_on_recv, p) < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream write failed\"}");
}

void dyn_app_proxy_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len,
    const char* path, int64_t clen)
{
    dyn_app_proxy_t* p;
    char head[DYN_PROXY_HEAD_MAX];
    size_t n = 0, ls, sp1 = 0, i, sub_len = 0;
    const char* sub;
    int k;

    for (i = 0; i < head_len && base[i] != ' '; i++)
        ;
    sp1 = i;
    if (sp1 > 24) {
        dyn_app_send_err(c, 400, "{\"error\":\"bad method\"}");
        dyn_app_conn_close(c);
        return;
    }
    {
        const char* tstart = base + sp1 + 1;
        size_t tlen = 0, plen = strlen(rt->path);
        while (tstart + tlen < base + head_len && tstart[tlen] != ' '
            && tstart[tlen] != '\r')
            tlen++;
        if (plen > tlen)
            plen = tlen;
        sub = tstart + plen;
        sub_len = tlen - plen;
    }
    if (sub_len == 0) {
        sub = "/";
        sub_len = 1;
    }

    k = snprintf(head, sizeof(head), "%.*s %s%.*s HTTP/1.1\r\nHost: %s:%u\r\n",
        (int)sp1, base, sub[0] == '/' ? "" : "/", (int)sub_len, sub,
        rt->up_host, (unsigned)rt->up_port);
    if (k < 0 || (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n = (size_t)k;

    for (ls = 0; ls + 1 < head_len;) {
        if (base[ls] == '\r' && base[ls + 1] == '\n') {
            ls += 2;
            break;
        }
        ls++;
    }
    while (ls + 1 < head_len) {
        size_t le = ls, colon;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        for (colon = ls; colon < le && base[colon] != ':'; colon++)
            ;
        if (colon < le && !httpk_name_ok(base + ls, colon - ls)) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad header name\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (colon < le && !dyn_proxy_hop_by_hop(base + ls, colon - ls)) {
            if (n + (le - ls) + 2 >= sizeof(head)) {
                dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
                dyn_app_conn_close(c);
                return;
            }
            memcpy(head + n, base + ls, le - ls);
            n += le - ls;
            head[n++] = '\r';
            head[n++] = '\n';
        }
        ls = le + 2;
    }

    {
        char peer[INET6_ADDRSTRLEN];
        if (dyn_net_peer_ip(c->fd, peer, sizeof peer, 0) != 0)
            snprintf(peer, sizeof peer, "%s", "127.0.0.1");
        k = snprintf(head + n, sizeof(head) - n,
            "X-Forwarded-For: %s\r\nX-Forwarded-Proto: http\r\n"
            "Via: 1.1 dynajs\r\nContent-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            peer, (long long)clen);
    }
    if (k < 0 || n + (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n += (size_t)k;

    p = (dyn_app_proxy_t*)calloc(1, sizeof(*p));
    if (!p) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    p->c = c;
    p->fd = -1;
    dyn_iobuf_init(&p->resp);
    p->req_len = n + (size_t)clen;
    p->req = (uint8_t*)malloc(p->req_len ? p->req_len : 1);
    if (!p->req) {
        free(p);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    memcpy(p->req, head, n);
    if (clen > 0)
        memcpy(p->req + n, base + head_len, (size_t)clen);

    c->refs++;
    c->proxy = p;
    p->fd = dyn_aio_connect(c->app->aio, rt->up_host, rt->up_port,
        dyn_proxy_on_connect, p);
    if (p->fd < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
}

JSValue dyn_app_proxy(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *prefix = NULL, *host = NULL;
    dyn_app_route_t* nr;
    JSValue jh, jp;
    int32_t port = 0;

    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "proxy(prefix, { host, port })");
    if (dyn_opts_strict(ctx, argv[1], http_proxy_keys, 2))
        return JS_EXCEPTION;
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix)
        return JS_EXCEPTION;
    jh = JS_GetPropertyStr(ctx, argv[1], "host");
    jp = JS_GetPropertyStr(ctx, argv[1], "port");
    if (JS_IsException(jh) || JS_IsException(jp))
        goto fail;
    host = JS_IsUndefined(jh) ? js_strdup(ctx, "127.0.0.1") : JS_ToCString(ctx, jh);
    if (!host)
        goto fail;
    if (JS_ToInt32(ctx, &port, jp) < 0)
        goto fail;
    if (port <= 0 || port > 65535) {
        JS_ThrowRangeError(ctx, "proxy: upstream port out of range");
        goto fail;
    }

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto fail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_PROXY;
    nr->handler = JS_UNDEFINED;
    nr->up_host = strdup(host);
    nr->up_port = (uint16_t)port;
    if (!nr->path || !nr->up_host) {
        free(nr->path);
        free(nr->up_host);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->n_routes++;
    JS_FreeValue(ctx, jh);
    JS_FreeValue(ctx, jp);
    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_DupValue(ctx, this_val);
fail:
    JS_FreeValue(ctx, jh);
    JS_FreeValue(ctx, jp);
    if (host)
        JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}
#endif
