#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_HTTP_DEFAULT_MAX_BODY (16 * 1024 * 1024)
#define DYN_ACONN_MAX_REQS 100000
#define DYN_APP_GZIP_MIN 256

typedef struct {
    dyn_app_conn_t* c;
    int status;
    char* ctype;
    char* extra;
    uint8_t* body;
    size_t body_len;
    uint8_t* gz;
    size_t gz_len;
} dyn_app_gz_job_t;

void dyn_http_client_unpin(dyn_http_client_t* cl)
{
    JSContext* ctx = cl->ctx;
    if (--cl->n_async > 0)
        return;
    {
        JSValue v = cl->self_pending;
        cl->self_pending = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static void dyn_http_client_dispose(void* native)
{
    dyn_http_client_t* c = (dyn_http_client_t*)native;
    if (c) {
        JSValue v = c->self_pending;
        c->self_pending = JS_UNDEFINED;
        hc_jobs_orphan(c);
        hc_pool_disown(c, 0);
        JS_FreeValue(c->ctx, v);
#ifdef CONFIG_TLS
        dyn_tls_ctx_free(c->tls);
#endif
        free(c->ca);
        dyn_cookiejar_unref(c->jar);
    }
    free(native);
}

static int hc_ctor_meta(JSContext* ctx, int argc, JSValueConst* argv, unsigned* cap)
{
    static const char* const keys[] = { "dyna" };
    static const char* const meta_keys[] = { "maxPerOrigin" };
    JSValue mv, v;
    double d;
    *cap = 0;
    if (dyn_opts_bag(ctx, argc, argv, 1, keys, 1, "HTTPClient(maxBody, opts)"))
        return -1;
    if (argc < 2 || JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]))
        return 0;
    mv = JS_GetPropertyStr(ctx, argv[1], "dyna");
    if (JS_IsException(mv))
        return -1;
    if (JS_IsUndefined(mv))
        return 0;
    if (!JS_IsObject(mv) || JS_IsFunction(ctx, mv)) {
        JS_FreeValue(ctx, mv);
        JS_ThrowTypeError(ctx, "HTTPClient: dyna must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, mv, meta_keys, 1)) {
        JS_FreeValue(ctx, mv);
        return -1;
    }
    v = JS_GetPropertyStr(ctx, mv, "maxPerOrigin");
    JS_FreeValue(ctx, mv);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "HTTPClient: dyna.maxPerOrigin must be a number");
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    if (isinf(d) && d > 0) {
        *cap = UINT_MAX;
        return 0;
    }
    if (!(d >= 1.0 && d <= 2147483647.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "HTTPClient: dyna.maxPerOrigin is an integer in [1, 2^31-1] or Infinity");
        return -1;
    }
    *cap = (unsigned)d;
    return 0;
}

JSValue dyn_http_client_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    int64_t max_body = 0;
    unsigned cap = 0;

    if (hc_ctor_meta(ctx, argc, argv, &cap))
        return JS_EXCEPTION;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt64(ctx, &max_body, argv[0]))
            return JS_EXCEPTION;
        if (max_body < 0)
            max_body = 0;
    }
    cl = (dyn_http_client_t*)calloc(1, sizeof(*cl));
    if (!cl)
        return JS_ThrowOutOfMemory(ctx);
    cl->timeout_ms = DYN_HTTP_DEFAULT_TIMEOUT_MS;
    cl->max_body = max_body ? (size_t)max_body : DYN_HTTP_DEFAULT_MAX_BODY;
    cl->origin_cap = cap;
    cl->self_pending = JS_UNDEFINED;
    cl->ctx = ctx;
    return dyn_res_wrap(ctx, new_target, dyn_http_client_class_id, cl,
        dyn_http_client_dispose);
}

