#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

static ssize_t hc_raw_send(int fd, const void* p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t s = send(fd, (const char*)p + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)n;
}

int hc_conn_open(hc_conn_t* c, const char* host, uint16_t port,
    int64_t timeout_ms, int (*reject)(int fd, void* ud), void* ud,
    int* reason, char* why, size_t whyn)
{
    int r = 0, gai = 0;
    c->fd = -1;
#ifdef CONFIG_TLS
    c->tls = NULL;
#endif
    c->deadline_ms = 0;
    c->tls_cut = 0;
    c->fd = dyn_net_connect_blocking(host, port, timeout_ms, reject, ud, &r, &gai);
    if (reason)
        *reason = c->fd >= 0 ? 0 : r;
    if (c->fd >= 0) {
        if (timeout_ms > 0)
            dyn_net_set_io_timeout(c->fd, timeout_ms);
        return 0;
    }
    if (r == DYN_NET_CONNECT_RESOLVE)
        snprintf(why, whyn, "cannot resolve %.100s: %s", host,
            gai ? gai_strerror(gai) : "no address");
    else if (r == DYN_NET_CONNECT_TIMEOUT)
        snprintf(why, whyn, "connect to %.100s timed out", host);
    else if (r == DYN_NET_CONNECT_REFUSED_BY_FILTER)
        snprintf(why, whyn,
            "connect to %.100s reached a private/loopback address; refused",
            host);
    else
        snprintf(why, whyn, "connect to %.100s failed", host);
    return -1;
}

void hc_conn_close(hc_conn_t* c)
{
    if (!c)
        return;
#ifdef CONFIG_TLS
    dyn_tls_conn_free(c->tls);
    c->tls = NULL;
#endif
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

#ifdef CONFIG_TLS
static int hc_tls_flush(hc_conn_t* c)
{
    uint8_t out[16384] DYN_IO_SCRATCH;
    int n;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0)
        if (hc_raw_send(c->fd, out, (size_t)n) < 0)
            return -1;
    return 0;
}

static int hc_tls_handshake(hc_conn_t* c)
{
    uint8_t buf[16384] DYN_IO_SCRATCH;
    for (;;) {
        int st = dyn_tls_handshake(c->tls);
        ssize_t r;
        if (hc_tls_flush(c) < 0)
            return -2;
        if (st < 0)
            return -1;
        if (st == 1)
            return 0;
        r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0)
            return -3;
        if (dyn_tls_feed(c->tls, buf, (size_t)r) != 0)
            return -3;
    }
}

int hc_conn_tls_open(hc_conn_t* conn, dyn_tls_ctx_t* tls_ctx,
    const char* host, char* why, size_t why_n)
{
    char terr[192];
    terr[0] = 0;
    conn->tls = dyn_tls_conn_new(tls_ctx, host, terr, sizeof terr);
    if (!conn->tls || hc_tls_handshake(conn) != 0) {
        const char* w = conn->tls ? dyn_tls_error(conn->tls) : terr;
        snprintf(why, why_n, "%s", w && *w ? w : "handshake failed");
        return -1;
    }
    return 0;
}

dyn_tls_ctx_t* hc_tls_client_ctx(const char* ca, char* why, size_t whyn)
{
    dyn_tls_opts_t o;
    memset(&o, 0, sizeof o);
    o.min_version = 12;
    if (ca && *ca) {
        if (strstr(ca, "-----BEGIN"))
            o.ca_pem = ca;
        else
            o.ca_file = ca;
    }
    return dyn_tls_ctx_client(&o, why, whyn);
}

int hc_client_adopt_ca(JSValueConst client, const char* ca, char* why, size_t whyn)
{
    DynResource* r = (DynResource*)JS_GetOpaque(client, dyn_http_client_class_id);
    dyn_http_client_t* cl;
    dyn_tls_ctx_t* t;
    char* dup;
    if (!r || r->closed || !r->native)
        return 0;
    cl = (dyn_http_client_t*)r->native;
    t = hc_tls_client_ctx(ca, why, whyn);
    if (!t)
        return -1;
    dup = strdup(ca);
    if (!dup) {
        dyn_tls_ctx_free(t);
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    if (cl->tls)
        dyn_tls_ctx_free(cl->tls);
    cl->tls = t;
    free(cl->ca);
    cl->ca = dup;
    return 1;
}
#endif

ssize_t hc_send(hc_conn_t* c, const void* p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        size_t off = 0;
        while (off < n) {
            int w = dyn_tls_write(c->tls, (const uint8_t*)p + off, n - off);
            if (w <= 0)
                return -1;
            off += (size_t)w;
            if (hc_tls_flush(c) != 0)
                return -1;
        }
        return (ssize_t)n;
    }
#endif
    return hc_raw_send(c->fd, p, n);
}

