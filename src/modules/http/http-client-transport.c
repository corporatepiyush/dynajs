#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_HTTP_ERR_TOOBIG 8
#define DYN_HTTP_MAX_RESP_HEADER (64 * 1024)
#define DYN_HTTP_TOTAL_TIMEOUT_FACTOR 20
#define HC_RESP_HEAD 0
#define HC_RESP_TO_EOF 1
#define HC_RESP_TO_LENGTH 2
#define HC_RESP_TO_CHUNKED 3
#define HC_ASTAGE_CONNECT 1
#define HC_ASTAGE_TLS 2
#define HC_ASTAGE_EXCHANGE 3
#define HC_POOL_MAX 256u
#define HC_POOL_IDLE_MS 4000u
#define HC_DROPPERS 16u
#define HC_DROPPER_MS 60000u
#define HC_ORIGIN_MAX 64u

typedef struct {
    uint64_t until_ms;
    uint16_t port;
    int is_tls;
    char host[256];
} hc_dropper_t;

static _Thread_local int hc_no_pump;

const char* hc_err_name(int code)
{
    static const char* const names[] = {
        "ok", "bad URL", "DNS resolution failed", "connection failed",
        "send failed", "receive failed", "malformed response",
        "out of memory", "response too large",
        "TLS handshake failed", "aborted", "truncated body"
    };
    return (code >= 0 && code < (int)countof(names)) ? names[code] : "error";
}

static void hc_redact_put(char* out, size_t cap, size_t* n, const char* s)
{
    size_t left, l;

    if (*n >= cap)
        return;
    left = cap - 1 - *n;
    l = strlen(s);
    if (l > left) {
        memcpy(out + *n, s, left);
        *n = cap - 1;
        out[*n] = 0;
        return;
    }
    memcpy(out + *n, s, l);
    *n += l;
    out[*n] = 0;
}

static void hc_url_redact(const char* url, char* out, size_t cap)
{
    const char* p = url ? url : "";
    const char* scheme = strstr(p, "://");
    size_t n = 0;

    if (cap == 0)
        return;
    out[0] = 0;
    if (scheme) {
        const char* auth = scheme + 3;
        const char* q = auth;
        const char* at;
        size_t pre;
        while (*q && *q != '/' && *q != '?' && *q != '#')
            q++;
        at = (const char*)memchr(auth, '@', (size_t)(q - auth));
        pre = (size_t)(auth - p);
        if (pre > cap - 1)
            pre = cap - 1;
        memcpy(out, p, pre);
        out[pre] = 0;
        n = pre;
        if (at)
            hc_redact_put(out, cap, &n, "***@");
        hc_redact_put(out, cap, &n, at ? at + 1 : auth);
    } else {
        hc_redact_put(out, cap, &n, p);
    }
    {
        char* qm = strchr(out, '?');
        if (qm)
            snprintf(qm, cap - (size_t)(qm - out), "?[redacted]");
    }
}

