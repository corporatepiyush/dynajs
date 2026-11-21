#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_APP_OUT_HIGH (1024 * 1024)

static inline size_t dyn_put_u64(char* p, uint64_t v)
{
    char tmp[20];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++)
        p[i] = tmp[n - 1 - i];
    return (size_t)n;
}

#define DYN_APP_SMALL_RESP 1024
#define DYN_APP_GZIP_OFFLOAD ((size_t)8 * 1024)

JSValue dyn_build_response(JSContext* ctx, const char* raw, size_t len,
    size_t hdr_end, const char* method, const char* url)
{
    return dyn_build_response_ex(ctx, raw, len, hdr_end, method, url, 0);
}

JSValue dyn_build_response_ex(JSContext* ctx, const char* raw, size_t len,
    size_t hdr_end, const char* method, const char* url, int bytes_only)
{
    JSValue obj, headers;
    httpk_head_t h;
    httpk_hdr_t small[32], *heap = NULL;
    char why[160], what[200];
    const char* body;
    size_t body_len;
    int rc;

    why[0] = 0;
    rc = hdr_end <= len ? dyn_head_parse(raw, hdr_end, &h, small, 32, &heap)
                        : HTTPK_E_STATUS;
    if (rc != HTTPK_OK) {
        dyn_head_why(raw, &h, rc, why, sizeof why);
        free(heap);
        snprintf(what, sizeof what, "%s: %s", hc_err_name(DYN_HTTP_ERR_PARSE), why);
        return dyn_http_throw_named(ctx, DYN_HTTP_ERR_PARSE, method, url, what);
    }
    headers = dyn_headers_js(ctx, raw, &h);
    free(heap);
    if (JS_IsException(headers))
        return JS_EXCEPTION;
    body = raw + hdr_end;
    body_len = len - hdr_end;
    if (httpk_bodyless(h.status, method)) {
        body = "";
        body_len = 0;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, headers);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "status", JS_NewInt32(ctx, h.status),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
        JS_NewStringLen(ctx, raw + h.reason, h.reason_len),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok",
        JS_NewBool(ctx, h.status >= 200 && h.status < 300),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "headers", headers, JS_PROP_C_W_E);
    if (!bytes_only)
        JS_DefinePropertyValueStr(ctx, obj, "body",
            JS_NewStringLen(ctx, body, body_len),
            JS_PROP_C_W_E);
    {
        JSValue bb = JS_NewArrayBufferCopy(ctx, (const uint8_t*)body, body_len);
        if (JS_IsException(bb)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, obj, "bodyBytes", bb, JS_PROP_C_W_E);
    }
    return obj;
}

const char* dyn_reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 415:
        return "Unsupported Media Type";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return status < 200 ? "Continue"
            : status < 300  ? "OK"
            : status < 400  ? "Found"
            : status < 500  ? "Bad Request"
                            : "Internal Server Error";
    }
}

int dyn_http_format(dyn_bytes_t* out, int status, const char* ct,
    const char* body, size_t body_len, int keep_alive, int head_only)
{
    char head[512];
    int n;
    const char* date = status >= 200 ? dyn_http_date(NULL, 0) : "";
    if ((status >= 100 && status < 200) || status == 204 || status == 304) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n%sConnection: %s\r\n\r\n",
            status, dyn_reason_phrase(status), date,
            keep_alive ? "keep-alive" : "close");
        if (n < 0 || n >= (int)sizeof(head))
            return -1;
        if (dyn_bytes_append(out, head, (size_t)n) < 0)
            return -1;
        return 0;
    }
    if (status == 200 && keep_alive && ct && strcmp(ct, "text/plain") == 0) {
        static const char sl[] = "HTTP/1.1 200 OK\r\n";
        static const char pre[] = "Content-Type: text/plain\r\nContent-Length: ";
        static const char post[] = "\r\nConnection: keep-alive\r\n\r\n";
        size_t dl = strlen(date), k = sizeof(sl) - 1;
        memcpy(head, sl, k);
        memcpy(head + k, date, dl);
        k += dl;
        memcpy(head + k, pre, sizeof(pre) - 1);
        k += sizeof(pre) - 1;
        k += (size_t)dyn_put_u64(head + k, (uint64_t)body_len);
        memcpy(head + k, post, sizeof(post) - 1);
        n = (int)(k + sizeof(post) - 1);
    } else {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n"
            "%s"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            status, dyn_reason_phrase(status), date, ct ? ct : "text/plain",
            body_len, keep_alive ? "keep-alive" : "close");
        if (n < 0 || n >= (int)sizeof(head))
            return -1;
    }
    if (dyn_bytes_append(out, head, (size_t)n) < 0)
        return -1;
    if (!head_only && body_len > 0 && dyn_bytes_append(out, body, body_len) < 0)
        return -1;
    return 0;
}

