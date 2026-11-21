#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#include <time.h>

#define DYN_JAR_DEFAULT_MAX 3000

JSClassID dyn_cookiejar_class_id;

static void dyn_cookiejar_dispose(void* native)
{
    dyn_cookiejar_unref((dyn_cookiejar_t*)native);
}

void dyn_cookiejar_unref(dyn_cookiejar_t* j)
{
    if (j && --j->refs == 0) {
        httpk_jar_free(&j->jar);
        free(j);
    }
}

dyn_cookiejar_t* dyn_cookiejar_ref(dyn_cookiejar_t* j)
{
    if (j)
        j->refs++;
    return j;
}

dyn_cookiejar_t* dyn_cookiejar_of(JSContext* ctx, JSValueConst v,
    const char* who)
{
    DynResource* r = JS_IsObject(v)
        ? (DynResource*)JS_GetOpaque(v, dyn_cookiejar_class_id)
        : NULL;
    if (!r || !r->native) {
        JS_ThrowTypeError(ctx, "%s: cookieJar must be a CookieJar", who);
        return NULL;
    }
    if (r->closed) {
        JS_ThrowTypeError(ctx, "%s: the CookieJar is closed", who);
        return NULL;
    }
    return (dyn_cookiejar_t*)r->native;
}

static dyn_cookiejar_t* dyn_cookiejar_this(JSContext* ctx,
    JSValueConst this_val)
{
    DynResource* r = dyn_res_get(ctx, this_val, dyn_cookiejar_class_id);
    return r ? (dyn_cookiejar_t*)r->native : NULL;
}

int dyn_cookiejar_header(dyn_cookiejar_t* j, const char* url, dyn_sb_t* out)
{
    return httpk_jar_header(&j->jar, url, (int64_t)time(NULL), out);
}

int dyn_cookiejar_absorb(dyn_cookiejar_t* j, const char* url,
    const char* raw, const httpk_head_t* h)
{
    uint32_t it = 0;
    const char* v;
    size_t vl;
    int stored = 0;
    int64_t now = (int64_t)time(NULL);
    while (httpk_hdr_next(h, raw, "set-cookie", &it, &v, &vl))
        if (httpk_jar_set(&j->jar, url, v, vl, now) == HTTPK_CK_STORED)
            stored++;
    return stored;
}

int dyn_cookiejar_absorb_raw(dyn_cookiejar_t* j, const char* url,
    const char* raw, size_t head_len)
{
    httpk_head_t h;
    httpk_hdr_t small[32], *heap = NULL;
    int n = 0;
    if (dyn_head_parse(raw, head_len, &h, small, 32, &heap) == HTTPK_OK)
        n = dyn_cookiejar_absorb(j, url, raw, &h);
    free(heap);
    return n;
}

static JSValue dyn_cookiejar_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_cookiejar_t* j;
    int64_t max = DYN_JAR_DEFAULT_MAX;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[0], "maxCookies");
        if (JS_IsException(m))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(m)) {
            if (JS_ToInt64(ctx, &max, m)) {
                JS_FreeValue(ctx, m);
                return JS_EXCEPTION;
            }
            if (max < 1 || max > 1000000) {
                JS_FreeValue(ctx, m);
                return JS_ThrowRangeError(ctx,
                    "CookieJar: maxCookies must be 1..1000000");
            }
        }
        JS_FreeValue(ctx, m);
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "CookieJar: options must be an object");
    }
    j = (dyn_cookiejar_t*)calloc(1, sizeof *j);
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->refs = 1;
    httpk_jar_init(&j->jar, (size_t)max);
    return dyn_res_wrap(ctx, new_target, dyn_cookiejar_class_id, j,
        dyn_cookiejar_dispose);
}