JSValue hc_error_value_named(JSContext* ctx, int code, const char* method,
    const char* url, const char* what)
{
    JSValue e;
    char msg[640];
    char rurl[256];

    hc_url_redact(url, rurl, sizeof rurl);
    snprintf(msg, sizeof(msg), "HTTP %s %s failed: %s", method,
        rurl, what);
    if (code == DYN_HTTP_ERR_TOO_LARGE) {
        JS_ThrowRangeError(ctx, "%s", msg);
        e = JS_GetException(ctx);
    } else {
        e = JS_NewError(ctx);
        if (JS_IsException(e))
            return JS_EXCEPTION;
        JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    JS_DefinePropertyValueStr(ctx, e, "dynajsError", JS_NewInt32(ctx, code),
        JS_PROP_C_W_E);
    return e;
}

JSValue hc_error_value(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy)
{
    const char* what = hc_err_name(code);

    if (code == DYN_HTTP_ERR_TLS && tlswhy && *tlswhy)
        what = tlswhy;
    return hc_error_value_named(ctx, code, method, url, what);
}

void hc_resp_init(hc_resp_t* st, const char* method, size_t max_body)
{
    memset(st, 0, sizeof(*st));
    st->method = method;
    st->max_body = max_body;
}

size_t hc_resp_room(const hc_resp_t* st, const dyn_bytes_t* resp)
{
    if (st->phase == HC_RESP_TO_LENGTH && st->want > resp->len)
        return st->want - resp->len;
    return 8192;
}

static int hc_resp_parse_error(char* why, size_t whyn, const char* what)
{
    snprintf(why, whyn, "%s: %s", hc_err_name(DYN_HTTP_ERR_PARSE), what);
    return DYN_HTTP_ERR_PARSE;
}

static int hc_resp_head(hc_resp_t* st, dyn_bytes_t* resp, char* why,
    size_t whyn)
{
    size_t hdr_end = st->hdr_end;
    httpk_head_t h;
    httpk_hdr_t small[32], *heap = NULL;
    int rc = dyn_head_parse(resp->data, hdr_end, &h, small, 32, &heap);
    if (rc != HTTPK_OK) {
        char what[120];
        dyn_head_why(resp->data, &h, rc, what, sizeof what);
        free(heap);
        return hc_resp_parse_error(why, whyn, what);
    }
    free(heap);
    st->chunked = h.chunked;
    if (h.conn_close || hdr_end < 8 || memcmp(resp->data, "HTTP/1.1", 8) != 0)
        st->peer_closes = 1;
    if (httpk_bodyless(h.status, st->method)) {
        st->reusable = !st->peer_closes && resp->len == hdr_end;
        return 0;
    }
    if (h.chunked) {
        st->chunk_pos = st->out_pos = hdr_end;
        httpk_dechunk_init(&st->dc);
        st->phase = HC_RESP_TO_CHUNKED;
    } else if (!h.has_cl) {
        st->phase = HC_RESP_TO_EOF;
    } else {
        if (h.content_length > (uint64_t)(SIZE_MAX - hdr_end)
            || hdr_end + (size_t)h.content_length > st->max_body)
            return DYN_HTTP_ERR_TOOBIG;
        st->content_length = (size_t)h.content_length;
        st->want = hdr_end + st->content_length;
        st->phase = HC_RESP_TO_LENGTH;
    }
    return HC_RESP_MORE;
}

static int hc_resp_chunked(hc_resp_t* st, dyn_bytes_t* resp, int eof,
    char* why, size_t whyn)
{
    size_t used = 0, outn = 0, rem;
    int dr = httpk_dechunk(&st->dc, (uint8_t*)resp->data + st->chunk_pos,
        resp->len - st->chunk_pos, &used, &outn);
    if (dr == HTTPK_DC_BAD) {
        snprintf(why, whyn, "malformed chunked body: %s", st->dc.why);
        return DYN_HTTP_ERR_PARSE;
    }
    if (outn && st->out_pos != st->chunk_pos)
        memmove(resp->data + st->out_pos, resp->data + st->chunk_pos, outn);
    st->out_pos += outn;
    st->chunk_pos += used;
    if (dr == HTTPK_DC_DONE) {
        st->reusable = !st->peer_closes && st->chunk_pos == resp->len;
        resp->len = st->out_pos;
        return 0;
    }
    rem = resp->len - st->chunk_pos;
    if (st->chunk_pos != st->out_pos) {
        memmove(resp->data + st->out_pos, resp->data + st->chunk_pos, rem);
        resp->len = st->out_pos + rem;
        st->chunk_pos = st->out_pos;
    }
    if (eof) {
        snprintf(why, whyn,
            "truncated chunked body: connection ended before the terminal chunk");
        return DYN_HTTP_ERR_PARSE;
    }
    return HC_RESP_MORE;
}

static int hc_resp_step_in(hc_resp_t* st, dyn_bytes_t* resp, int eof, int tls_cut,
    char* truncwhy, size_t truncwhy_n)
{
    if (st->phase == HC_RESP_HEAD) {
        int rc;
        for (;;) {
            size_t hdr_end = httpk_head_end(resp->data, resp->len, &st->scanned);
            const char* eol;
            int status = 0, minor;
            size_t ro, rl;
            if (!hdr_end) {
                if (resp->len > DYN_HTTP_MAX_RESP_HEADER)
                    return DYN_HTTP_ERR_TOOBIG;
                if (eof && resp->len == 0 && st->interims == 0)
                    snprintf(truncwhy, truncwhy_n, "connection closed before any response");
                return eof ? DYN_HTTP_ERR_PARSE : HC_RESP_MORE;
            }
            if (hdr_end > DYN_HTTP_MAX_RESP_HEADER)
                return DYN_HTTP_ERR_TOOBIG;
            eol = (const char*)memchr(resp->data, '\r', hdr_end);
            if (!eol || httpk_status_line(resp->data, (size_t)(eol - resp->data), &status, &minor, &ro, &rl) < 0)
                return hc_resp_parse_error(truncwhy, truncwhy_n, "malformed status line");
            if (httpk_is_interim(status)) {
                if (++st->interims > 8)
                    return hc_resp_parse_error(truncwhy, truncwhy_n, "too many interim responses");
                memmove(resp->data, resp->data + hdr_end, resp->len - hdr_end);
                resp->len -= hdr_end;
                st->scanned = 0;
                continue;
            }
            st->hdr_end = hdr_end;
            break;
        }
        rc = hc_resp_head(st, resp, truncwhy, truncwhy_n);
        if (rc != HC_RESP_MORE)
            return rc;
    }
    if (st->phase == HC_RESP_TO_CHUNKED)
        return hc_resp_chunked(st, resp, eof, truncwhy, truncwhy_n);
    if (st->phase == HC_RESP_TO_EOF) {
        if (!eof)
            return HC_RESP_MORE;
        if (tls_cut) {
            snprintf(truncwhy, truncwhy_n,
                "truncated body: the TLS stream ended without "
                "close_notify and the response declares no length");
            return DYN_HTTP_ERR_TRUNC;
        }
        return 0;
    }
    if (resp->len >= st->want) {
        st->reusable = !st->peer_closes && resp->len == st->want;
        resp->len = st->want;
        return 0;
    }
    if (eof) {
        snprintf(truncwhy, truncwhy_n,
            "truncated body: connection ended after %llu of "
            "%llu declared bytes",
            (unsigned long long)(resp->len - st->hdr_end + st->dropped),
            (unsigned long long)(st->content_length));
        return DYN_HTTP_ERR_TRUNC;
    }
    return HC_RESP_MORE;
}

int hc_resp_step(hc_resp_t* st, dyn_bytes_t* resp, int eof, int tls_cut,
    char* truncwhy, size_t truncwhy_n)
{
    int rc;
    if (st->phase != HC_RESP_HEAD && resp->len + st->dropped > st->max_body)
        return DYN_HTTP_ERR_TOOBIG;
    rc = hc_resp_step_in(st, resp, eof, tls_cut, truncwhy, truncwhy_n);
    if ((rc == HC_RESP_MORE || rc == 0) && resp->len + st->dropped > st->max_body)
        return DYN_HTTP_ERR_TOOBIG;
    return rc;
}

static int hc_exchange_open(hc_exch_t* x, hc_conn_t* conn)
{
    int reason = 0;
    char hbuf[256], why[160];

    x->peer_ip[0] = 0;
    if (hc_conn_open(conn, dyn_host_bare(x->u.host, hbuf, sizeof hbuf),
            x->u.port, x->timeout_ms, NULL, NULL, &reason, why, sizeof why)
        < 0)
        return reason == DYN_NET_CONNECT_RESOLVE ? DYN_HTTP_ERR_RESOLVE : DYN_HTTP_ERR_CONNECT;
    if (x->on_connect && dyn_net_peer_ip(conn->fd, x->peer_ip, sizeof x->peer_ip, 0) != 0)
        x->peer_ip[0] = 0;
    return 0;
}

static int hc_build_request(const hc_exch_t* x, dyn_bytes_t* req, int keepalive)
{
    char line[512];

    if (x->u.port == 80)
        snprintf(line, sizeof(line), "Host: %s\r\n", x->u.host);
    else
        snprintf(line, sizeof(line), "Host: %s:%u\r\n", x->u.host,
            (unsigned)x->u.port);
    if (dyn_bytes_append(req, x->method, strlen(x->method)) < 0
        || dyn_bytes_append(req, " ", 1) < 0
        || dyn_bytes_append(req, x->u.path, x->u.path_len) < 0
        || dyn_bytes_append(req, " HTTP/1.1\r\n", 11) < 0
        || dyn_bytes_append(req, line, strlen(line)) < 0) {
        free(req->data);
        req->data = NULL;
        return DYN_HTTP_ERR_OOM;
    }
    if (x->hdr && dyn_bytes_append(req, x->hdr, strlen(x->hdr)) < 0) {
        free(req->data);
        req->data = NULL;
        return DYN_HTTP_ERR_OOM;
    }
    if (x->body) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", x->body_len);
        if (dyn_bytes_append(req, line, strlen(line)) < 0) {
            free(req->data);
            req->data = NULL;
            return DYN_HTTP_ERR_OOM;
        }
    }
    if ((keepalive ? dyn_bytes_append(req, "Connection: keep-alive\r\n\r\n", 26)
                   : dyn_bytes_append(req, "Connection: close\r\n\r\n", 21))
            < 0
        || (x->body && dyn_bytes_append(req, x->body, x->body_len) < 0)) {
        free(req->data);
        req->data = NULL;
        return DYN_HTTP_ERR_OOM;
    }
    return 0;
}

#define HC_RUN_REUSED 1
#define HC_RUN_KEEPALIVE 2

static int hc_exchange_run(hc_exch_t* x, hc_conn_t* conn, int mode)
{
    dyn_bytes_t req = { 0 };
    __maybe_unused char hbuf[256];

    if (conn->fd < 0)
        return DYN_HTTP_ERR_CONNECT;
    x->tlswhy[0] = 0;
    conn->deadline_ms = x->timeout_ms > 0
        ? dyn_timer_now_ms() + (uint64_t)x->timeout_ms * DYN_HTTP_TOTAL_TIMEOUT_FACTOR
        : 0;
    if (x->timeout_ms > 0)
        dyn_net_set_io_timeout(conn->fd, x->timeout_ms);
#ifdef CONFIG_TLS
    if (x->u.tls && !(mode & HC_RUN_REUSED)) {
        if (!x->tls_ctx) {
            snprintf(x->tlswhy, sizeof x->tlswhy, "no TLS context");
            return DYN_HTTP_ERR_TLS;
        }
        if (hc_conn_tls_open(conn, x->tls_ctx,
                dyn_host_bare(x->u.host, hbuf, sizeof hbuf), x->tlswhy,
                sizeof x->tlswhy)
            != 0)
            return DYN_HTTP_ERR_TLS;
    }
#endif

    {
        int berr = hc_build_request(x, &req, (mode & HC_RUN_KEEPALIVE) != 0);
        if (berr != 0)
            return berr;
    }

    if (hc_send(conn, req.data, req.len) < 0) {
        free(req.data);
        return DYN_HTTP_ERR_SEND;
    }
    free(req.data);
    return dyn_read_response(conn, x->method, x->max_body, &x->resp,
        &x->hdr_end, &x->chunked, x->cancel,
        x->truncwhy, sizeof x->truncwhy, &x->reusable);
}