void dyn_app_out_account(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    size_t q = dyn_aio_queued(app->aio, c->fd);
    app->out_bytes += q;
    app->out_bytes -= c->out_q;
    c->out_q = q;
}

int dyn_app_out_blocked(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;

    dyn_app_out_account(c);
    if (!c->file_inflight && c->out_q < DYN_APP_OUT_HIGH && app->out_bytes < DYN_APP_OUT_TOTAL)
        return 0;
    if (!c->out_stalled) {
        c->out_stalled = 1;
        c->stall_ms = dyn_timer_now_ms();
        c->stall_q = c->out_q;
    }
    return 1;
}

void dyn_app_out_unstall(dyn_app_conn_t* c)
{
    c->out_stalled = 0;
    c->stall_ms = 0;
    c->stall_q = 0;
}

static int dyn_app_out_ready(dyn_app_conn_t* c)
{
    return !c->file_inflight && c->out_q < DYN_APP_OUT_LOW && c->app->out_bytes < DYN_APP_OUT_TOTAL;
}

void dyn_app_out_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    (void)buf;
    (void)len;
    dyn_app_out_account(c);
    if (res < 0)
        dyn_app_conn_close(c);
    else if (c->out_stalled && !c->closed && dyn_app_out_ready(c)) {
        dyn_app_out_unstall(c);
        dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

void dyn_app_out_file_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    ((dyn_app_conn_t*)udata)->file_inflight = 0;
    dyn_app_out_last(aio, res, buf, len, udata);
}

