#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_URL_MAX_PATH 2048
#define DYN_APP_ENV_HDR_MAX 8192

int dyn_parse_url(const char* url, dyn_url_t* out)
{
    const char* p = url;
    const char *host_start, *host_end;
    size_t host_len, path_len;

    out->tls = 0;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        out->tls = 1;
    }
    else
        return -1;

    host_start = p;
    if (*p == '[') {
        const char* close = strchr(p, ']');
        if (!close)
            return -1;
        p = close + 1;
    } else {
        while (*p && *p != ':' && *p != '/')
            p++;
    }
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return -1;
    {
        const char* h;
        for (h = host_start; h < host_end; h++)
            if ((unsigned char)*h < 0x21 || (unsigned char)*h > 0x7e)
                return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    out->port = out->tls ? 443 : 80;
    if (*p == ':') {
        long port = 0;
        p++;
        if (*p < '0' || *p > '9')
            return -1;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            if (port > 65535)
                return -1;
            p++;
        }
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        out->path = "/";
        out->path_len = 1;
        return 0;
    }
    if (*p != '/')
        return -1;
    path_len = strlen(p);
    if (path_len >= DYN_URL_MAX_PATH)
        return -1;
    {
        const char* q;
        for (q = p; q < p + path_len; q++)
            if ((unsigned char)*q < 0x21 || (unsigned char)*q > 0x7e)
                return -1;
    }
    {
        const char* frag = (const char*)memchr(p, '#', path_len);
        if (frag)
            path_len = (size_t)(frag - p);
    }
    if (path_len == 0) {
        out->path = "/";
        out->path_len = 1;
        return 0;
    }
    out->path = p;
    out->path_len = path_len;
    return 0;
}

const char* dyn_req_header(const char* buf, size_t len,
    const char* name, size_t* vlen)
{
    size_t nlen = strlen(name), i;
    for (i = 0; i + 2 + nlen + 1 <= len; i++) {
        const char *v, *ve, *end;
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;
        if (!dyn_ci_eq(buf + i + 2, name, nlen) || buf[i + 2 + nlen] != ':')
            continue;
        v = buf + i + 2 + nlen + 1;
        end = buf + len;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        ve = v;
        while (ve < end && *ve != '\r' && *ve != '\n')
            ve++;
        *vlen = (size_t)(ve - v);
        return v;
    }
    return NULL;
}

static const char* dyn_app_env_hdr_put(JSContext* ctx, dyn_iobuf_t* out,
    const char* name, size_t nlen, JSValueConst v)
{
    const char* vs;
    size_t vn = 0;
    const char* err = NULL;

    if (!JS_IsString(v))
        return "{\"error\":\"response header values must be strings or arrays of strings\"}";
    vs = JS_ToCStringLen(ctx, &vn, v);
    if (!vs) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "{\"error\":\"oom\"}";
    }
    if (!httpk_value_gen_ok(vs, vn))
        err = "{\"error\":\"response header values must not contain control characters\"}";
    else if (out->len + nlen + vn + 4 > DYN_APP_ENV_HDR_MAX)
        err = "{\"error\":\"response headers too large\"}";
    else if (dyn_iobuf_append(out, name, nlen) < 0 || dyn_iobuf_append(out, ": ", 2) < 0
        || dyn_iobuf_append(out, vs, vn) < 0 || dyn_iobuf_append(out, "\r\n", 2) < 0)
        err = "{\"error\":\"oom\"}";
    JS_FreeCString(ctx, vs);
    return err;
}

static const char* dyn_app_env_hdr_one(JSContext* ctx, dyn_iobuf_t* out,
    const char* name, size_t nlen, JSValueConst v)
{
    static const char* const owned[] = { "content-length", "transfer-encoding",
        "connection", "content-encoding" };
    const char* err = NULL;
    size_t k;

    if (!httpk_name_ok(name, nlen))
        return "{\"error\":\"response header names must be HTTP tokens\"}";
    if (dyn_ci_equal(name, nlen, "content-type"))
        return "{\"error\":\"response headers: set Content-Type with contentType\"}";
    for (k = 0; k < countof(owned); k++)
        if (dyn_ci_equal(name, nlen, owned[k]))
            return "{\"error\":\"response headers: Content-Length, Transfer-Encoding, "
                   "Connection and Content-Encoding belong to the server\"}";
    if (JS_IsArray(ctx, v) > 0) {
        uint32_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &len, lv) < 0) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, JS_GetException(ctx));
            return "{\"error\":\"bad response headers\"}";
        }
        JS_FreeValue(ctx, lv);
        for (i = 0; i < len && !err; i++) {
            JSValue it = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(it)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return "{\"error\":\"bad response headers\"}";
            }
            err = dyn_app_env_hdr_put(ctx, out, name, nlen, it);
            JS_FreeValue(ctx, it);
        }
        return err;
    }
    return dyn_app_env_hdr_put(ctx, out, name, nlen, v);
}