void hc_exchange(hc_exch_t* x)
{
    hc_conn_t conn;

    memset(&conn, 0, sizeof conn);
    conn.fd = -1;
    x->err = hc_exchange_open(x, &conn);
    if (x->err == 0 && x->on_connect && x->peer_ip[0]
        && x->on_connect(x->peer_ip, x->on_connect_ud) != 0)
        x->err = DYN_HTTP_ERR_CONNECT;
    if (x->err == 0)
        x->err = hc_exchange_run(x, &conn, 0);
    hc_conn_close(&conn);
}

#ifdef CONFIG_TLS
int hc_tls_ensure(JSContext* ctx, dyn_http_client_t* cl, char* terr,
    size_t terr_n)
{
    if (!cl->tls) {
        dyn_tls_opts_t o;
        (void)ctx;
        memset(&o, 0, sizeof o);
        o.min_version = 12;
        cl->tls = dyn_tls_ctx_client(&o, terr, terr_n);
        if (!cl->tls)
            return -1;
    }
    return 0;
}
#endif

int hc_js_on_connect(const char* ip, void* ud)
{
    hc_onconn_t* h = (hc_onconn_t*)ud;
    JSValueConst argv[1];
    JSValue r;
    int refuse;

    argv[0] = JS_NewString(h->ctx, ip);
    r = JS_Call(h->ctx, h->fn, JS_UNDEFINED, 1, argv);
    JS_FreeValue(h->ctx, argv[0]);
    if (JS_IsException(r)) {
        JS_FreeValue(h->ctx, JS_GetException(h->ctx));
        return 1;
    }
    refuse = !JS_ToBool(h->ctx, r);
    JS_FreeValue(h->ctx, r);
    return refuse;
}

void hc_jobs_orphan(dyn_http_client_t* cl)
{
    hc_job_t* j;
    for (j = cl->jobs; j; j = j->jnext)
        j->cl = NULL;
    cl->jobs = NULL;
}

static void hc_job_free(hc_job_t* j)
{
    JSContext* ctx = j->ctx;
    if (ctx && !JS_IsUndefined(j->on_connect_fn))
        JS_FreeValue(ctx, j->on_connect_fn);
    j->on_connect_fn = JS_UNDEFINED;
    free(j->x.resp.data);
    free(j->method_own);
    free(j->x.url);
    free(j->x.hdr);
    free(j->x.body);
    free(j->ca);
    dyn_cookiejar_unref(j->jar);
    free(j);
}

static void hc_job_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    hc_exchange(&j->x);
}

static void hc_job_run_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    j->x.err = hc_exchange_run(&j->x, &j->conn, 0);
    hc_conn_close(&j->conn);
}

static void hc_job_connect_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    j->x.err = hc_exchange_open(&j->x, &j->conn);
}

static void hc_job_unlink(hc_job_t* j)
{
    hc_job_t** pp;
    if (!j->cl)
        return;
    pp = &j->cl->jobs;
    while (*pp && *pp != j)
        pp = &(*pp)->jnext;
    if (*pp)
        *pp = j->jnext;
    j->jnext = NULL;
}

typedef struct hc_sresp {
    JSContext* ctx;
    hc_job_t* j;
    dyn_bytes_t buf;
    size_t off;
    int done;
    int err;
    int pending;
    char why[224];
    JSValue self, rbuf, rres, rrej;
} hc_sresp_t;

static void hc_async_finish(hc_job_t* j, int err);
static void hc_async_touch(hc_job_t* j);
static void hc_slot_release(hc_job_t* j);

static size_t hc_sr_take(hc_sresp_t* s, uint8_t* dst, size_t n)
{
    size_t have = s->buf.len - s->off;
    if (n > have)
        n = have;
    memcpy(dst, s->buf.data + s->off, n);
    s->off += n;
    if (s->off == s->buf.len) {
        s->off = 0;
        s->buf.len = 0;
        if (s->buf.cap > 65536) {
            free(s->buf.data);
            s->buf.data = NULL;
            s->buf.cap = 0;
        }
    } else if (s->off > 65536 && s->off * 2 > s->buf.len) {
        memmove(s->buf.data, s->buf.data + s->off, s->buf.len - s->off);
        s->buf.len -= s->off;
        s->off = 0;
    }
    return n;
}

static JSValue hc_sr_error(hc_sresp_t* s)
{
    JS_ThrowRangeError(s->ctx, "HTTPBodyStream: %s", s->why[0] ? s->why : hc_err_name(s->err));
    return JS_GetException(s->ctx);
}

static void hc_sr_unpend(hc_sresp_t* s, JSRuntime* rt)
{
    JSValue self = s->self;
    JSValue v[3] = { s->rbuf, s->rres, s->rrej };
    int i;
    s->rbuf = s->rres = s->rrej = s->self = JS_UNDEFINED;
    s->pending = 0;
    for (i = 0; i < 3; i++)
        JS_FreeValueRT(rt, v[i]);
    JS_FreeValueRT(rt, self);
}

static void hc_sr_settle(hc_sresp_t* s)
{
    JSContext* ctx = s->ctx;
    JSValue v, r;
    int rej = 0;
    if (!s->pending)
        return;
    if (s->buf.len > s->off) {
        size_t n = 0;
        uint8_t* base = hc_view_bytes(ctx, s->rbuf, &n, "HTTPBodyStream");
        if (!base) {
            v = JS_GetException(ctx);
            rej = 1;
        } else {
            v = JS_NewInt64(ctx, (int64_t)hc_sr_take(s, base, n));
        }
    } else if (s->err) {
        v = hc_sr_error(s);
        rej = 1;
    } else if (s->done) {
        v = JS_NewInt32(ctx, 0);
    } else {
        return;
    }
    r = JS_Call(ctx, rej ? s->rrej : s->rres, JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, v);
    hc_sr_unpend(s, JS_GetRuntime(ctx));
}

static void hc_sr_end(hc_sresp_t* s, int err, const char* why)
{
    s->j = NULL;
    if (err) {
        s->err = err;
        snprintf(s->why, sizeof s->why, "%s", why && *why ? why : hc_err_name(err));
    } else {
        s->done = 1;
    }
    hc_sr_settle(s);
}

static void hc_sr_dispose(void* native)
{
    hc_sresp_t* s = (hc_sresp_t*)native;
    hc_job_t* j = s->j;
    if (j) {
        j->sr = NULL;
        s->j = NULL;
        atomic_store_explicit(&j->cancelled, 1, memory_order_relaxed);
        hc_async_finish(j, DYN_HTTP_ERR_CANCEL);
    }
    if (s->pending) {
        s->done = 1;
        hc_sr_settle(s);
    }
    free(s->buf.data);
    free(s);
}

static int hc_sr_move(hc_job_t* j)
{
    hc_resp_t* st = &j->arst;
    dyn_bytes_t* r = &j->x.resp;
    hc_sresp_t* s = j->sr;
    size_t from = st->hdr_end, to, n;
    if (st->phase == HC_RESP_TO_CHUNKED)
        to = st->out_pos;
    else if (st->phase == HC_RESP_TO_LENGTH)
        to = r->len < st->want ? r->len : st->want;
    else if (st->phase == HC_RESP_TO_EOF)
        to = r->len;
    else
        to = from;
    if (to <= from)
        return 0;
    n = to - from;
    if (s->off && s->buf.len + n > s->buf.cap) {
        memmove(s->buf.data, s->buf.data + s->off, s->buf.len - s->off);
        s->buf.len -= s->off;
        s->off = 0;
    }
    if (dyn_bytes_reserve(&s->buf, n) < 0)
        return -1;
    memcpy(s->buf.data + s->buf.len, r->data + from, n);
    s->buf.len += n;
    memmove(r->data + from, r->data + to, r->len - to);
    r->len -= n;
    if (st->phase == HC_RESP_TO_LENGTH)
        st->want -= n;
    if (st->phase == HC_RESP_TO_CHUNKED) {
        st->chunk_pos -= n;
        st->out_pos -= n;
    }
    st->dropped += n;
    return 0;
}

