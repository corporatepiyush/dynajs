#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_APP_LINGER_MS 5000
#define DYN_APP_LINGER_MAX ((int64_t)64 * 1024 * 1024)

const char* const http_env_keys[] = { "status", "contentType", "body", "headers" };

const char* const http_ws_handler_keys[] = { "open", "message", "close", "upgrade" };

int dyn_bytes_reserve(dyn_bytes_t* b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;
    char* nd;

    if (need < b->len)
        return -1;
    if (need <= b->cap)
        return 0;
    nc = b->cap ? b->cap * 2 : 4096;
    while (nc < need) {
        size_t doubled = nc * 2;
        if (doubled < nc)
            return -1;
        nc = doubled;
    }
    nd = (char*)realloc(b->data, nc);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

int dyn_bytes_append(dyn_bytes_t* b, const char* src, size_t n)
{
    if (dyn_bytes_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

const char* dyn_memfind(const char* hay, size_t hlen,
    const char* needle, size_t nlen)
{
    size_t idx;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    idx = simd.strfind((const uint8_t*)hay, hlen,
        (const uint8_t*)needle, nlen);
    return idx == SIZE_MAX ? NULL : hay + idx;
}

JSClassID dyn_http_client_class_id;

int dyn_method_valid(const char* m, size_t n)
{
    size_t i;
    if (!m || n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (!httpk_tchar[(unsigned char)m[i]])
            return 0;
    return 1;
}

void dyn_call_drop(JSContext* ctx, JSValueConst fn,
    JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = JS_Call(ctx, fn, this_val, argc, argv);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

const char* dyn_host_bare(const char* host, char* buf, size_t cap)
{
    const char* close_;
    size_t n;
    if (host[0] != '[')
        return host;
    close_ = strchr(host, ']');
    if (!close_)
        return host;
    n = (size_t)(close_ - host) - 1;
    if (n == 0 || n >= cap)
        return host;
    memcpy(buf, host + 1, n);
    buf[n] = '\0';
    return buf;
}

JSValue dyn_http_throw(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy)
{
    JSValue e = hc_error_value(ctx, code, method, url, tlswhy);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

JSValue dyn_http_throw_named(JSContext* ctx, int code,
    const char* method, const char* url,
    const char* what)
{
    JSValue e = hc_error_value_named(ctx, code, method, url, what);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

JSClassID dyn_hc_stream_class_id;

JSClassID dyn_http_server_class_id;

JSClassID dyn_http_async_class_id;

void dyn_app_met_response(dyn_app_conn_t* c, double bytes)
{
    static dyn_met_handle_t h_responses, h_bytes, h_duration;

    dyn_metrics_c_record_h(&h_responses, DYN_MET_COUNTER, "dyn_http_responses_total", 1);
    dyn_metrics_c_record_h(&h_bytes, DYN_MET_COUNTER, "dyn_http_response_bytes_total",
        bytes);
    if (c->req_start_ms) {
        dyn_metrics_c_record_h(&h_duration,
            DYN_MET_HISTOGRAM, "dyn_http_request_duration_seconds",
            (double)(dyn_timer_now_ms() - c->req_start_ms) / 1000.0);
        c->req_start_ms = 0;
    }
}

JSValue dyn_app_own_get(JSContext* ctx, JSValueConst obj, const char* key)
{
    JSAtom a = JS_NewAtom(ctx, key);
    JSPropertyDescriptor d;
    JSValue v;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &d, obj, a);
    if (r != 0) {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    if (r < 0) {
        JS_FreeAtom(ctx, a);
        return JS_EXCEPTION;
    }
    if (r == 0) {
        JS_FreeAtom(ctx, a);
        return JS_UNDEFINED;
    }
    v = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return v;
}

JSClassID dyn_ws_class_id;

void dws_accept(const char* key, size_t klen, char out[32])
{
    uint8_t buf[64], d[20];
    size_t kn = klen < 24 ? klen : 24, n;

    memcpy(buf, key, kn);
    memcpy(buf + kn, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    dyn_sha1(buf, kn + 36, d);
    n = dyn_codec_base64_encode(d, sizeof(d), out);
    out[n] = '\0';
}

JSClassID dyn_sse_class_id;

JSClassID dyn_app_class_id;

void dyn_json_escape_n_into(char* buf, size_t limit, size_t* pos,
    const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n && *pos + 7 < limit; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = '"';
        } else if (ch == '\\') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = '\\';
        } else if (ch == '\n') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'n';
        } else if (ch == '\r') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'r';
        } else if (ch == '\t') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 't';
        } else if (ch < 0x20)
            *pos += (size_t)snprintf(buf + *pos, 7, "\\u%04x", ch);
        else
            buf[(*pos)++] = (char)ch;
    }
}

int dyn_value_is_byte_source(JSContext* ctx, JSValueConst v)
{
    JSValue rf;
    int is;
    if (!JS_IsObject(v))
        return 0;
    if (js_streams_kind(v) == 1)
        return 1;
    rf = JS_GetPropertyStr(ctx, v, "read");
    is = JS_IsFunction(ctx, rf);
    JS_FreeValue(ctx, rf);
    return is;
}

JSClassID dyn_pend_class_id;

void dyn_app_pend_release(dyn_app_pend_t* pd, int abandoned)
{
    dyn_app_conn_t* c = pd->conn;
    int parked = pd->counted;
    if (parked)
        c->resp_parked--;
    free(pd->id);
    free(pd);
    if (parked) {
        if (abandoned)
            dyn_app_conn_close(c);
        else
            dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

dyn_app_pend_t* dyn_app_pend_claim(JSValueConst obj)
{
    dyn_app_pend_t* pd = (dyn_app_pend_t*)JS_GetOpaque(obj, dyn_pend_class_id);
    if (pd)
        JS_SetOpaque(obj, NULL);
    return pd;
}

void dyn_app_reject_linger(dyn_app_conn_t* c, int status, const char* msg,
    int64_t clen)
{
    c->linger = 1;
    c->linger_left = clen > 0 && clen < DYN_APP_LINGER_MAX ? clen : DYN_APP_LINGER_MAX;
    c->linger_until = dyn_timer_now_ms() + DYN_APP_LINGER_MS;
    dyn_app_send_err(c, status, msg);
    if (!c->closed)
        dyn_iobuf_reset(&c->in);
}

char* dyn_pct_decode(const char* s, size_t n, int plus_space, size_t* outn)
{
    char* o = (char*)malloc(n + 1);
    size_t w = 0, i = 0;
    if (!o)
        return NULL;
    while (i < n) {
        if (s[i] == '%' && i + 2 < n + 1) {
            int h, l;
            if (i + 2 < n && ((h = dyn_hexval(s[i + 1])) >= 0) && ((l = dyn_hexval(s[i + 2])) >= 0)) {
                o[w++] = (char)((h << 4) | l);
                i += 3;
                continue;
            }
            o[w++] = s[i++];
        } else if (plus_space && s[i] == '+') {
            o[w++] = ' ';
            i++;
        } else {
            o[w++] = s[i++];
        }
    }
    o[w] = '\0';
    if (outn)
        *outn = w;
    return o;
}

void dyn_app_dyn_err_json_n(dyn_app_conn_t* c, int status,
    const char* msg, size_t msg_len)
{
    char buf[256];
    size_t pos = 0;
    buf[pos++] = '{';
    buf[pos++] = '"';
    buf[pos++] = 'e';
    buf[pos++] = 'r';
    buf[pos++] = 'r';
    buf[pos++] = 'o';
    buf[pos++] = 'r';
    buf[pos++] = '"';
    buf[pos++] = ':';
    buf[pos++] = '"';
    dyn_json_escape_n_into(buf, sizeof buf - 3, &pos,
        msg ? msg : "Server error",
        msg ? msg_len : strlen("Server error"));
    buf[pos++] = '"';
    buf[pos++] = '}';
    dyn_app_send_json(c, status, buf, pos);
}

JSClassID dyn_wsc_class_id;

const char* const dyn_http_fetch_names[DYN_HTTP_FETCH_NAMES_N] = {
    "fetch", "Request", "Response", "Headers", "AbortSignal",
    "AbortController", "FormData"
};

void dyn_http_add_exports(JSContext* ctx, JSModuleDef* m)
{
    size_t i;
    dyn_httpmsg_add_exports(ctx, m);
    JS_AddModuleExport(ctx, m, "HTTPClient");
    JS_AddModuleExport(ctx, m, "CookieJar");
    JS_AddModuleExport(ctx, m, "HTTPServer");
    JS_AddModuleExport(ctx, m, "HTTPServerAsync");
    JS_AddModuleExport(ctx, m, "App");
    JS_AddModuleExport(ctx, m, "WsClient");
    for (i = 0; i < DYN_HTTP_FETCH_NAMES_N; i++)
        JS_AddModuleExport(ctx, m, dyn_http_fetch_names[i]);
}

int js_nat_init_http(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSModuleDef* m;
    js_fetch_install(ctx, g);
    JS_FreeValue(ctx, g);
    m = JS_NewCModule(ctx, "dyna:http", dyn_http_register);
    if (!m)
        return -1;
    dyn_http_add_exports(ctx, m);
    return 0;
}
#endif