void dyn_app_out_last(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_out_account(c);
    if (res < 0 || (c->close_after && !c->linger && !c->file_inflight))
        dyn_app_conn_close(c);
    else if (c->out_stalled && !c->closed && dyn_app_out_ready(c)) {
        dyn_app_out_unstall(c);
        dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

static size_t dyn_app_head_cat(char* p, const char* s, size_t n)
{
    memcpy(p, s, n);
    return n;
}

static int dyn_app_head_plain(char* head, size_t head_cap, int status, const char* date,
    const char* extra, size_t extra_len, const char* ctype, size_t slen, const char* conn_tok)
{
    const char* reason = dyn_reason_phrase(status);
    size_t rl = strlen(reason), dl = strlen(date), cl = strlen(ctype), tl = strlen(conn_tok);
    size_t need = 9 + 3 + 1 + rl + 2 + dl + extra_len + 14 + cl + 2 + 16 + 20 + 2 + 12 + tl + 4;
    size_t n = 0;

    if (status < 100 || status > 999 || need >= head_cap)
        return snprintf(head, head_cap,
            "HTTP/1.1 %d %s\r\n%s%sContent-Type: %s\r\n"
            "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
            status, reason, date, extra ? extra : "", ctype, slen, conn_tok);
    n += dyn_app_head_cat(head + n, "HTTP/1.1 ", 9);
    head[n++] = (char)('0' + status / 100);
    head[n++] = (char)('0' + status / 10 % 10);
    head[n++] = (char)('0' + status % 10);
    head[n++] = ' ';
    n += dyn_app_head_cat(head + n, reason, rl);
    n += dyn_app_head_cat(head + n, "\r\n", 2);
    n += dyn_app_head_cat(head + n, date, dl);
    if (extra_len)
        n += dyn_app_head_cat(head + n, extra, extra_len);
    n += dyn_app_head_cat(head + n, "Content-Type: ", 14);
    n += dyn_app_head_cat(head + n, ctype, cl);
    n += dyn_app_head_cat(head + n, "\r\nContent-Length: ", 18);
    n += dyn_put_u64(head + n, (uint64_t)slen);
    n += dyn_app_head_cat(head + n, "\r\nConnection: ", 14);
    n += dyn_app_head_cat(head + n, conn_tok, tl);
    n += dyn_app_head_cat(head + n, "\r\n\r\n", 4);
    return (int)n;
}

void dyn_app_send_built(dyn_app_conn_t* c, int status,
    const char* ctype, const char* body,
    size_t body_len, const char* extra, uint8_t* gz, size_t gz_len)
{
    static const char hdr200_s[] = "HTTP/1.1 200 OK\r\n";
    static const char hdr200_a[] = "Content-Type: application/json\r\nContent-Length: ";
    static const char hdr_b[] = "\r\nConnection: keep-alive\r\n\r\n";
    char head_fixed[512];
    char* head = head_fixed;
    size_t head_cap = sizeof(head_fixed);
    size_t extra_len = extra ? strlen(extra) : 0;
    const char* sbody = body;
    size_t slen = body_len;
    size_t wlen;
    int n, compressed = 0;
    int no_body = (status >= 100 && status < 200) || status == 204 || status == 304;
    const char* conn_tok = dyn_app_conn_token(c);
    const char* date = status >= 200 ? dyn_http_date(extra, extra_len) : "";

    if (extra_len > 64) {
        head_cap = extra_len + sizeof(head_fixed);
        head = (char*)malloc(head_cap);
        if (!head) {
            free(gz);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
    }
    if (no_body) {
        sbody = "";
        slen = 0;
        free(gz);
        gz = NULL;
    } else if (gz && (gz_len < body_len || c->identity_refused)) {
        compressed = 1;
        sbody = (const char*)gz;
        slen = gz_len;
    } else {
        free(gz);
        gz = NULL;
    }
    if (!compressed && !no_body && c->identity_refused) {
        c->identity_refused = 0;
        if (head != head_fixed)
            free(head);
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        return;
    }

    wlen = (no_body || c->head_only) ? 0 : slen;
    if (no_body) {
        n = snprintf(head, head_cap,
            "HTTP/1.1 %d %s\r\n%s%sConnection: %s\r\n\r\n",
            status, dyn_reason_phrase(status), date, extra ? extra : "", conn_tok);
    } else if (compressed) {
        n = snprintf(head, head_cap,
            "HTTP/1.1 %d %s\r\n%s%sContent-Type: %s\r\n"
            "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"
            "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
            status, dyn_reason_phrase(status), date, extra ? extra : "",
            ctype, slen, conn_tok);
    } else if (!extra && !c->linger && !c->close_after && status == 200 && strcmp(ctype, "application/json") == 0) {
        size_t dl = strlen(date);
        n = (int)(sizeof(hdr200_s) - 1);
        memcpy(head, hdr200_s, (size_t)n);
        memcpy(head + n, date, dl);
        n += (int)dl;
        memcpy(head + n, hdr200_a, sizeof(hdr200_a) - 1);
        n += (int)(sizeof(hdr200_a) - 1);
        n += dyn_put_u64(head + n, (uint64_t)body_len);
        memcpy(head + n, hdr_b, sizeof(hdr_b) - 1);
        n += (int)(sizeof(hdr_b) - 1);
    } else {
        n = dyn_app_head_plain(head, head_cap, status, date, extra, extra_len, ctype, slen, conn_tok);
    }
    if (n > 0 && (size_t)n >= head_cap) {
        free(gz);
        if (head != head_fixed)
            free(head);
        dyn_app_send_err(c, 500, "{\"error\":\"response head too large\"}");
        return;
    }
    if (n > 0) {
        if ((size_t)n + wlen <= DYN_APP_SMALL_RESP) {
            char small[DYN_APP_SMALL_RESP];
            memcpy(small, head, (size_t)n);
            if (wlen)
                memcpy(small + n, sbody, wlen);
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, small, (size_t)n + wlen, DYN_AIO_HOLD,
                    dyn_app_out_last, c)
                < 0)
                dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
            free(gz);
            if (head != head_fixed)
                free(head);
            dyn_app_met_response(c, (double)wlen);
            return;
        }
        c->refs++;
        if (dyn_aio_send2(c->app->aio, c->fd, head, (size_t)n, sbody, wlen, DYN_AIO_HOLD,
                dyn_app_out_last, c)
            < 0)
            dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
    }
    free(gz);
    if (head != head_fixed)
        free(head);
    dyn_app_met_response(c, (double)wlen);
}

static void dyn_app_send_body_x(dyn_app_conn_t* c, int status,
    const char* ctype, const char* body,
    size_t body_len, const char* extra)
{
    uint8_t* gz = NULL;
    size_t gz_len = 0;

    if (dyn_app_gz_wanted(c, status, ctype, body_len)) {
        if (body_len >= DYN_APP_GZIP_OFFLOAD
            && dyn_app_gz_offload(c, status, ctype, body, body_len, extra) == 0)
            return;
        if (dyn_gzip_build((const uint8_t*)body, body_len, &gz, &gz_len) != 0) {
            gz = NULL;
            gz_len = 0;
        }
    }
    dyn_app_send_built(c, status, ctype, body, body_len, extra, gz, gz_len);
}

void dyn_app_send_body(dyn_app_conn_t* c, int status, const char* ctype,
    const char* body, size_t body_len)
{
    dyn_app_send_body_x(c, status, ctype, body, body_len, NULL);
}

void dyn_app_send_json(dyn_app_conn_t* c, int status, const char* body,
    size_t body_len)
{
    dyn_app_send_body(c, status, "application/json", body, body_len);
}

void dyn_app_send_rpc_result(dyn_app_conn_t* c, JSValueConst result,
    const char* id_s)
{
    JSContext* ctx = c->app->ctx;
    size_t out_len;
    if (dyn_app_try_stream_result(c, result))
        return;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    int res_undef = !JS_IsException(jstr) && JS_IsUndefined(jstr);
    const char* out_s = (JS_IsException(jstr) || res_undef)
        ? NULL
        : JS_ToCStringLen(ctx, &out_len, jstr);
    if (res_undef) {
        out_s = "null";
        out_len = 4;
    }
    if (out_s) {
        dyn_iobuf_t o;
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(&o, out_s, out_len);
        dyn_iobuf_append(&o, ",\"id\":", 6);
        dyn_iobuf_append(&o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char*)o.data, o.len);
        dyn_iobuf_free(&o);
        if (!res_undef)
            JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr))
            JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 500, -32603, "Internal error", id_s);
    }
    JS_FreeValue(ctx, jstr);
}