static int hc_sr_open(hc_job_t* j)
{
    JSContext* ctx = j->ctx;
    const char* raw = j->x.resp.data;
    size_t he = j->arst.hdr_end, ctl = 0;
    httpk_head_t h;
    httpk_hdr_t small[32], *heap = NULL;
    hc_sresp_t* s;
    JSValue obj, hh, r;
    const char* ct;
    int status;
    if (dyn_head_parse(raw, he, &h, small, 32, &heap) != HTTPK_OK) {
        free(heap);
        return -1;
    }
    s = (hc_sresp_t*)calloc(1, sizeof *s);
    if (!s) {
        free(heap);
        return -1;
    }
    s->ctx = ctx;
    s->self = s->rbuf = s->rres = s->rrej = JS_UNDEFINED;
    if (j->jar)
        dyn_cookiejar_absorb_raw(j->jar, j->x.url, raw, he);
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_hc_stream_class_id, s, hc_sr_dispose);
    if (JS_IsException(obj)) {
        free(heap);
        free(s);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    status = h.status;
    JS_DefinePropertyValueStr(ctx, obj, "status", JS_NewInt32(ctx, status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
        JS_NewStringLen(ctx, raw + h.reason, h.reason_len < 63 ? h.reason_len : 63), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok", JS_NewBool(ctx, status >= 200 && status < 300), JS_PROP_C_W_E);
    hh = dyn_headers_js(ctx, raw, &h);
    if (JS_IsException(hh)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        hh = JS_NewObject(ctx);
    }
    JS_DefinePropertyValueStr(ctx, obj, "headers", hh, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "url", JS_NewString(ctx, j->x.url), JS_PROP_C_W_E);
    ct = httpk_hdr_first(&h, raw, "content-type", &ctl);
    JS_DefinePropertyValueStr(ctx, obj, "contentType",
        JS_NewStringLen(ctx, ct ? ct : "", ct ? ctl : 0), JS_PROP_C_W_E);
    free(heap);
    j->sr = s;
    s->j = j;
    j->a_total_deadline_ms = 0;
    hc_slot_release(j);
    r = JS_Call(ctx, j->resolve, JS_UNDEFINED, 1, (JSValueConst*)&obj);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    j->resolve = j->reject = JS_UNDEFINED;
    return 0;
}

static int hc_sr_step(hc_job_t* j, int rc)
{
    if (!j->arst.hdr_end)
        return 0;
    if (!j->sr) {
        if (rc > 0 && j->arst.phase == HC_RESP_HEAD)
            return 0;
        if (hc_sr_open(j) < 0) {
            hc_async_finish(j, DYN_HTTP_ERR_OOM);
            return 1;
        }
    }
    if (!j->sr)
        return 1;
    if (hc_sr_move(j) < 0) {
        hc_async_finish(j, DYN_HTTP_ERR_OOM);
        return 1;
    }
    if (rc != HC_RESP_MORE) {
        if (rc == DYN_HTTP_ERR_TOOBIG)
            snprintf(j->x.truncwhy, sizeof j->x.truncwhy, "body exceeds maxBodyBytes");
        hc_async_finish(j, rc);
        return 1;
    }
    hc_sr_settle(j->sr);
    return 1;
}

JSValue dyn_hc_stream_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t len = 0;
    uint8_t* base;
    hc_sresp_t* s;
    JSValue funcs[2], promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "HTTPBodyStream.read: buf must be a byte-wide view (Uint8Array)");
    base = hc_view_bytes(ctx, argv[0], &len, "HTTPBodyStream");
    if (!base)
        return JS_EXCEPTION;
    s = (hc_sresp_t*)dyn_res_native(ctx, this_val, dyn_hc_stream_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (s->pending)
        return hc_promise_rejected(ctx,
            JS_ThrowTypeError(ctx, "HTTPBodyStream: a read is already pending"));
    if (len == 0)
        return hc_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (s->buf.len > s->off)
        return hc_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)hc_sr_take(s, base, len)));
    if (s->err)
        return hc_promise_rejected(ctx, hc_sr_error(s));
    if (s->done)
        return hc_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    s->rres = funcs[0];
    s->rrej = funcs[1];
    s->rbuf = JS_DupValue(ctx, argv[0]);
    s->self = JS_DupValue(ctx, this_val);
    s->pending = 1;
    if (s->j)
        hc_async_touch(s->j);
    return promise;
}

static void hc_async_detach(hc_job_t* j);

static void hc_job_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    hc_job_t* j = (hc_job_t*)opaque;
    dyn_http_client_t* cl = j->cl;
    int was_on_reactor = j->on_reactor;

    if (was_on_reactor) {
        hc_no_pump++;
        hc_async_detach(j);
        hc_no_pump--;
    }

    if (cl) {
        JSValue v = JS_UNDEFINED;
        hc_job_unlink(j);
        j->cl = NULL;
        if (--cl->n_async <= 0) {
            v = cl->self_pending;
            cl->self_pending = JS_UNDEFINED;
        }
        if (ctx)
            JS_FreeValue(ctx, v);
        else
            JS_FreeValueRT(rt, v);
    }
    if (j->sr) {
        hc_sresp_t* s = j->sr;
        j->sr = NULL;
        s->j = NULL;
        s->err = DYN_HTTP_ERR_CANCEL;
        snprintf(s->why, sizeof s->why, "HTTP exchange aborted at engine shutdown");
        if (ctx)
            hc_sr_settle(s);
        else if (s->pending)
            hc_sr_unpend(s, rt);
    }
    if (ctx && !JS_IsUndefined(j->reject)) {
        JSValue e = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message",
                JS_NewString(ctx, "HTTP exchange aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            e = JS_GetException(ctx);
        }
        r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }
    if (ctx) {
        JS_FreeValue(ctx, j->resolve);
        JS_FreeValue(ctx, j->reject);
    } else {
        JS_FreeValueRT(rt, j->resolve);
        JS_FreeValueRT(rt, j->reject);
    }
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
    if (was_on_reactor)
        hc_job_free(j);
}

