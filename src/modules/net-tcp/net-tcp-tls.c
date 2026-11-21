#include "net-tcp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#ifdef CONFIG_TLS
static const char* const tcp_tls_srv_keys[] = { "cert", "key", "alpn", "ca", "requestCert" };
static const char* const tcp_tls_cli_keys[] = { "ca", "servername", "minVersion", "rejectUnauthorized", "cert", "key",
    "alpn" };
#endif

#ifdef CONFIG_TLS
static JSValue tcp_own_get(JSContext* ctx, JSValueConst obj, const char* key)
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

int tcp_setup_tls_server(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts)
{
    JSValue v;
    const char *cert = NULL, *key = NULL, *ca = NULL;
    char *alpn = NULL, err[256];
    dyn_tls_srv_opts_t so;
    JSValue w;
    int rc = -1, request_cert = 0;

    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "tls");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (dyn_opts_strict(ctx, v, tcp_tls_srv_keys, 5)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    w = tcp_own_get(ctx, v, "cert");
    if (JS_IsString(w) || dyn_value_is_path(w))
        cert = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = tcp_own_get(ctx, v, "key");
    if (JS_IsString(w) || dyn_value_is_path(w))
        key = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = tcp_own_get(ctx, v, "alpn");
    if (JS_IsString(w)) {
        const char* sa = JS_ToCString(ctx, w);
        if (sa) {
            alpn = strdup(sa);
            JS_FreeCString(ctx, sa);
        }
    }
    JS_FreeValue(ctx, w);
    w = tcp_own_get(ctx, v, "ca");
    if (JS_IsString(w) || dyn_value_is_path(w))
        ca = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = tcp_own_get(ctx, v, "requestCert");
    if (JS_ToBool(ctx, w) == 1)
        request_cert = 1;
    JS_FreeValue(ctx, w);
    memset(&so, 0, sizeof so);
    so.cert = cert;
    so.key = key;
    so.alpn = alpn;
    so.ca_file = ca;
    so.request_cert = request_cert;
    if (!cert || !key) {
        JS_ThrowTypeError(ctx, "TCPServer: tls needs both `cert` and `key` "
                               "(PEM paths); there is no self-signed default");
        goto done;
    }
    t->tls_ctx = dyn_tls_ctx_server(&so, err, sizeof err);
    if (!t->tls_ctx) {
        JS_ThrowInternalError(ctx, "TCPServer: tls: %s", err);
        goto done;
    }
    rc = 0;
done:
    if (cert)
        JS_FreeCString(ctx, cert);
    if (key)
        JS_FreeCString(ctx, key);
    if (ca)
        JS_FreeCString(ctx, ca);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}

int tcp_setup_tls(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts,
    const char* host)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    dyn_tls_opts_t o;
    const char *ca = NULL, *sni = NULL, *minv = NULL;
    const char *cert = NULL, *key = NULL;
    char *alpn = NULL, err[192];
    int rc = -1;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v) || (JS_IsBool(v) && !JS_ToBool(ctx, v))) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.min_version = 12;
    if (JS_IsObject(v)) {
        if (dyn_opts_strict(ctx, v, tcp_tls_cli_keys, 7)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JSValue w;
        w = tcp_own_get(ctx, v, "ca");
        if (JS_IsString(w) || dyn_value_is_path(w))
            ca = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "servername");
        if (JS_IsString(w))
            sni = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "minVersion");
        if (JS_IsString(w))
            minv = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "rejectUnauthorized");
        if (JS_IsBool(w) && !JS_ToBool(ctx, w))
            o.insecure = 1;
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "cert");
        if (JS_IsString(w) || dyn_value_is_path(w))
            cert = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "key");
        if (JS_IsString(w) || dyn_value_is_path(w))
            key = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = tcp_own_get(ctx, v, "alpn");
        if (JS_IsString(w)) {
            const char* s = JS_ToCString(ctx, w);
            if (s) {
                alpn = strdup(s);
                JS_FreeCString(ctx, s);
            }
        } else if (JS_IsArray(ctx, w)) {
            uint32_t i, n = 0;
            if (dyn_js_len_u32(ctx, w, &n)) {
                JS_FreeValue(ctx, w);
                goto out;
            }
            for (i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, w, i);
                const char* s = JS_ToCString(ctx, e);
                JS_FreeValue(ctx, e);
                if (!s) {
                    JS_FreeValue(ctx, w);
                    goto out;
                }
                {
                    size_t slen = strlen(s);
                    size_t have = alpn ? strlen(alpn) : 0;
                    char* nw = (char*)realloc(alpn, have + slen + 2);
                    if (nw) {
                        alpn = nw;
                        if (have)
                            alpn[have++] = ',';
                        memcpy(alpn + have, s, slen + 1);
                    }
                    JS_FreeCString(ctx, s);
                }
            }
        }
        JS_FreeValue(ctx, w);
    }
    o.ca_file = ca;
    o.cert = cert;
    o.key = key;
    o.alpn = alpn;
    if (minv && strstr(minv, "1.3"))
        o.min_version = 13;

    t->tls_servername = strdup(sni ? sni : (host ? host : ""));
    t->tls_ctx = dyn_tls_ctx_client(&o, err, sizeof err);
    if (!t->tls_ctx)
        JS_ThrowInternalError(ctx, "tls: %s", err);
    else
        rc = 0;

out:
    if (ca)
        JS_FreeCString(ctx, ca);
    if (cert)
        JS_FreeCString(ctx, cert);
    if (key)
        JS_FreeCString(ctx, key);
    if (sni)
        JS_FreeCString(ctx, sni);
    if (minv)
        JS_FreeCString(ctx, minv);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}
#endif

#endif