const char* dyn_app_env_headers(JSContext* ctx, JSValueConst env,
    const char* const* keys, int nkeys, dyn_iobuf_t* out)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    const char* err = NULL;
    JSValue hv;

    if (dyn_opts_strict(ctx, env, keys, nkeys) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return keys == http_env_keys
            ? "{\"error\":\"response envelope: unknown key (valid: status, contentType, body, headers)\"}"
            : "{\"error\":\"response envelope: unknown key (valid: status, contentType, stream, headers)\"}";
    }
    hv = dyn_app_own_get(ctx, env, "headers");
    if (JS_IsException(hv)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "{\"error\":\"bad response headers\"}";
    }
    if (JS_IsUndefined(hv))
        return NULL;
    if (!JS_IsObject(hv) || JS_IsArray(ctx, hv) > 0) {
        JS_FreeValue(ctx, hv);
        return "{\"error\":\"response headers must be an object\"}";
    }
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, hv,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, hv);
        return "{\"error\":\"bad response headers\"}";
    }
    for (i = 0; i < nprops && !err; i++) {
        size_t nlen = 0;
        const char* name = JS_AtomToCStringLen(ctx, &nlen, props[i].atom);
        JSValue v = JS_GetProperty(ctx, hv, props[i].atom);
        if (!name || JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            err = "{\"error\":\"bad response headers\"}";
        } else {
            err = dyn_app_env_hdr_one(ctx, out, name, nlen, v);
        }
        if (name)
            JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, v);
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    JS_FreeValue(ctx, hv);
    if (!err && out->len && dyn_iobuf_append(out, "", 1) < 0)
        err = "{\"error\":\"oom\"}";
    return err;
}

size_t dyn_req_header_count(const char* buf, size_t len)
{
    size_t i, n = 0;
    for (i = 0; i + 2 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] != '\r')
            n++;
    return n;
}

void dyn_app_req_method(const char* base, char* out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap && base[i] && base[i] != ' ' && base[i] != '\r') {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

void dyn_app_req_target(const char* base, size_t head_len,
    const char** tgt, size_t* tlen)
{
    const char* s = strchr(base, ' ');
    const char* e;
    if (!s) {
        *tgt = "";
        *tlen = 0;
        return;
    }
    s++;
    e = strchr(s, ' ');
    if (!e || (size_t)(e - base) > head_len) {
        *tgt = "";
        *tlen = 0;
        return;
    }
    *tgt = s;
    *tlen = (size_t)(e - s);
}

static void dyn_app_allow_free(char** out, size_t n)
{
    size_t k;
    for (k = 0; k < n; k++)
        free(out[k]);
    free(out);
}

int dyn_app_parse_allow(JSContext* ctx, JSValueConst opts, char*** pout, size_t* pn)
{
    JSValue av = JS_GetPropertyStr(ctx, opts, "allow");
    char** out = NULL;
    size_t got = 0;
    int isarr;
    uint32_t len = 0, i;
    *pout = NULL;
    *pn = 0;
    if (JS_IsException(av))
        return -1;
    isarr = JS_IsArray(ctx, av);
    if (isarr <= 0) {
        JS_FreeValue(ctx, av);
        return isarr;
    }
    if (dyn_js_len_u32(ctx, av, &len)) {
        JS_FreeValue(ctx, av);
        return -1;
    }
    if (len && !(out = (char**)calloc(len, sizeof(char*)))) {
        JS_FreeValue(ctx, av);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, av, i);
        const char* s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (s) {
            out[got] = strdup(s);
            JS_FreeCString(ctx, s);
        }
        if (!s || !out[got]) {
            if (s)
                JS_ThrowOutOfMemory(ctx);
            JS_FreeValue(ctx, av);
            dyn_app_allow_free(out, got);
            return -1;
        }
        got++;
    }
    JS_FreeValue(ctx, av);
    *pout = out;
    *pn = got;
    return 0;
}
#endif