static void hc_job_done(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    JSContext* ctx = j->ctx;
    JSValue v, r;
    JSValueConst a1[1];

    JS_RemoveShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
    if (!j->cl && JS_IsUndefined(j->resolve)) {
        if (j->sr)
            hc_sr_end(j->sr, j->x.err, j->x.truncwhy[0] ? j->x.truncwhy : NULL);
        j->sr = NULL;
        if (j->offloaded && j->stream)
            dyn_http_async_release(j->ctx);
        hc_job_free(j);
        return;
    }

    if (j->cl) {
        hc_job_unlink(j);
        dyn_http_client_unpin(j->cl);
        j->cl = NULL;
    }

    if (j->stream && JS_IsUndefined(j->resolve)) {
        if (j->sr)
            hc_sr_end(j->sr, j->x.err, j->x.truncwhy[0] ? j->x.truncwhy : (j->x.tlswhy[0] ? j->x.tlswhy : NULL));
        j->sr = NULL;
        if (j->offloaded)
            dyn_http_async_release(j->ctx);
        hc_job_free(j);
        return;
    }

    {
        int reject_it = j->x.err != 0;
        if (j->x.err) {
            v = ((j->x.err == DYN_HTTP_ERR_TRUNC || j->x.err == DYN_HTTP_ERR_PARSE) && j->x.truncwhy[0])
                ? hc_error_value_named(ctx, j->x.err, j->x.method, j->x.url,
                      j->x.truncwhy)
                : hc_error_value(ctx, j->x.err, j->x.method, j->x.url,
                      j->x.tlswhy[0] ? j->x.tlswhy : NULL);
        } else {
            if (j->jar)
                dyn_cookiejar_absorb_raw(j->jar, j->x.url,
                    (const char*)j->x.resp.data, j->x.hdr_end);
            v = dyn_build_response_ex(ctx, j->x.resp.data ? (char*)j->x.resp.data : "",
                j->x.resp.len, j->x.hdr_end, j->x.method, j->x.url,
                j->bytes_only);
        }
        if (JS_IsException(v)) {
            v = JS_GetException(ctx);
            reject_it = 1;
        }
        a1[0] = v;
        r = JS_Call(ctx, reject_it ? j->reject : j->resolve, JS_UNDEFINED, 1, a1);
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    if (j->offloaded)
        dyn_http_async_release(j->ctx);
    hc_job_free(j);
}

static void hc_job_connect_done(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;

    if (j->x.err == 0 && j->x.on_connect && j->x.peer_ip[0]
        && j->x.on_connect(j->x.peer_ip, j->x.on_connect_ud) != 0)
        j->x.err = DYN_HTTP_ERR_CONNECT;
    if (j->x.err != 0) {
        hc_conn_close(&j->conn);
        hc_job_done(j);
        return;
    }
    if (j->offloaded && j->aio) {
        dyn_aio_offload(j->aio, hc_job_run_work, hc_job_done, j);
        return;
    }
    j->x.err = hc_exchange_run(&j->x, &j->conn, 0);
    hc_conn_close(&j->conn);
    hc_job_done(j);
}

static _Thread_local hc_job_t* hc_async_jobs;

static _Thread_local hc_pconn_t* hc_idle_conns;

static _Thread_local unsigned hc_idle_count;

static _Thread_local hc_dropper_t hc_droppers[HC_DROPPERS];

static _Thread_local unsigned hc_dropper_next;

static _Thread_local JSRuntime* hc_idle_rt;

static _Thread_local int hc_async_hooked;

typedef struct hc_origin {
    struct hc_origin* next;
    hc_job_t* qhead;
    hc_job_t* qtail;
    unsigned busy;
    int pumping;
    uint16_t port;
    int is_tls;
    char host[256];
} hc_origin_t;

static _Thread_local hc_origin_t* hc_origins;

static void hc_async_sweep(void* unused);

static void hc_async_open(hc_job_t* j);

void hc_async_hook_sync(void)
{
    int want = hc_async_jobs || hc_idle_conns || wsc_hs_any_pending();
    if (want && !hc_async_hooked) {
        if (dyn_net_on_drain(hc_async_sweep, &hc_async_hooked) >= 0)
            hc_async_hooked = 1;
    } else if (!want && hc_async_hooked) {
        hc_async_hooked = 0;
        dyn_net_off_drain(&hc_async_hooked);
    }
}

static void hc_pconn_close(hc_pconn_t* pc)
{
    pc->closing = 1;
    pc->job = NULL;
    if (pc->fd >= 0) {
        int fd = pc->fd;
        pc->fd = -1;
        dyn_aio_close(pc->aio, fd);
    }
#ifdef CONFIG_TLS
    pc->tls = NULL;
    dyn_tls_ctx_free(pc->tls_ctx);
#endif
    free(pc->ca);
    free(pc);
}

static void hc_pool_shutdown(JSContext* ctx, JSRuntime* rt, void* opaque);

static void hc_pool_unlink(hc_pconn_t* pc)
{
    hc_pconn_t** pp = &hc_idle_conns;
    while (*pp && *pp != pc)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    *pp = pc->next;
    pc->next = NULL;
    hc_idle_count--;
    if (!hc_idle_conns) {
        JSRuntime* rt = hc_idle_rt;
        hc_idle_rt = NULL;
        JS_RemoveShutdownSweep(rt, hc_pool_shutdown, NULL);
        dyn_net_reactor_weak_drop(rt);
    }
}

static void hc_pool_shutdown(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    (void)ctx;
    (void)opaque;
    while (hc_idle_conns) {
        hc_pconn_t* pc = hc_idle_conns;
        hc_idle_conns = pc->next;
        hc_idle_count--;
        hc_pconn_close(pc);
    }
    hc_idle_rt = NULL;
    dyn_net_reactor_weak_drop(rt);
    hc_async_hook_sync();
}

static int hc_pool_same_host(const hc_pconn_t* pc, const char* host, uint16_t port, int is_tls)
{
    return pc->port == port && pc->is_tls == is_tls && strcmp(pc->host, host) == 0;
}

static int hc_pool_same_trust(const hc_pconn_t* pc, const char* ca)
{
    return pc->ca ? ca && !strcmp(pc->ca, ca) : !ca;
}

static void hc_pool_put(hc_pconn_t* pc, JSRuntime* rt)
{
    if (hc_idle_count >= HC_POOL_MAX) {
        hc_pconn_t* oldest = hc_idle_conns;
        while (oldest->next)
            oldest = oldest->next;
        hc_pool_unlink(oldest);
        hc_pconn_close(oldest);
    }
    pc->job = NULL;
    pc->idle_deadline_ms = dyn_timer_now_ms() + HC_POOL_IDLE_MS;
    if (!hc_idle_conns) {
        hc_idle_rt = rt;
        dyn_net_reactor_weak_hold();
        JS_AddShutdownSweep(rt, hc_pool_shutdown, NULL);
    }
    pc->next = hc_idle_conns;
    hc_idle_conns = pc;
    hc_idle_count++;
}

static int hc_pconn_alive(const hc_pconn_t* pc)
{
    char b;
    ssize_t n = recv(pc->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0)
        return 0;
    if (n > 0)
        return pc->is_tls;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static hc_dropper_t* hc_dropper_find(const char* host, uint16_t port, int is_tls)
{
    uint64_t now = dyn_timer_now_ms();
    unsigned i;
    for (i = 0; i < HC_DROPPERS; i++) {
        hc_dropper_t* d = &hc_droppers[i];
        if (d->until_ms > now && d->port == port && d->is_tls == is_tls && strcmp(d->host, host) == 0)
            return d;
    }
    return NULL;
}

static void hc_dropper_note(const hc_pconn_t* pc)
{
    hc_dropper_t* d = hc_dropper_find(pc->host, pc->port, pc->is_tls);
    if (!d) {
        d = &hc_droppers[hc_dropper_next++ % HC_DROPPERS];
        d->port = pc->port;
        d->is_tls = pc->is_tls;
        snprintf(d->host, sizeof d->host, "%s", pc->host);
    }
    d->until_ms = dyn_timer_now_ms() + HC_DROPPER_MS;
}

void hc_pool_disown(const void* owner, int close_them)
{
    hc_pconn_t* pc;
again:
    for (pc = hc_idle_conns; pc; pc = pc->next) {
        if (owner && pc->owner == owner) {
            pc->owner = NULL;
            if (close_them) {
                hc_pool_unlink(pc);
                hc_pconn_close(pc);
                goto again;
            }
        }
    }
    if (close_them)
        hc_async_hook_sync();
}

static hc_pconn_t* hc_pool_take(const char* host, uint16_t port, int is_tls, int replayable, const char* ca)
{
    hc_pconn_t* it;
    if (!replayable && hc_dropper_find(host, port, is_tls))
        return NULL;
again:
    for (it = hc_idle_conns; it; it = it->next) {
        if (hc_pool_same_host(it, host, port, is_tls) && hc_pool_same_trust(it, ca)) {
            hc_pool_unlink(it);
            if (it->fd < 0 || !hc_pconn_alive(it)) {
                hc_pconn_close(it);
                goto again;
            }
            return it;
        }
    }
    return NULL;
}

static hc_origin_t* hc_origin_get(const char* host, uint16_t port, int is_tls)
{
    hc_origin_t* o;
    for (o = hc_origins; o; o = o->next)
        if (o->port == port && o->is_tls == is_tls && strcmp(o->host, host) == 0)
            return o;
    o = (hc_origin_t*)calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->port = port;
    o->is_tls = is_tls;
    snprintf(o->host, sizeof o->host, "%s", host);
    o->next = hc_origins;
    hc_origins = o;
    return o;
}

static void hc_origin_drop_if_idle(hc_origin_t* o)
{
    hc_origin_t** pp = &hc_origins;
    if (o->busy || o->qhead || o->pumping)
        return;
    while (*pp && *pp != o)
        pp = &(*pp)->next;
    if (*pp)
        *pp = o->next;
    free(o);
}

static int hc_slot_acquire(hc_job_t* j, const char* host)
{
    hc_origin_t* o;
    if (j->org)
        return 1;
    o = hc_origin_get(host, j->x.u.port, j->x.u.tls ? 1 : 0);
    if (!o)
        return 1;
    j->org = o;
    if (o->busy < j->cap) {
        o->busy++;
        return 1;
    }
    j->queued = 1;
    j->qnext = NULL;
    if (o->qtail)
        o->qtail->qnext = j;
    else
        o->qhead = j;
    o->qtail = j;
    j->astage = HC_ASTAGE_CONNECT;
    j->a_idle_deadline_ms = 0;
    j->a_total_deadline_ms = j->x.timeout_ms > 0
        ? dyn_timer_now_ms() + (uint64_t)j->x.timeout_ms * DYN_HTTP_TOTAL_TIMEOUT_FACTOR
        : 0;
    return 0;
}

static void hc_slot_release(hc_job_t* j)
{
    hc_origin_t* o = j->org;
    if (!o)
        return;
    j->org = NULL;
    if (j->queued) {
        hc_job_t** pp = &o->qhead;
        hc_job_t* prev = NULL;
        while (*pp && *pp != j) {
            prev = *pp;
            pp = &(*pp)->qnext;
        }
        if (*pp)
            *pp = j->qnext;
        if (o->qtail == j)
            o->qtail = prev;
        j->queued = 0;
        j->qnext = NULL;
    } else if (o->busy) {
        o->busy--;
    }
    if (o->pumping || hc_no_pump) {
        hc_origin_drop_if_idle(o);
        return;
    }
    o->pumping = 1;
    while (o->qhead && o->busy < o->qhead->cap) {
        hc_job_t* n = o->qhead;
        o->qhead = n->qnext;
        if (!o->qhead)
            o->qtail = NULL;
        n->qnext = NULL;
        n->queued = 0;
        n->org = NULL;
        if (!HC_CANCELLED(&n->cancelled))
            hc_async_open(n);
    }
    o->pumping = 0;
    hc_origin_drop_if_idle(o);
}

static void hc_async_link(hc_job_t* j)
{
    j->anext = hc_async_jobs;
    hc_async_jobs = j;
    j->on_reactor = 1;
    hc_async_hook_sync();
}

static void hc_async_detach(hc_job_t* j)
{
    hc_job_t** pp = &hc_async_jobs;
    while (*pp && *pp != j)
        pp = &(*pp)->anext;
    if (*pp)
        *pp = j->anext;
    j->anext = NULL;
    j->on_reactor = 0;
    if (j->pc) {
        hc_pconn_t* pc = j->pc;
        j->pc = NULL;
        hc_pconn_close(pc);
    }
    hc_slot_release(j);
    hc_async_hook_sync();
}

static void hc_async_finish(hc_job_t* j, int err)
{
    hc_pconn_t* pc = j->pc;
    if (pc && err == 0 && j->arst.reusable && pc->fd >= 0 && pc->host[0] && !pc->send_failed
        && !HC_CANCELLED(&j->cancelled)) {
        j->pc = NULL;
        pc->owner = j->cl;
        hc_pool_put(pc, JS_GetRuntime(j->ctx));
    }
    hc_async_detach(j);
    j->x.err = err;
    j->x.hdr_end = j->arst.hdr_end;
    j->x.chunked = j->arst.chunked;
    hc_job_done(j);
}

static int hc_method_idempotent(const char* m);

static int hc_hdr_present(const char* hdr, const char* name)
{
    const char* p = hdr;
    while (p && *p) {
        const char* colon = strchr(p, ':');
        const char* eol = strstr(p, "\r\n");
        if (colon && (!eol || colon < eol) && dyn_ci_equal(p, (size_t)(colon - p), name))
            return 1;
        p = eol ? eol + 2 : NULL;
    }
    return 0;
}

static int hc_job_replayable(const hc_job_t* j)
{
    return hc_method_idempotent(j->x.method) || hc_hdr_present(j->x.hdr, "idempotency-key");
}

static int hc_request_never_acked(const hc_pconn_t* pc)
{
    int queued = -1;
    if (!pc || pc->fd < 0 || pc->is_tls || pc->sent_len == 0)
        return 0;
#if defined(__APPLE__)
    {
        socklen_t ql = sizeof queued;
        if (getsockopt(pc->fd, SOL_SOCKET, SO_NWRITE, &queued, &ql) != 0)
            return 0;
    }
#elif defined(__linux__)
    if (ioctl(pc->fd, TIOCOUTQ, &queued) != 0)
        return 0;
#else
    return 0;
#endif
    return queued > 0 && (size_t)queued >= pc->sent_len;
}

static int hc_async_retry_fresh(hc_job_t* j)
{
    hc_pconn_t* pc = j->pc;
    if (!j->a_reused || j->a_retried || j->x.resp.len != 0)
        return 0;
    if (pc && pc->host[0])
        hc_dropper_note(pc);
    if (!hc_job_replayable(j) && !hc_request_never_acked(pc))
        return 0;
    j->a_retried = 1;
    j->a_reused = 0;
    j->x.truncwhy[0] = 0;
    j->pc = NULL;
    if (pc)
        hc_pconn_close(pc);
    hc_async_open(j);
    return 1;
}

static void hc_async_sweep(void* unused)
{
    uint64_t now = dyn_timer_now_ms();
    hc_job_t* j;
    hc_pconn_t* pc;
    (void)unused;
restart:
    for (j = hc_async_jobs; j; j = j->anext) {
        if (HC_CANCELLED(&j->cancelled)) {
            hc_async_finish(j, DYN_HTTP_ERR_CANCEL);
            goto restart;
        }
        if (j->sr && !j->sr->pending)
            continue;
        if ((j->a_idle_deadline_ms && now >= j->a_idle_deadline_ms)
            || (j->a_total_deadline_ms && now >= j->a_total_deadline_ms)) {
            hc_async_finish(j, j->astage == HC_ASTAGE_CONNECT ? DYN_HTTP_ERR_CONNECT : DYN_HTTP_ERR_RECV);
            goto restart;
        }
    }
again:
    for (pc = hc_idle_conns; pc; pc = pc->next) {
        if (now >= pc->idle_deadline_ms) {
            hc_pool_unlink(pc);
            hc_pconn_close(pc);
            goto again;
        }
    }
    wsc_hs_sweep(now);
    hc_async_hook_sync();
}

static void hc_async_touch(hc_job_t* j)
{
    j->a_idle_deadline_ms = j->x.timeout_ms > 0
        ? dyn_timer_now_ms() + (uint64_t)j->x.timeout_ms
        : 0;
}

#ifdef CONFIG_TLS
static void hc_async_tls_fail(hc_job_t* j, const char* why)
{
    snprintf(j->x.tlswhy, sizeof j->x.tlswhy, "%s", why && *why ? why : "handshake failed");
    hc_async_finish(j, DYN_HTTP_ERR_TLS);
}
#endif

static void hc_async_on_sent(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    hc_pconn_t* pc = (hc_pconn_t*)ud;
    hc_job_t* j = pc->job;
    (void)aio;
    (void)buf;
    (void)len;
    if (res >= 0 || pc->closing)
        return;
    pc->send_failed = 1;
    if (pc->sending || !j)
        return;
    if (!hc_async_retry_fresh(j))
        hc_async_finish(j, DYN_HTTP_ERR_SEND);
}

static void hc_async_send_request(hc_job_t* j)
{
    hc_pconn_t* pc = j->pc;
    dyn_bytes_t req = { 0 };
    int err = hc_build_request(&j->x, &req, 1);
    int rc;
    if (err != 0) {
        hc_async_finish(j, err);
        return;
    }
    j->astage = HC_ASTAGE_EXCHANGE;
    j->a_total_deadline_ms = j->x.timeout_ms > 0
        ? dyn_timer_now_ms() + (uint64_t)j->x.timeout_ms * DYN_HTTP_TOTAL_TIMEOUT_FACTOR
        : 0;
    hc_async_touch(j);
    pc->sending = 1;
    pc->send_failed = 0;
    pc->sent_len = req.len;
    rc = dyn_aio_send(pc->aio, pc->fd, req.data, req.len, 0, hc_async_on_sent, pc);
    pc->sending = 0;
    free(req.data);
    if (rc < 0 || pc->send_failed) {
        pc->send_failed = 1;
        if (!hc_async_retry_fresh(j))
            hc_async_finish(j, DYN_HTTP_ERR_SEND);
    }
}

#ifdef CONFIG_TLS
static void hc_async_on_handshake(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    hc_pconn_t* pc = (hc_pconn_t*)ud;
    hc_job_t* j = pc->job;
    (void)aio;
    (void)buf;
    (void)len;
    if (!j || pc->closing)
        return;
    if (res < 0) {
        hc_async_tls_fail(j, dyn_tls_error(pc->tls));
        return;
    }
    hc_async_send_request(j);
}
#endif

static void hc_async_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    hc_pconn_t* pc = (hc_pconn_t*)ud;
    hc_job_t* j = pc->job;
    int eof = 0, tls_cut = 0, rc;
    (void)aio;

    if (pc->closing)
        return;
    if (!j) {
        hc_pool_unlink(pc);
        hc_pconn_close(pc);
        hc_async_hook_sync();
        return;
    }
    if (j->astage == HC_ASTAGE_TLS) {
#ifdef CONFIG_TLS
        hc_async_tls_fail(j, pc->tls ? dyn_tls_error(pc->tls) : NULL);
#else
        hc_async_finish(j, DYN_HTTP_ERR_TLS);
#endif
        return;
    }
    if (res > 0) {
        if (dyn_bytes_reserve(&j->x.resp, len) < 0) {
            hc_async_finish(j, DYN_HTTP_ERR_OOM);
            return;
        }
        memcpy(j->x.resp.data + j->x.resp.len, buf, len);
        j->x.resp.len += len;
        hc_async_touch(j);
    } else {
        if (hc_async_retry_fresh(j))
            return;
        if (res == 0) {
            eof = 1;
            tls_cut = pc->is_tls;
        } else {
#ifdef CONFIG_TLS
            if (pc->is_tls && pc->tls && dyn_tls_peer_closed(pc->tls)) {
                eof = 1;
            } else
#endif
            {
                hc_async_finish(j, DYN_HTTP_ERR_RECV);
                return;
            }
        }
    }
    rc = hc_resp_step(&j->arst, &j->x.resp, eof, tls_cut, j->x.truncwhy,
        sizeof j->x.truncwhy);
    if (j->stream && hc_sr_step(j, rc))
        return;
    if (rc != HC_RESP_MORE)
        hc_async_finish(j, rc);
}

static void hc_async_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    hc_pconn_t* pc = (hc_pconn_t*)ud;
    hc_job_t* j = pc->job;
    (void)buf;
    (void)len;

    if (!j || pc->closing)
        return;
    if (res < 0) {
        hc_async_finish(j, res == -EINVAL ? DYN_HTTP_ERR_RESOLVE : DYN_HTTP_ERR_CONNECT);
        return;
    }
    if (dyn_net_peer_ip(pc->fd, pc->peer_ip, sizeof pc->peer_ip, 1) != 0)
        pc->peer_ip[0] = 0;
    if (j->x.on_connect) {
        snprintf(j->x.peer_ip, sizeof j->x.peer_ip, "%s", pc->peer_ip);
        if (j->x.peer_ip[0] && j->x.on_connect(j->x.peer_ip, j->x.on_connect_ud) != 0) {
            hc_async_finish(j, DYN_HTTP_ERR_CONNECT);
            return;
        }
    }
    hc_async_touch(j);
    if (dyn_aio_recv(aio, pc->fd, 0, 1, hc_async_on_recv, pc) < 0) {
        hc_async_finish(j, DYN_HTTP_ERR_RECV);
        return;
    }
#ifdef CONFIG_TLS
    if (pc->is_tls) {
        char terr[192];
        dyn_tls_opts_t o;
        dyn_tls_conn_t* t;
        memset(&o, 0, sizeof o);
        o.min_version = 12;
        terr[0] = 0;
        pc->tls_ctx = pc->ca ? hc_tls_client_ctx(pc->ca, terr, sizeof terr) : dyn_tls_ctx_client(&o, terr, sizeof terr);
        t = pc->tls_ctx ? dyn_tls_conn_new(pc->tls_ctx, pc->host, terr, sizeof terr) : NULL;
        if (!t) {
            hc_async_tls_fail(j, terr);
            return;
        }
        if (dyn_aio_tls_attach(aio, pc->fd, t, hc_async_on_handshake, pc) < 0) {
            dyn_tls_conn_free(t);
            hc_async_tls_fail(j, NULL);
            return;
        }
        pc->tls = t;
        j->astage = HC_ASTAGE_TLS;
        if (dyn_aio_tls_start(aio, pc->fd) < 0)
            hc_async_tls_fail(j, dyn_tls_error(t));
        return;
    }
#endif
    hc_async_send_request(j);
}