int dyn_read_response(hc_conn_t* conn, const char* method,
    size_t max_body, dyn_bytes_t* resp,
    size_t* phdr_end, int* pchunked,
    const _Atomic int* cancel,
    char* truncwhy, size_t truncwhy_n, int* preusable)
{
    hc_resp_t st;
    int eof = 0;

    hc_resp_init(&st, method, max_body);
    for (;;) {
        ssize_t r;
        int rc;
        if (HC_CANCELLED(cancel))
            return DYN_HTTP_ERR_CANCEL;
        if (dyn_bytes_reserve(resp, hc_resp_room(&st, resp)) < 0)
            return DYN_HTTP_ERR_OOM;
        rc = hc_resp_step(&st, resp, eof, conn->tls_cut, truncwhy, truncwhy_n);
        *phdr_end = st.hdr_end;
        *pchunked = st.chunked;
        if (rc != HC_RESP_MORE) {
            *preusable = rc == 0 && st.reusable;
            return rc;
        }
        r = hc_recv(conn, resp->data + resp->len, resp->cap - resp->len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return DYN_HTTP_ERR_RECV;
        }
        if (r == 0)
            eof = 1;
        else
            resp->len += (size_t)r;
    }
}

JSValue dyn_http_client_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return hc_submit_async_ex(ctx, this_val, "GET", argc > 0 ? argv[0] : JS_UNDEFINED, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, 0);
}

JSValue dyn_http_client_post(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return hc_submit_async_ex(ctx, this_val, "POST", argc > 0 ? argv[0] : JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED,
        argc > 2 ? argv[2] : JS_UNDEFINED, 0);
}

JSValue dyn_http_client_request(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "request(method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = hc_submit_async_ex(ctx, this_val, method, argv[1],
        argc > 2 ? argv[2] : JS_UNDEFINED,
        argc > 3 ? argv[3] : JS_UNDEFINED, 0);
    JS_FreeCString(ctx, method);
    return res;
}

JSValue dyn_http_client_request_pending(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "HTTPClient: the pending request needs (method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = hc_submit_async_ex(ctx, this_val, method, argv[1],
        argc > 2 ? argv[2] : JS_UNDEFINED,
        argc > 3 ? argv[3] : JS_UNDEFINED,
        argc > 4 && JS_ToBool(ctx, argv[4]));
    JS_FreeCString(ctx, method);
    return res;
}

JSValue dyn_http_client_set_timeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    int64_t ms;

    (void)argc;
    if (JS_ToInt64(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    cl->timeout_ms = ms;
    return JS_UNDEFINED;
}

JSValue dyn_http_client_get_stream(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getStream(url[, headers])");
    return hc_submit_async_ex(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, 2);
}

JSValue dyn_http_client_disconnect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    (void)argc;
    (void)argv;
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    {
        hc_job_t* j;
        for (j = cl->jobs; j; j = j->jnext)
            atomic_store_explicit(&j->cancelled, 1, memory_order_relaxed);
    }
    hc_pool_disown(cl, 1);
    return JS_UNDEFINED;
}