void dyn_app_send_rpc_throw(dyn_app_conn_t* c, JSValueConst exc,
    const char* id_s)
{
    JSContext* ctx = c->app->ctx;
    size_t em_len = 0;
    const char* em = JS_ToCStringLen(ctx, &em_len, exc);
    if (!em)
        JS_FreeValue(ctx, JS_GetException(ctx));
    dyn_app_rpc_error_n(c, 500, -32000, em ? em : "Server error",
        em ? em_len : strlen("Server error"), id_s);
    if (em)
        JS_FreeCString(ctx, em);
}

void dyn_app_build_error(int code, const char* msg, const char* id_s,
    dyn_iobuf_t* o)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_s);
    if (n > 0)
        dyn_iobuf_append(o, buf, n);
}

void dyn_app_build_result(JSContext* ctx, JSValueConst result,
    const char* id_s, dyn_iobuf_t* o)
{
    size_t out_len;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    const char* out_s = JS_IsException(jstr) ? NULL : JS_ToCStringLen(ctx, &out_len, jstr);
    if (out_s) {
        dyn_iobuf_append(o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(o, out_s, out_len);
        dyn_iobuf_append(o, ",\"id\":", 6);
        dyn_iobuf_append(o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(o, "}", 1);
        JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr))
            JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_build_error(-32603, "Internal error", id_s, o);
    }
    JS_FreeValue(ctx, jstr);
}

void dyn_app_send_err(dyn_app_conn_t* c, int status, const char* msg)
{
    dyn_app_send_json(c, status, msg, strlen(msg));
}

void dyn_app_send_err_allow(dyn_app_conn_t* c, int status,
    const char* msg, const char* allow)
{
    char extra[256];
    size_t al = strlen(allow);
    size_t ml = strlen(msg);
    int n;
    if (al > 200)
        al = 200;
    n = snprintf(extra, sizeof(extra), "Allow: %.*s\r\n", (int)al, allow);
    if (n < 0 || (size_t)n >= sizeof(extra))
        dyn_app_send_json(c, status, msg, ml);
    else
        dyn_app_send_body_x(c, status, "application/json", msg, ml, extra);
}

JSValue dyn_app_build_query(JSContext* ctx, const char* qs, size_t qn)
{
    JSValue o = JS_NewObject(ctx);
    size_t i = 0;
    if (JS_IsException(o))
        return o;
    while (i < qn) {
        size_t ks = i, ke, vs, ve;
        while (i < qn && qs[i] != '&')
            i++;
        ke = i;
        if (i < qn && qs[i] == '&')
            i++;
        vs = ke;
        ve = ke;
        {
            size_t eq = ks;
            while (eq < ke && qs[eq] != '=')
                eq++;
            if (eq < ke) {
                vs = eq + 1;
                ve = ke;
                ke = eq;
            } else {
                vs = ve = ke;
            }
        }
        if (ke > ks && dyn_pct_plain(qs + ks, ke - ks, 1) && dyn_pct_plain(qs + vs, ve - vs, 1)) {
            JSAtom ka = JS_NewAtomLen(ctx, qs + ks, ke - ks);
            JSValue v;
            int rc;
            if (ka == JS_ATOM_NULL) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
            v = JS_NewStringLen(ctx, qs + vs, ve - vs);
            if (JS_IsException(v)) {
                JS_FreeAtom(ctx, ka);
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
            rc = JS_DefinePropertyValue(ctx, o, ka, v, JS_PROP_C_W_E);
            JS_FreeAtom(ctx, ka);
            if (rc < 0) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
        } else if (ke > ks) {
            char* kdec = dyn_pct_decode(qs + ks, ke - ks, 1, NULL);
            char* vdec = dyn_pct_decode(qs + vs, ve - vs, 1, NULL);
            if (!kdec || !vdec) {
                free(kdec);
                free(vdec);
                JS_FreeValue(ctx, o);
                return JS_ThrowOutOfMemory(ctx);
            }
            {
                JSValue v = JS_NewString(ctx, vdec);
                int rc;
                free(vdec);
                if (JS_IsException(v)) {
                    free(kdec);
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                }
                rc = JS_DefinePropertyValueStr(ctx, o, kdec, v, JS_PROP_C_W_E);
                free(kdec);
                if (rc < 0) {
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                }
            }
        }
    }
    return o;
}

typedef struct {
    JSAtom atom;
    size_t vs, ve;
    char* joined;
    size_t jlen;
} dyn_hdr_field_t;

static int dyn_hdr_join(dyn_hdr_field_t* f, const char* base, const char* sep,
    size_t vs, size_t ve)
{
    const char* cur = f->joined ? f->joined : base + f->vs;
    size_t cl = f->joined ? f->jlen : f->ve - f->vs;
    size_t nl = cl + 2 + (ve - vs);
    char* j = (char*)malloc(nl + 1);
    if (!j)
        return -1;
    memcpy(j, cur, cl);
    memcpy(j + cl, sep, 2);
    memcpy(j + cl + 2, base + vs, ve - vs);
    j[nl] = '\0';
    free(f->joined);
    f->joined = j;
    f->jlen = nl;
    return 0;
}

static void dyn_hdr_fields_free(JSContext* ctx, dyn_hdr_field_t* fs, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        JS_FreeAtom(ctx, fs[i].atom);
        free(fs[i].joined);
    }
}