static int hc_async_usable(const hc_job_t* j)
{
#ifdef CONFIG_TLS
    return !j->x.u.tls || j->x.tls_ctx != NULL;
#else
    return !j->x.u.tls;
#endif
}

static int hc_method_idempotent(const char* m)
{
    return dyn_ci_equal(m, strlen(m), "GET") || dyn_ci_equal(m, strlen(m), "HEAD")
        || dyn_ci_equal(m, strlen(m), "OPTIONS");
}

static void hc_async_open(hc_job_t* j)
{
    char hbuf[256];
    const char* host = dyn_host_bare(j->x.u.host, hbuf, sizeof hbuf);
    hc_pconn_t* pc;
    int fd;

    hc_resp_init(&j->arst, j->x.method, j->x.max_body);
    j->arst.keepalive = 1;
    j->x.resp.len = 0;
    j->a_total_deadline_ms = 0;
    hc_async_touch(j);
    if (strlen(host) < sizeof pc->host && !hc_slot_acquire(j, host))
        return;

    if (!j->a_retried && strlen(host) < sizeof pc->host
        && (pc = hc_pool_take(host, j->x.u.port, j->x.u.tls ? 1 : 0, hc_job_replayable(j), j->ca)) != NULL) {
        j->pc = pc;
        pc->job = j;
        j->a_reused = 1;
        if (j->x.on_connect) {
            snprintf(j->x.peer_ip, sizeof j->x.peer_ip, "%s", pc->peer_ip);
            if (j->x.peer_ip[0] && j->x.on_connect(j->x.peer_ip, j->x.on_connect_ud) != 0) {
                hc_async_finish(j, DYN_HTTP_ERR_CONNECT);
                return;
            }
        }
        hc_async_send_request(j);
        return;
    }

    pc = (hc_pconn_t*)calloc(1, sizeof(*pc));
    if (!pc) {
        hc_async_finish(j, DYN_HTTP_ERR_OOM);
        return;
    }
    pc->aio = j->aio;
    pc->fd = -1;
    pc->port = j->x.u.port;
    pc->is_tls = j->x.u.tls ? 1 : 0;
    if (j->ca && !(pc->ca = strdup(j->ca))) {
        free(pc);
        j->pc = NULL;
        hc_async_finish(j, DYN_HTTP_ERR_OOM);
        return;
    }
    if (strlen(host) < sizeof pc->host)
        snprintf(pc->host, sizeof pc->host, "%s", host);
    pc->job = j;
    j->pc = pc;
    j->astage = HC_ASTAGE_CONNECT;
    fd = dyn_aio_connect(j->aio, host, j->x.u.port, hc_async_on_connect, pc);
    if (fd < 0) {
        hc_async_finish(j, errno == EINVAL ? DYN_HTTP_ERR_RESOLVE : DYN_HTTP_ERR_CONNECT);
        return;
    }
    pc->fd = fd;
}