int dyn_http_pump(dyn_bytes_t* in, dyn_bytes_t* out, int* pnreq,
    size_t* pscan,
    const dyn_route_t* routes, size_t n_routes,
    const dyn_route_idx_t* route_idx, size_t max_req)
{
    char path[2048];

    for (;;) {
        const char* base = in->data;
        size_t avail = in->len;
        size_t from = *pscan < avail ? *pscan : avail;
        const char* hdr_end;
        size_t head_len, body_len = 0, req_total, clv_len = 0, connv_len = 0;
        const char *cl, *conn;
        int http11, keep_alive, head_only = 0;
        const dyn_route_t* route;
        dyn_reqinfo_t ri;

        if (out->len > DYN_HTTP_OUTBOUND_MAX)
            return 0;

        hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        if (!hdr_end) {
            *pscan = avail >= 3 ? avail - 3 : 0;
            if (avail > max_req) {
                dyn_http_format(out, 431, "text/plain",
                    "Request Header Fields Too Large", 31, 0, head_only);
                return 1;
            }
            return 0;
        }
        head_len = (size_t)(hdr_end - base) + 4;

        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src != 0 || ri.head_len == 0 || ri.head_len > (size_t)(hdr_end - base) + 4) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
        }
        head_len = ri.head_len;
        http11 = ri.http11;
        head_only = ri.method_len == 4 && memcmp(ri.method, "HEAD", 4) == 0;

        if (head_len > max_req) {
            dyn_http_format(out, 431, "text/plain",
                "Request Header Fields Too Large", 31, 0, head_only);
            return 1;
        }

        if (ri.has_te) {
            dyn_http_format(out, 501, "text/plain", "Not Implemented", 15, 0, head_only);
            return 1;
        }
        {
            if (ri.cl_count > 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
            if (http11 && ri.host_count != 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
        }
        cl = ri.cl;
        clv_len = ri.cl_len;
        if (cl) {
            size_t j;
            if (clv_len == 0 || clv_len > 19) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
            for (j = 0; j < clv_len; j++) {
                if (cl[j] < '0' || cl[j] > '9') {
                    dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                    return 1;
                }
                if (body_len > (SIZE_MAX - 9) / 10) {
                    dyn_http_format(out, 413, "text/plain", "Content Too Large", 17, 0, head_only);
                    return 1;
                }
                body_len = body_len * 10 + (size_t)(cl[j] - '0');
            }
        }
        req_total = head_len + body_len;
        if (req_total > max_req) {
            dyn_http_format(out, 413, "text/plain", "Content Too Large", 17, 0, head_only);
            return 1;
        }
        if (avail < req_total)
            return 0;

        conn = ri.conn;
        connv_len = ri.conn_len;
        keep_alive = http11;
        if (httpk_list_has(conn, connv_len, "close"))
            keep_alive = 0;
        else if (httpk_list_has(conn, connv_len, "keep-alive"))
            keep_alive = 1;
        if (++(*pnreq) >= DYN_ACONN_MAX_REQS)
            keep_alive = 0;

        if (ri.bad_target) {
            dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
            return 1;
        }
        if (ri.target_len >= sizeof(path)) {
            dyn_http_format(out, 414, "text/plain", "URI Too Long", 12, 0, head_only);
            return 1;
        }
        {
            size_t plen = ri.target_len;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
        }
        route = dyn_route_lookup(routes, n_routes, route_idx, path);
        if (route) {
            if (dyn_http_format(out, route->status, route->content_type,
                    route->body, route->body_len, keep_alive, head_only)
                < 0)
                return 1;
        } else {
            if (dyn_http_format(out, 404, "text/plain", "Not Found", 9,
                    keep_alive, head_only)
                < 0)
                return 1;
        }

        memmove(in->data, in->data + req_total, in->len - req_total);
        in->len -= req_total;
        *pscan = 0;
        if (!keep_alive)
            return 1;
    }
}

static int dyn_app_ct_packed(const char* ct)
{
    static const char* const packed[] = { "application/zip", "application/gzip",
        "application/x-gzip", "application/zstd", "application/x-7z-compressed",
        "application/x-bzip2", "application/x-xz", "font/woff", "font/woff2" };
    size_t k;

    if (strncasecmp(ct, "image/", 6) == 0)
        return strncasecmp(ct + 6, "svg", 3) != 0;
    if (strncasecmp(ct, "video/", 6) == 0 || strncasecmp(ct, "audio/", 6) == 0)
        return 1;
    for (k = 0; k < countof(packed); k++) {
        size_t l = strlen(packed[k]);
        if (strncasecmp(ct, packed[k], l) == 0 && (ct[l] == '\0' || ct[l] == ';' || ct[l] == ' '))
            return 1;
    }
    return 0;
}

int dyn_app_gz_wanted(const dyn_app_conn_t* c, int status, const char* ctype,
    size_t body_len)
{
    if ((status >= 100 && status < 200) || status == 204 || status == 304)
        return 0;
    if (!c->app->compress || !c->accept_gzip)
        return 0;
    if (c->identity_refused)
        return 1;
    return body_len >= DYN_APP_GZIP_MIN && !dyn_app_ct_packed(ctype);
}

static void dyn_app_gz_work(void* arg)
{
    dyn_app_gz_job_t* j = (dyn_app_gz_job_t*)arg;
    if (dyn_gzip_build(j->body, j->body_len, &j->gz, &j->gz_len) != 0) {
        j->gz = NULL;
        j->gz_len = 0;
    }
}