static JSObjectTemplate* dyn_app_hdr_template(JSContext* ctx, dyn_app_t* app,
    const dyn_hdr_field_t* fs, uint32_t n, uint32_t h)
{
    JSAtom atoms[DYN_APP_MAX_HEADER_COUNT] DYN_IO_SCRATCH;
    dyn_app_hdr_tpl_t* e;
    uint32_t i, k;

    for (i = 0; i < n; i++)
        atoms[i] = fs[i].atom;
    for (k = 0; k < DYN_APP_HDR_TPLS; k++) {
        e = &app->hdr_tpl[k];
        if (e->tpl && e->n == n && memcmp(e->atoms, atoms, n * sizeof(JSAtom)) == 0)
            return e->tpl;
    }
    if (app->hdr_last_miss != h) {
        app->hdr_last_miss = h;
        return NULL;
    }
    app->hdr_last_miss = 0;
    e = &app->hdr_tpl[app->hdr_tpl_next % DYN_APP_HDR_TPLS];
    if (e->tpl) {
        JS_FreeObjectTemplate(ctx, e->tpl);
        for (i = 0; i < e->n; i++)
            JS_FreeAtom(ctx, e->atoms[i]);
        free(e->atoms);
        e->tpl = NULL;
        e->atoms = NULL;
        e->n = 0;
    }
    e->atoms = (JSAtom*)malloc(n * sizeof(JSAtom));
    if (!e->atoms)
        return NULL;
    e->tpl = JS_NewObjectTemplate(ctx, atoms, n);
    if (!e->tpl) {
        free(e->atoms);
        e->atoms = NULL;
        return NULL;
    }
    for (i = 0; i < n; i++)
        e->atoms[i] = JS_DupAtom(ctx, atoms[i]);
    e->n = n;
    app->hdr_tpl_next++;
    return e->tpl;
}

void dyn_app_hdr_templates_free(JSContext* ctx, dyn_app_t* app)
{
    uint32_t k, i;
    for (k = 0; k < DYN_APP_HDR_TPLS; k++) {
        dyn_app_hdr_tpl_t* e = &app->hdr_tpl[k];
        if (!e->tpl)
            continue;
        JS_FreeObjectTemplate(ctx, e->tpl);
        for (i = 0; i < e->n; i++)
            JS_FreeAtom(ctx, e->atoms[i]);
        free(e->atoms);
        e->tpl = NULL;
        e->atoms = NULL;
        e->n = 0;
    }
}

void dyn_app_hdr_templates_mark(JSRuntime* rt, dyn_app_t* app, JS_MarkFunc* mark_func)
{
    uint32_t k;
    for (k = 0; k < DYN_APP_HDR_TPLS; k++)
        JS_MarkObjectTemplate(rt, app->hdr_tpl[k].tpl, mark_func);
}