static void hc_async_start(hc_job_t* j)
{
    j->x.tlswhy[0] = 0;
    j->x.peer_ip[0] = 0;
    j->pc = NULL;
    j->a_reused = 0;
    j->a_retried = 0;
    hc_async_link(j);
    hc_async_open(j);
}

JSValue hc_submit_async(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val)
{
    return hc_submit_async_ex(ctx, this_val, method, url_val, body_val, headers_val, 0);
}

JSValue hc_submit_async_ex(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val, int bytes_only)
{
    dyn_http_client_t* cl;
    const char* url = NULL;
    const char* body = NULL;
    size_t body_len = 0;
    char* raw_body = NULL;
    size_t raw_body_len = 0;
    char* hdr = NULL;
    int hdr_err = 0;
    hc_job_t* j;
    JSValue funcs[2], promise;
    struct dyn_aio* aio;

    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");
    if (!dyn_method_valid(method, strlen(method)))
        return JS_ThrowTypeError(ctx, "invalid HTTP method");
    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        if (JS_IsString(body_val)) {
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        } else {
            size_t boff = 0, blen = 0, bpe = 0, ab = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, body_val, &boff, &blen, &bpe);
            if (!JS_IsException(buf)) {
                uint8_t* base;
                if (bpe != 1) {
                    JS_FreeValue(ctx, buf);
                    JS_ThrowTypeError(ctx, "body must be a byte view (Uint8Array, "
                                           "Uint8ClampedArray, Int8Array, DataView) "
                                           "or a string; wider views are not a byte body");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                base = JS_GetArrayBuffer(ctx, &ab, buf);
                JS_FreeValue(ctx, buf);
                if (!base) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                if (boff > ab || blen > ab - boff) {
                    JS_ThrowRangeError(ctx, "body view out of bounds");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                raw_body = (char*)malloc(blen + 1);
                if (!raw_body) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                memcpy(raw_body, base + boff, blen);
                raw_body[blen] = 0;
                raw_body_len = blen;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                body = JS_ToCStringLen(ctx, &body_len, body_val);
            }
        }
        if (!body && !raw_body) {
            if (raw_body)
                free(raw_body);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }
    hdr = dyn_headers_to_string(ctx, headers_val, &hdr_err);
    if (hdr_err) {
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl) {
        free(hdr);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    j = (hc_job_t*)calloc(1, sizeof(*j));
    if (!j)
        goto oom;
    j->ctx = ctx;
    j->on_connect_fn = JS_UNDEFINED;
    {
        int joom;
        hdr = dyn_cookiejar_with_header(cl->jar, url, hdr, &joom);
        if (joom) {
            free(j);
            goto oom;
        }
    }
    j->jar = dyn_cookiejar_ref(cl->jar);
    j->x.hdr = hdr;
    hdr = NULL;
    j->method_own = strdup(method);
    j->x.url = strdup(url);
    j->x.body = NULL;
    if (body || raw_body) {
        j->x.body = (char*)malloc((body ? body_len : raw_body_len) + 1);
        if (j->x.body) {
            if (body) {
                memcpy(j->x.body, body, body_len);
            } else {
                memcpy(j->x.body, raw_body, raw_body_len);
            }
            j->x.body[body ? body_len : raw_body_len] = 0;
        }
    }
    if (!j->method_own || !j->x.url || ((body || raw_body) && !j->x.body)) {
        hc_job_free(j);
        if (raw_body)
            free(raw_body);
        goto oom;
    }
    j->x.method = j->method_own;
    j->bytes_only = bytes_only == 1;
    j->stream = bytes_only == 2;
    j->x.body_len = body ? body_len : raw_body_len;
    if (dyn_parse_url(j->x.url, &j->x.u) < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_URL, method, url, NULL);
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return e;
    }
#ifdef CONFIG_TLS
    if (j->x.u.tls) {
        char terr[192];
        if (hc_tls_ensure(ctx, cl, terr, sizeof terr) < 0) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, method, url, terr);
            hc_job_free(j);
            if (body)
                JS_FreeCString(ctx, body);
            if (raw_body)
                free(raw_body);
            JS_FreeCString(ctx, url);
            return e;
        }
        j->x.tls_ctx = cl->tls;
    }