ssize_t hc_recv(hc_conn_t* c, void* p, size_t n)
{
    if (c->deadline_ms && dyn_timer_now_ms() >= c->deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
#ifdef CONFIG_TLS
    if (c->tls) {
        for (;;) {
            uint8_t cipher[16384] DYN_IO_SCRATCH;
            ssize_t r;
            int got = dyn_tls_read(c->tls, (uint8_t*)p, n);
            if (got > 0)
                return got;
            if (got < 0)
                return dyn_tls_peer_closed(c->tls) ? 0 : -1;
            r = recv(c->fd, cipher, sizeof cipher, 0);
            if (r == 0)
                c->tls_cut = 1;
            if (r <= 0)
                return r;
            if (dyn_tls_feed(c->tls, cipher, (size_t)r) != 0)
                return -1;
        }
    }
#endif
    return dyn_recv_eintr(c->fd, p, n, 0);
}

void hc_head_free(hc_head_t* hd)
{
    free(hd->raw);
    free(hd->extra);
    hd->raw = NULL;
    hd->extra = NULL;
    hd->extra_len = 0;
}

int hc_read_head(hc_conn_t* conn, hc_head_t* hd, char* why, size_t whyn)
{
    char* buf = (char*)malloc(HC_HEAD_MAX + 1);
    size_t n = 0, scan = 0, he;
    int skips = 0, rc, ret = -1;
    hd->raw = NULL;
    hd->extra = NULL;
    hd->extra_len = 0;
    if (!buf) {
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    for (;;) {
        he = httpk_head_end(buf, n, &scan);
        while (!he && n < HC_HEAD_MAX) {
            ssize_t r = hc_recv(conn, buf + n, HC_HEAD_MAX - n);
            if (r < 0) {
                snprintf(why, whyn, "receiving response head failed");
                goto bad;
            }
            if (r == 0) {
                snprintf(why, whyn, "connection closed before response head");
                goto bad;
            }
            n += (size_t)r;
            he = httpk_head_end(buf, n, &scan);
        }
        if (!he) {
            snprintf(why, whyn, "response head exceeds %d bytes", HC_HEAD_MAX);
            goto bad;
        }
        {
            const char* eol = (const char*)memchr(buf, '\r', he);
            int st = 0, minor;
            size_t ro, rl;
            if (he < 12 || memcmp(buf, "HTTP/", 5) != 0) {
                snprintf(why, whyn, "not an HTTP/1.x response");
                ret = -2;
                goto bad;
            }
            if (!eol || httpk_status_line(buf, (size_t)(eol - buf), &st, &minor, &ro, &rl) < 0) {
                snprintf(why, whyn, "malformed status line");
                ret = -2;
                goto bad;
            }
            if (httpk_is_interim(st)) {
                if (++skips > HC_MAX_INTERIM) {
                    snprintf(why, whyn, "too many interim responses");
                    ret = -2;
                    goto bad;
                }
                memmove(buf, buf + he, n - he);
                n -= he;
                scan = 0;
                continue;
            }
        }
        hd->h.hdrs = hd->f;
        hd->h.cap_hdrs = HC_HEAD_FIELDS;
        rc = httpk_resp_head(buf, he, &hd->h);
        if (rc != HTTPK_OK) {
            dyn_head_why(buf, &hd->h, rc, why, whyn);
            ret = -2;
            goto bad;
        }
        if (n > he) {
            hd->extra = (uint8_t*)malloc(n - he);
            if (!hd->extra) {
                snprintf(why, whyn, "out of memory");
                goto bad;
            }
            memcpy(hd->extra, buf + he, n - he);
            hd->extra_len = n - he;
        }
        buf[he] = 0;
        hd->raw = buf;
        return 0;
    }
bad:
    free(buf);
    return ret;
}

const char* hc_head_get(const hc_head_t* hd, const char* name, size_t* vlen)
{
    return httpk_hdr_first(&hd->h, hd->raw, name, vlen);
}

JSValue hc_head_headers(JSContext* ctx, const hc_head_t* hd)
{
    return dyn_headers_js(ctx, hd->raw, &hd->h);
}

void hc_stream_init(hc_stream_t* s, hc_conn_t* conn, hc_head_t* hd,
    uint64_t max_body)
{
    s->conn = *conn;
    conn->fd = -1;
#ifdef CONFIG_TLS
    conn->tls = NULL;
#endif
    s->chunked = hd->h.chunked;
    s->clen = (hd->h.chunked || !hd->h.has_cl) ? UINT64_MAX : hd->h.content_length;
    s->max_body = max_body;
    httpk_dechunk_init(&s->dc);
    if (hd->extra_len) {
        s->rbuf = hd->extra;
        s->rcap = hd->extra_len;
        s->rn = hd->extra_len;
        hd->extra = NULL;
        hd->extra_len = 0;
    }
}

void hc_stream_dispose(void* native)
{
    hc_stream_t* s = (hc_stream_t*)native;
    if (!s)
        return;
    hc_conn_close(&s->conn);
    free(s->rbuf);
    free(s->err);
    free(s);
}


static int hc_stream_fill(hc_stream_t* s, char* why, size_t whyn)
{
    ssize_t r;
    if (s->rn == s->rcap) {
        size_t nc = s->rcap ? s->rcap * 2 : 8192;
        uint8_t* nb = (uint8_t*)realloc(s->rbuf, nc);
        if (!nb) {
            snprintf(why, whyn, "out of memory");
            return -1;
        }
        s->rbuf = nb;
        s->rcap = nc;
    }
    r = hc_recv(&s->conn, s->rbuf + s->rn, s->rcap - s->rn);
    if (r < 0) {
        snprintf(why, whyn, "receiving body failed");
        return -1;
    }
    if (r == 0) {
        s->eof = 1;
        return 0;
    }
    s->rn += (size_t)r;
    return 0;
}

static long long hc_stream_read(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn, int some)
{
    size_t got = 0;
    if (s->eof)
        return 0;
    while (got < want) {
        if (!s->chunked && s->clen != UINT64_MAX && s->served >= s->clen) {
            s->eof = 1;
            return (long long)got;
        }
        if (s->rpos >= s->rn) {
            if (some && got)
                return (long long)got;
            s->rpos = s->rn = 0;
            if (hc_stream_fill(s, why, whyn) < 0)
                return -1;
            if (s->eof) {
                if (s->chunked) {
                    snprintf(why, whyn,
                        "truncated chunked body: connection ended before the terminal chunk");
                    return -1;
                }
                if (s->clen != UINT64_MAX && s->served < s->clen) {
                    snprintf(why, whyn,
                        "truncated body: connection ended after %llu of %llu declared bytes",
                        (unsigned long long)s->served, (unsigned long long)s->clen);
                    return -1;
                }
                return (long long)got;
            }
            continue;
        }
        if (s->chunked) {
            size_t avail = s->rn - s->rpos, used = 0, outn = 0;
            size_t slice = avail < want - got ? avail : want - got;
            int rc = httpk_dechunk(&s->dc, s->rbuf + s->rpos, slice, &used, &outn);
            if (rc == HTTPK_DC_BAD) {
                snprintf(why, whyn, "malformed chunked body: %s", s->dc.why);
                return -1;
            }
            memcpy(dst + got, s->rbuf + s->rpos, outn);
            got += outn;
            s->served += outn;
            s->rpos += used;
            if (s->served + s->dc.trailer > s->max_body) {
                snprintf(why, whyn, "body exceeds maxBodyBytes (trailer section included)");
                return -1;
            }
            if (rc == HTTPK_DC_DONE) {
                s->eof = 1;
                if (s->served > s->max_body)
                    break;
                return (long long)got;
            }
        } else {
            size_t have = s->rn - s->rpos, room = want - got;
            if (s->clen != UINT64_MAX && (uint64_t)room > s->clen - s->served)
                room = (size_t)(s->clen - s->served);
            if (have > room)
                have = room;
            memcpy(dst + got, s->rbuf + s->rpos, have);
            s->rpos += have;
            got += have;
            s->served += have;
        }
        if (s->served > s->max_body)
            break;
    }
    if (s->served > s->max_body) {
        snprintf(why, whyn, "body exceeds maxBodyBytes");
        return -1;
    }
    return (long long)got;
}

long long hc_stream_body(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn)
{
    return hc_stream_read(s, dst, want, why, whyn, 0);
}

long long hc_stream_some(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn)
{
    return hc_stream_read(s, dst, want, why, whyn, 1);
}

JSValue hc_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

JSValue hc_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;
    if (JS_IsException(exc))
        exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

uint8_t* hc_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen,
    const char* who)
{
    size_t n = 0;
    uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
    *plen = 0;
    if (p) {
        *plen = n;
        return p;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return NULL;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx,
                "%s.read: buf must be a byte-wide view "
                "(Uint8Array/DataView/ArrayBuffer)",
                who);
            return NULL;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return NULL;
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}
#endif