static void dyn_app_gz_done(void* arg)
{
    dyn_app_gz_job_t* j = (dyn_app_gz_job_t*)arg;
    dyn_app_conn_t* c = j->c;

    c->resp_parked--;
    if (c->closed)
        free(j->gz);
    else
        dyn_app_send_built(c, j->status, j->ctype, (const char*)j->body, j->body_len,
            j->extra, j->gz, j->gz_len);
    free(j);
    if (!c->closed)
        dyn_app_pump_resume(c);
    dyn_app_conn_unref(c);
}

int dyn_app_gz_offload(dyn_app_conn_t* c, int status, const char* ctype,
    const char* body, size_t body_len, const char* extra)
{
    size_t cl = strlen(ctype) + 1, xl = extra ? strlen(extra) + 1 : 0;
    dyn_app_gz_job_t* j;
    char* p;

    if (!dyn_aio_cpu_room(c->app->aio))
        return -1;
    if (body_len > SIZE_MAX - sizeof(*j) - cl - xl)
        return -1;
    j = (dyn_app_gz_job_t*)malloc(sizeof(*j) + cl + xl + body_len);
    if (!j)
        return -1;
    p = (char*)(j + 1);
    j->c = c;
    j->status = status;
    j->ctype = p;
    memcpy(p, ctype, cl);
    p += cl;
    j->extra = xl ? p : NULL;
    if (xl)
        memcpy(p, extra, xl);
    p += xl;
    j->body = (uint8_t*)p;
    memcpy(p, body, body_len);
    j->body_len = body_len;
    j->gz = NULL;
    j->gz_len = 0;
    c->refs++;
    c->resp_parked++;
    if (dyn_aio_offload_cpu(c->app->aio, dyn_app_gz_work, dyn_app_gz_done, j) != 0) {
        c->resp_parked--;
        c->refs--;
        free(j);
        return -1;
    }
    return 0;
}

static JSValue dyn_app_read_noop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)data;
    return JS_UNDEFINED;
}

static int dyn_read_is_promise(JSContext* ctx, JSValueConst pr)
{
    JSPromiseStateEnum ps = JS_PromiseState(ctx, pr);
    return ps == JS_PROMISE_PENDING || ps == JS_PROMISE_FULFILLED || ps == JS_PROMISE_REJECTED;
}

static int dyn_app_proto_then(JSContext* ctx, JSValueConst pr,
    JSValueConst onres, JSValueConst onrej)
{
    JSValue g, pcons, pproto, pthen, tr;
    JSValueConst a[2];
    int ret = -1;

    a[0] = onres;
    a[1] = onrej;
    g = JS_GetGlobalObject(ctx);
    pcons = JS_GetPropertyStr(ctx, g, "Promise");
    JS_FreeValue(ctx, g);
    if (JS_IsException(pcons)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    pproto = JS_GetPropertyStr(ctx, pcons, "prototype");
    if (JS_IsException(pproto)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, pcons);
        return -1;
    }
    pthen = JS_GetPropertyStr(ctx, pproto, "then");
    JS_FreeValue(ctx, pproto);
    if (JS_IsException(pthen)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, pcons);
        return -1;
    }
    if (JS_IsFunction(ctx, pthen)) {
        tr = JS_Call(ctx, pthen, pr, 2, a);
        if (!JS_IsException(tr)) {
            JS_FreeValue(ctx, tr);
            ret = 0;
            goto out;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, tr);
        if (JS_DefinePropertyValueStr(ctx, pr, "constructor",
                JS_DupValue(ctx, pcons),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)
            >= 0) {
            tr = JS_Call(ctx, pthen, pr, 2, a);
            if (!JS_IsException(tr)) {
                JS_FreeValue(ctx, tr);
                ret = 0;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, tr);
            }
        }
    }
out:
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, pcons);
    return ret;
}