#else
    if (j->x.u.tls) {
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return dyn_tls_missing(ctx, "HTTPClient", "https");
    }
#endif
    j->x.timeout_ms = cl->timeout_ms;
    j->cap = cl->origin_cap ? cl->origin_cap : HC_ORIGIN_MAX;
    if (cl->ca && !(j->ca = strdup(cl->ca))) {
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_ThrowOutOfMemory(ctx);
    }
    j->x.max_body = cl->max_body;
    {
        JSValue oc = JS_GetPropertyStr(ctx, this_val, "onConnect");
        if (JS_IsFunction(ctx, oc)) {
            j->on_connect_fn = oc;
            j->onconn.ctx = ctx;
            j->onconn.fn = oc;
            j->x.on_connect = hc_js_on_connect;
            j->x.on_connect_ud = &j->onconn;
        } else {
            JS_FreeValue(ctx, oc);
        }
    }

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return promise;
    }
    j->resolve = funcs[0];
    j->reject = funcs[1];
    atomic_store_explicit(&j->cancelled, 0, memory_order_relaxed);
    j->x.cancel = &j->cancelled;
    j->jnext = cl->jobs;
    cl->jobs = j;
    j->cl = cl;
    cl->n_async++;
    if (cl->n_async == 1)
        cl->self_pending = JS_DupValue(ctx, this_val);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);

    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) {
        if (j->stream) {
            j->x.err = DYN_HTTP_ERR_CONNECT;
            hc_job_done(j);
            return promise;
        }
        if (j->x.on_connect)
            hc_job_connect_done(j);
        else {
            hc_job_work(j);
            hc_job_done(j);
        }
        return promise;
    }
    dyn_http_async_hook(ctx);
    j->offloaded = 1;
    j->aio = aio;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
    if (hc_async_usable(j)) {
        hc_async_start(j);
        return promise;
    }
    if (j->stream) {
        j->x.err = DYN_HTTP_ERR_TLS;
        hc_job_done(j);
        return promise;
    }
    if (j->x.on_connect)
        dyn_aio_offload(aio, hc_job_connect_work, hc_job_connect_done, j);
    else
        dyn_aio_offload(aio, hc_job_work, hc_job_done, j);
    return promise;

oom:
    free(hdr);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);
    return JS_ThrowOutOfMemory(ctx);
}


#endif