static const char* dyn_cookiejar_url(JSContext* ctx, JSValueConst v,
    const char* who, int* bad)
{
    size_t n = 0;
    const char* u;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "CookieJar.%s: url must be a string", who);
        return NULL;
    }
    u = JS_ToCStringLen(ctx, &n, v);
    *bad = u && strlen(u) != n;
    return u;
}

static JSValue dyn_cookiejar_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cookiejar_t* j = dyn_cookiejar_this(ctx, this_val);
    const char *u, *line;
    size_t ln;
    int rc, bad = 0;
    (void)argc;
    if (!j)
        return JS_EXCEPTION;
    if (!JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "CookieJar.setCookie: setCookie must be a string");
    u = dyn_cookiejar_url(ctx, argv[0], "setCookie", &bad);
    if (!u)
        return JS_EXCEPTION;
    line = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!line) {
        JS_FreeCString(ctx, u);
        return JS_EXCEPTION;
    }
    rc = bad ? HTTPK_CK_IGNORED : httpk_jar_set(&j->jar, u, line, ln, (int64_t)time(NULL));
    JS_FreeCString(ctx, line);
    JS_FreeCString(ctx, u);
    if (rc == HTTPK_CK_OOM)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewString(ctx, rc == HTTPK_CK_STORED ? "stored" : rc == HTTPK_CK_DELETED ? "deleted"
                                                                                       : "ignored");
}

static JSValue dyn_cookiejar_cookie_header(JSContext* ctx,
    JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_cookiejar_t* j = dyn_cookiejar_this(ctx, this_val);
    const char* u;
    dyn_sb_t sb;
    JSValue r;
    int bad = 0;
    (void)argc;
    if (!j)
        return JS_EXCEPTION;
    u = dyn_cookiejar_url(ctx, argv[0], "cookieHeader", &bad);
    if (!u)
        return JS_EXCEPTION;
    dyn_sb_init(&sb);
    if (!bad)
        dyn_cookiejar_header(j, u, &sb);
    JS_FreeCString(ctx, u);
    if (sb.oom) {
        dyn_sb_free(&sb);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, sb.p ? sb.p : "", sb.n);
    dyn_sb_free(&sb);
    return r;
}