int dyn_app_read_attach(JSContext* ctx, JSValueConst pr,
    JSValueConst onres, JSValueConst onrej)
{
    JSValue thenf, tr;
    JSValueConst a[2];

    if (!JS_IsObject(pr))
        return 0;
    a[0] = onres;
    a[1] = onrej;
    if (dyn_read_is_promise(ctx, pr)) {
        if (dyn_app_proto_then(ctx, pr, onres, onrej) == 0)
            return 0;
    }
    thenf = JS_GetPropertyStr(ctx, pr, "then");
    if (JS_IsException(thenf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    if (!JS_IsFunction(ctx, thenf)) {
        JS_FreeValue(ctx, thenf);
        return -1;
    }
    tr = JS_Call(ctx, thenf, pr, 2, a);
    JS_FreeValue(ctx, thenf);
    if (JS_IsException(tr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JS_FreeValue(ctx, tr);
    return 0;
}

void dyn_app_read_abandon(JSContext* ctx, JSValueConst pr)
{
    JSValue onres, onrej;
    if (!JS_IsObject(pr))
        return;
    onres = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 0, 0, NULL);
    onrej = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 1, 0, NULL);
    if (!JS_IsException(onres) && !JS_IsException(onrej))
        dyn_app_read_attach(ctx, pr, onres, onrej);
    else {
        if (JS_IsException(onres))
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (JS_IsException(onrej))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, onres);
    JS_FreeValue(ctx, onrej);
}

const char* dyn_app_content_type(const char* path)
{
    const char* d = strrchr(path, '.');
    if (!d)
        return "application/octet-stream";
    if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm"))
        return "text/html";
    if (!strcasecmp(d, ".js") || !strcasecmp(d, ".mjs"))
        return "text/javascript";
    if (!strcasecmp(d, ".css"))
        return "text/css";
    if (!strcasecmp(d, ".json"))
        return "application/json";
    if (!strcasecmp(d, ".txt"))
        return "text/plain";
    if (!strcasecmp(d, ".csv"))
        return "text/csv";
    if (!strcasecmp(d, ".png"))
        return "image/png";
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(d, ".gif"))
        return "image/gif";
    if (!strcasecmp(d, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(d, ".ico"))
        return "image/x-icon";
    if (!strcasecmp(d, ".wasm"))
        return "application/wasm";
    return "application/octet-stream";
}

void dyn_app_pump_resume(dyn_app_conn_t* c)
{
    if (c->closed || c->in_process || c->close_after || c->resp_parked || c->st || c->up || c->is_ws || c->is_sse)
        return;
    if (dyn_iobuf_rlen(&c->in) == 0)
        return;
    dyn_iobuf_ensure_nul(&c->in);
    dyn_app_process(c);
    if (!c->closed)
        dyn_iobuf_compact(&c->in);
}
JSValue dyn_http_client_redirect_plan(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t status = 0, hop = 0;
    const char *method = NULL, *cur = NULL, *next = NULL;
    httpk_redirect_t plan;
    JSValue ret = JS_EXCEPTION;
    int rc;
    (void)this_val;
    if (argc < 5 || JS_ToInt32(ctx, &status, argv[0]) || JS_ToInt32(ctx, &hop, argv[4]))
        return JS_ThrowTypeError(ctx, "redirect plan: (status, method, url, next, hop)");
    method = JS_ToCString(ctx, argv[1]);
    cur = JS_ToCString(ctx, argv[2]);
    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3]))
        next = JS_ToCString(ctx, argv[3]);
    if (!method || !cur)
        goto out;
    rc = httpk_redirect_plan(status, method, cur, next, hop, 20,
        HTTPK_REDIR_ALLOW_DOWNGRADE | HTTPK_REDIR_ALLOW_SELF, &plan);
    if (rc == HTTPK_REDIR_NONE) {
        ret = JS_NULL;
    } else if (rc == HTTPK_REDIR_E_TOO_MANY) {
        JS_ThrowTypeError(ctx, "fetch: too many redirects (more than 20)");
    } else if (rc != HTTPK_REDIR_FOLLOW) {
        JS_ThrowTypeError(ctx, "fetch: redirect to a non-HTTP URL is refused");
    } else {
        ret = JS_NewObject(ctx);
        if (!JS_IsException(ret)) {
            JS_DefinePropertyValueStr(ctx, ret, "method",
                JS_NewString(ctx, plan.method), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ret, "dropBody",
                JS_NewBool(ctx, plan.drop_body), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ret, "stripCredentials",
                JS_NewBool(ctx, plan.strip_credentials), JS_PROP_C_W_E);
        }
    }
out:
    if (method)
        JS_FreeCString(ctx, method);
    if (cur)
        JS_FreeCString(ctx, cur);
    if (next)
        JS_FreeCString(ctx, next);
    return ret;
}
#endif