JSValue dyn_app_build_headers(JSContext* ctx, dyn_app_t* app, const char* base, size_t head_len)
{
    dyn_hdr_field_t fs[DYN_APP_MAX_HEADER_COUNT] DYN_IO_SCRATCH;
    uint32_t n = 0, i, nh = 2166136261u;
    int digit_name = 0;
    size_t ls = 0, le;
    JSObjectTemplate* tpl;
    JSValue o;

    while (ls + 1 < head_len && !(base[ls] == '\r' && base[ls + 1] == '\n'))
        ls++;
    ls += 2;
    while (ls + 1 < head_len) {
        size_t colon, vs, ve, ns, ne, k;
        char nbuf[128];
        JSAtom a;
        le = ls;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        colon = ls;
        while (colon < le && base[colon] != ':')
            colon++;
        if (colon >= le) {
            ls = le + 2;
            continue;
        }
        ns = ls;
        ne = colon;
        while (ne > ns && (base[ne - 1] == ' ' || base[ne - 1] == '\t'))
            ne--;
        while (ns < ne && (base[ns] == ' ' || base[ns] == '\t'))
            ns++;
        vs = colon + 1;
        ve = le;
        while (vs < ve && (base[vs] == ' ' || base[vs] == '\t'))
            vs++;
        while (ve > vs && (base[ve - 1] == ' ' || base[ve - 1] == '\t'))
            ve--;
        ls = le + 2;
        if (ne - ns >= sizeof nbuf)
            continue;
        for (k = 0; k < ne - ns; k++) {
            unsigned char c = (unsigned char)base[ns + k];
            nbuf[k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        a = JS_NewAtomLen(ctx, nbuf, ne - ns);
        if (a == JS_ATOM_NULL) {
            dyn_hdr_fields_free(ctx, fs, n);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n && fs[i].atom != a; i++)
            ;
        if (i < n) {
            JS_FreeAtom(ctx, a);
            if (ne - ns == 10 && memcmp(nbuf, "set-cookie", 10) == 0)
                continue;
            if (dyn_hdr_join(&fs[i], base,
                    (ne - ns == 6 && memcmp(nbuf, "cookie", 6) == 0) ? "; " : ", ", vs, ve)
                < 0) {
                dyn_hdr_fields_free(ctx, fs, n);
                return JS_ThrowOutOfMemory(ctx);
            }
            continue;
        }
        if (n == DYN_APP_MAX_HEADER_COUNT) {
            JS_FreeAtom(ctx, a);
            continue;
        }
        if (ne > ns && nbuf[0] >= '0' && nbuf[0] <= '9')
            digit_name = 1;
        for (k = 0; k < ne - ns; k++)
            nh = (nh ^ (unsigned char)nbuf[k]) * 16777619u;
        nh = (nh ^ 0x3au) * 16777619u;
        fs[n].atom = a;
        fs[n].vs = vs;
        fs[n].ve = ve;
        fs[n].joined = NULL;
        fs[n].jlen = 0;
        n++;
    }
    tpl = (n && !digit_name) ? dyn_app_hdr_template(ctx, app, fs, n, nh) : NULL;
    o = tpl ? JS_NewObjectFromTemplate(ctx, tpl) : JS_NewObject(ctx);
    if (JS_IsException(o)) {
        dyn_hdr_fields_free(ctx, fs, n);
        return o;
    }
    for (i = 0; i < n; i++) {
        JSValue v = fs[i].joined ? JS_NewStringLen(ctx, fs[i].joined, fs[i].jlen)
                                 : JS_NewStringLen(ctx, base + fs[i].vs, fs[i].ve - fs[i].vs);
        if (JS_IsException(v)) {
            dyn_hdr_fields_free(ctx, fs, n);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        if (tpl)
            JS_SetTemplateCell(ctx, o, i, v);
        else if (JS_DefinePropertyValue(ctx, o, fs[i].atom, v, JS_PROP_C_W_E) < 0) {
            dyn_hdr_fields_free(ctx, fs, n);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    dyn_hdr_fields_free(ctx, fs, n);
    return o;
}

static int dyn_value_is_bytes(JSContext* ctx, JSValueConst v)
{
    size_t n = 0;
    uint8_t* p;
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE)
        return 0;
    p = JS_GetArrayBuffer(ctx, &n, v);
    if (p)
        return 1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            JS_FreeValue(ctx, ab);
            return bpe == 1;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return 0;
}

static int dyn_bytes_to_c(JSContext* ctx, JSValueConst v,
    const uint8_t** pp, size_t* pn,
    JSValue* keep)
{
    size_t n = 0;
    uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
    if (p) {
        *pp = p;
        *pn = n;
        *keep = JS_UNDEFINED;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, absz;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return -1;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &absz, ab);
        *keep = ab;
        if (!base) {
            JS_FreeValue(ctx, ab);
            *keep = JS_UNDEFINED;
            return -1;
        }
        if (off > absz || len > absz - off) {
            JS_FreeValue(ctx, ab);
            *keep = JS_UNDEFINED;
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
}

static void dyn_app_dyn_send(dyn_app_conn_t* c, JSValueConst val);

static void dyn_app_dyn_send_json_c(dyn_app_conn_t* c, JSContext* ctx, JSValueConst v)
{
    JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jstr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"unserializable response\"}");
        return;
    }
    {
        size_t n = 0;
        const char* s = JS_ToCStringLen(ctx, &n, jstr);
        JS_FreeValue(ctx, jstr);
        if (!s) {
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        dyn_app_send_body(c, 200, "application/json", s, n);
        JS_FreeCString(ctx, s);
    }
}

static void dyn_app_dyn_send(dyn_app_conn_t* c, JSValueConst val)
{
    JSContext* ctx = c->app->ctx;
    if (JS_IsString(val)) {
        size_t n = 0;
        const char* s = JS_ToCStringLen(ctx, &n, val);
        if (!s) {
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        dyn_app_send_body(c, 200, "text/plain", s, n);
        JS_FreeCString(ctx, s);
        return;
    }
    if (dyn_value_is_bytes(ctx, val)) {
        const uint8_t* p = NULL;
        size_t n = 0;
        JSValue keep = JS_UNDEFINED;
        if (dyn_bytes_to_c(ctx, val, &p, &n, &keep) == 0) {
            dyn_app_send_body(c, 200, "application/octet-stream",
                (const char*)p, n);
            JS_FreeValue(ctx, keep);
        } else {
            JS_FreeValue(ctx, keep);
            dyn_app_send_err(c, 500, "{\"error\":\"bad bytes response\"}");
        }
        return;
    }
    if (JS_IsObject(val)) {
        JSValue has_status = dyn_app_own_get(ctx, val, "status");
        JSValue has_body = dyn_app_own_get(ctx, val, "body");
        int is_env = 0;
        int status = 200;
        dyn_iobuf_t xh;
        const char* xerr;
        const char* extra;
        if (!JS_IsException(has_status) && !JS_IsUndefined(has_status) && JS_IsNumber(has_status))
            is_env = 1;
        if (!JS_IsException(has_body) && !JS_IsUndefined(has_body))
            is_env = 1;
        if (!is_env) {
            JS_FreeValue(ctx, has_status);
            JS_FreeValue(ctx, has_body);
            dyn_app_dyn_send_json_c(c, ctx, val);
            return;
        }
        if (!JS_IsUndefined(has_status)) {
            int32_t st = 200;
            if (JS_ToInt32(ctx, &st, has_status) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, has_status);
                JS_FreeValue(ctx, has_body);
                dyn_app_send_err(c, 500, "{\"error\":\"bad response status\"}");
                return;
            }
            status = st;
            if (status < 100 || status > 599) {
                JS_FreeValue(ctx, has_status);
                JS_FreeValue(ctx, has_body);
                dyn_app_send_err(c, 500, "{\"error\":\"bad response status\"}");
                return;
            }
        }
        JS_FreeValue(ctx, has_status);
        dyn_iobuf_init(&xh);
        xerr = dyn_app_env_headers(ctx, val, http_env_keys, countof(http_env_keys), &xh);
        if (xerr) {
            dyn_iobuf_free(&xh);
            JS_FreeValue(ctx, has_body);
            dyn_app_send_err(c, 500, xerr);
            return;
        }
        extra = xh.len ? (const char*)xh.data : NULL;
        {
            JSValue ctv = dyn_app_own_get(ctx, val, "contentType");
            char ct_owned[DYN_HTTP_CT_MAX + 1];
            const char* ct = "application/json";
            int ct_set = 0;
            if (!JS_IsException(ctv) && JS_IsString(ctv)) {
                size_t cn = 0;
                const char* cs = JS_ToCStringLen(ctx, &cn, ctv);
                if (cs) {
                    int bad = !httpk_value_gen_ok(cs, cn);
                    if (!bad && cn > DYN_HTTP_CT_MAX)
                        bad = 2;
                    if (bad) {
                        JS_FreeCString(ctx, cs);
                        JS_FreeValue(ctx, ctv);
                        JS_FreeValue(ctx, has_body);
                        dyn_iobuf_free(&xh);
                        dyn_app_send_err(c, 500, bad == 2 ? "{\"error\":\"response contentType too long\"}" : "{\"error\":\"response contentType must not "
                                                                                                              "contain control characters\"}");
                        return;
                    }
                    memcpy(ct_owned, cs, cn);
                    ct_owned[cn] = '\0';
                    ct = ct_owned;
                    ct_set = 1;
                    JS_FreeCString(ctx, cs);
                }
            }
            JS_FreeValue(ctx, ctv);
            if (JS_IsUndefined(has_body)) {
                JS_FreeValue(ctx, has_body);
                dyn_app_send_body_x(c, status, ct, "", 0, extra);
                dyn_iobuf_free(&xh);
                return;
            }
            if (JS_IsString(has_body)) {
                size_t n = 0;
                const char* s = JS_ToCStringLen(ctx, &n, has_body);
                const char* use_ct = ct_set ? ct : "text/plain";
                JS_FreeValue(ctx, has_body);
                if (!s) {
                    dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                    dyn_iobuf_free(&xh);
                    return;
                }
                dyn_app_send_body_x(c, status, use_ct, s, n, extra);
                JS_FreeCString(ctx, s);
                dyn_iobuf_free(&xh);
                return;
            }
            if (dyn_value_is_bytes(ctx, has_body)) {
                const uint8_t* p = NULL;
                size_t n = 0;
                JSValue keep = JS_UNDEFINED;
                const char* use_ct = ct_set ? ct : "application/octet-stream";
                if (dyn_bytes_to_c(ctx, has_body, &p, &n, &keep) == 0) {
                    dyn_app_send_body_x(c, status, use_ct, (const char*)p, n, extra);
                    JS_FreeValue(ctx, keep);
                } else {
                    JS_FreeValue(ctx, keep);
                    dyn_app_send_err(c, 500, "{\"error\":\"bad response body\"}");
                }
                JS_FreeValue(ctx, has_body);
                dyn_iobuf_free(&xh);
                return;
            }
            {
                JSValue jstr = JS_JSONStringify(ctx, has_body, JS_UNDEFINED, JS_UNDEFINED);
                JS_FreeValue(ctx, has_body);
                if (JS_IsException(jstr)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_send_err(c, 500, "{\"error\":\"unserializable response body\"}");
                    dyn_iobuf_free(&xh);
                    return;
                }
                {
                    size_t n = 0;
                    const char* s = JS_ToCStringLen(ctx, &n, jstr);
                    JS_FreeValue(ctx, jstr);
                    if (!s) {
                        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                        dyn_iobuf_free(&xh);
                        return;
                    }
                    dyn_app_send_body_x(c, status, ct, s, n, extra);
                    JS_FreeCString(ctx, s);
                }
                dyn_iobuf_free(&xh);
                return;
            }
        }
    }
    if (JS_IsUndefined(val)) {
        dyn_app_send_err(c, 500, "{\"error\":\"handler returned undefined\"}");
        return;
    }
    if (JS_IsNull(val)) {
        dyn_app_send_body(c, 200, "application/json", "null", 4);
        return;
    }
    dyn_app_dyn_send_json_c(c, ctx, val);
}

static JSValue dyn_app_dyn_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    dyn_app_pend_t* pd;
    dyn_app_conn_t* c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd)
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        if (magic) {
            const char* em = NULL;
            size_t em_len = 0;
            JSValue m = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "message") : JS_UNDEFINED;
            if (JS_IsString(m))
                em = JS_ToCStringLen(ctx, &em_len, m);
            JS_FreeValue(ctx, m);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
        } else {
            dyn_app_dyn_send(c, v);
        }
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

void dyn_app_dyn_send_or_await(dyn_app_conn_t* c, JSValue res)
{
    JSContext* ctx = c->app->ctx;
    int thenable = 0;
    if (JS_IsObject(res)) {
        JSValue th = JS_GetPropertyStr(ctx, res, "then");
        thenable = JS_IsFunction(ctx, th);
        JS_FreeValue(ctx, th);
    }
    if (!thenable) {
        dyn_app_dyn_send(c, res);
        JS_FreeValue(ctx, res);
        return;
    }
    {
        dyn_app_pend_t* pd = (dyn_app_pend_t*)calloc(1, sizeof *pd);
        JSValue pobj, thenv, onres, onrej;
        if (!pd) {
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        pd->conn = c;
        c->refs++;
        pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
        if (JS_IsException(pobj)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_app_pend_release(pd, 0);
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        JS_SetOpaque(pobj, pd);
        onres = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 0, 1, &pobj);
        onrej = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 1, 1, &pobj);
        if (JS_IsException(onres) || JS_IsException(onrej)) {
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        thenv = JS_GetPropertyStr(ctx, res, "then");
        if (JS_IsException(thenv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj);
            dyn_app_send_err(c, 500, "{\"error\":\"bad thenable\"}");
            return;
        }
        {
            JSValueConst a[2] = { onres, onrej };
            JSValue r;
            pd->counted = 1;
            c->resp_parked++;
            r = JS_Call(ctx, thenv, res, 2, a);
            JS_FreeValue(ctx, thenv);
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                dyn_app_pend_t* p2 = dyn_app_pend_claim(pobj);
                if (p2) {
                    if (!p2->conn->closed)
                        dyn_app_send_json(p2->conn, 500,
                            "{\"error\":\"Server error\"}", 24);
                    dyn_app_pend_release(p2, 0);
                }
            } else {
                JS_FreeValue(ctx, r);
            }
            JS_FreeValue(ctx, pobj);
        }
    }
}
#endif