static JSValue dyn_cookiejar_store(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cookiejar_t* j = dyn_cookiejar_this(ctx, this_val);
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    const char* u;
    int stored = 0, bad = 0, rc;
    int64_t now = (int64_t)time(NULL);
    (void)argc;
    if (!j)
        return JS_EXCEPTION;
    if (!JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "CookieJar.storeResponse: headers must be an object");
    u = dyn_cookiejar_url(ctx, argv[0], "storeResponse", &bad);
    if (!u)
        return JS_EXCEPTION;
    if (bad) {
        JS_FreeCString(ctx, u);
        return JS_NewInt32(ctx, 0);
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[1],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeCString(ctx, u);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        const char *k = JS_AtomToCString(ctx, tab[i].atom), *s;
        size_t vn = 0, b = 0, e;
        JSValue v;
        int is_sc;
        if (!k)
            goto fail;
        is_sc = strcasecmp(k, "set-cookie") == 0;
        JS_FreeCString(ctx, k);
        if (!is_sc)
            continue;
        v = JS_GetProperty(ctx, argv[1], tab[i].atom);
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsString(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        s = JS_ToCStringLen(ctx, &vn, v);
        JS_FreeValue(ctx, v);
        if (!s)
            goto fail;
        while (b < vn) {
            e = b;
            while (e < vn && s[e] != '\n')
                e++;
            rc = httpk_jar_set(&j->jar, u, s + b, e - b, now);
            if (rc == HTTPK_CK_OOM) {
                JS_FreeCString(ctx, s);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            stored += rc == HTTPK_CK_STORED;
            b = e + 1;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeCString(ctx, u);
    return JS_NewInt32(ctx, stored);
fail:
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeCString(ctx, u);
    return JS_EXCEPTION;
}

static JSValue dyn_cookiejar_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cookiejar_t* j = dyn_cookiejar_this(ctx, this_val);
    size_t max;
    (void)argc;
    (void)argv;
    if (!j)
        return JS_EXCEPTION;
    max = j->jar.max_cookies;
    httpk_jar_free(&j->jar);
    httpk_jar_init(&j->jar, max);
    return JS_UNDEFINED;
}

static JSValue dyn_cookiejar_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_cookiejar_t* j = dyn_cookiejar_this(ctx, this_val);
    if (!j)
        return JS_EXCEPTION;
    httpk_jar_purge(&j->jar, (int64_t)time(NULL));
    return JS_NewInt64(ctx, (int64_t)j->jar.n);
}

static const JSCFunctionListEntry dyn_cookiejar_proto[] = {
    JS_CFUNC_DEF("setCookie", 2, dyn_cookiejar_set),
    JS_CFUNC_DEF("cookieHeader", 1, dyn_cookiejar_cookie_header),
    JS_CFUNC_DEF("storeResponse", 2, dyn_cookiejar_store),
    JS_CFUNC_DEF("clear", 0, dyn_cookiejar_clear),
    JS_CGETSET_DEF("size", dyn_cookiejar_size, NULL),
};

static const JSClassDef dyn_cookiejar_class = {
    "CookieJar",
    .finalizer = dyn_res_finalizer,
};

char* dyn_cookiejar_with_header(dyn_cookiejar_t* j, const char* url,
    char* hdr, int* oom)
{
    dyn_sb_t sb;
    size_t hl;
    char* out;
    *oom = 0;
    if (!j || (hdr && httpk_block_has(hdr, strlen(hdr), "cookie")))
        return hdr;
    dyn_sb_init(&sb);
    dyn_cookiejar_header(j, url, &sb);
    if (sb.oom || sb.n == 0) {
        *oom = sb.oom;
        dyn_sb_free(&sb);
        return hdr;
    }
    hl = hdr ? strlen(hdr) : 0;
    out = (char*)malloc(hl + 8 + sb.n + 3);
    if (!out) {
        *oom = 1;
        dyn_sb_free(&sb);
        return hdr;
    }
    if (hl)
        memcpy(out, hdr, hl);
    memcpy(out + hl, "Cookie: ", 8);
    memcpy(out + hl + 8, sb.p, sb.n);
    memcpy(out + hl + 8 + sb.n, "\r\n", 3);
    dyn_sb_free(&sb);
    free(hdr);
    return out;
}

JSValue dyn_http_client_use_cookies(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    dyn_cookiejar_t* j = NULL;
    if (!cl)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        j = dyn_cookiejar_of(ctx, argv[0], "HTTPClient.useCookies");
        if (!j)
            return JS_EXCEPTION;
    }
    dyn_cookiejar_ref(j);
    dyn_cookiejar_unref(cl->jar);
    cl->jar = j;
    return JS_UNDEFINED;
}

int hc_client_adopt_jar(JSValueConst client, dyn_cookiejar_t* j)
{
    DynResource* r = (DynResource*)JS_GetOpaque(client, dyn_http_client_class_id);
    dyn_http_client_t* cl;
    if (!r || r->closed || !r->native)
        return 0;
    cl = (dyn_http_client_t*)r->native;
    dyn_cookiejar_ref(j);
    dyn_cookiejar_unref(cl->jar);
    cl->jar = j;
    return 1;
}

int dyn_cookiejar_register(JSContext* ctx, JSModuleDef* m,
    int (*reg)(JSContext*, JSModuleDef*, JSClassID*, const JSClassDef*,
        const JSCFunctionListEntry*, int, JSCFunction*, const char*))
{
    return reg(ctx, m, &dyn_cookiejar_class_id, &dyn_cookiejar_class,
        dyn_cookiejar_proto, (int)countof(dyn_cookiejar_proto),
        (JSCFunction*)dyn_cookiejar_ctor, "CookieJar");
}
#endif
