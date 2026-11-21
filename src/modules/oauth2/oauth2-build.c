#include "oauth2-internal.h"
#include "core/dyn-snappend.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

static char* form_encode_component(JSContext* ctx, const char* s, size_t n, size_t* out_len)
{
    char* out = (char*)js_malloc(ctx, n * 3 + 1);
    dyn_pct_buf_t b;
    if (!out)
        return NULL;
    b.p = out;
    b.n = 0;
    dyn_pct_encode_core(&b, dyn_pct_buf_sink, s, n, 1);
    out[b.n] = '\0';
    if (out_len)
        *out_len = b.n;
    return out;
}

JSValue oauth_build_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* t;
    size_t n;
    char* out;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "buildBearerHeader(token: string)");
    t = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    if (!is_valid_b64token(t, n, 0) || has_ctl_or_crlf(t, n)) {
        JS_FreeCString(ctx, t);
        return JS_ThrowTypeError(ctx, "buildBearerHeader: token must be b64token (ALPHA/DIGIT/-._~+/ and = padding)");
    }
    out = (char*)js_malloc(ctx, 7 + n + 1);
    if (!out) {
        JS_FreeCString(ctx, t);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(out, "Bearer ", 7);
    memcpy(out + 7, t, n);
    out[7 + n] = '\0';
    JS_FreeCString(ctx, t);
    r = JS_NewStringLen(ctx, out, 7 + n);
    js_free(ctx, out);
    return r;
}

static const char* const oauth_www_keys[] = { "realm", "error", "errorDescription", "error_description", "errorUri", "error_uri", "scope", "strict" };
static const char* const oauth_auth_url_keys[] = { "authorizationEndpoint", "clientId", "redirectUri", "responseType", "scope", "state", "codeChallenge", "codeChallengeMethod", "allowPlain", "allowInsecure", "extraParams" };

static int www_field(JSContext* ctx, JSValueConst o, const char* k, const char* alt, const char** s, size_t* n)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    *s = NULL;
    *n = 0;
    if (alt && JS_IsUndefined(v))
        v = JS_GetPropertyStr(ctx, o, alt);
    if (JS_IsException(v))
        return -1;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    JS_FreeValue(ctx, v);
    return *s ? 0 : -1;
}

JSValue oauth_build_www(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue opts = JS_UNDEFINED;
    const char *realm = NULL, *error = NULL, *desc = NULL, *uri = NULL, *scope = NULL;
    int strict = 0;
    size_t rn = 0, en = 0, dn = 0, un = 0, sn = 0;
    char* out;
    size_t cap = 512;
    size_t off = 0;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "buildWWWAuthenticate({realm, error, ...})");
    opts = argv[0];
    if (dyn_opts_strict(ctx, opts, oauth_www_keys, countof(oauth_www_keys)) < 0
        || dyn_opt_bool(ctx, opts, "strict", "buildWWWAuthenticate", &strict) < 0)
        return JS_EXCEPTION;
    if (www_field(ctx, opts, "realm", NULL, &realm, &rn) || www_field(ctx, opts, "error", NULL, &error, &en)
        || www_field(ctx, opts, "errorDescription", "error_description", &desc, &dn)
        || www_field(ctx, opts, "errorUri", "error_uri", &uri, &un) || www_field(ctx, opts, "scope", NULL, &scope, &sn))
        goto fail;
    if (realm && !is_valid_error_desc(realm, rn, 1))
        goto bad;
    if (error && !is_valid_error(error, en, strict))
        goto bad;
    if (desc && !is_valid_error_desc(desc, dn, 1))
        goto bad;
    if (uri && !is_valid_error_desc(uri, un, 0))
        goto bad;
    if (scope) {
        size_t i;
        for (i = 0; i < sn; i++)
            if (!is_nqchar((unsigned char)scope[i]) && scope[i] != ' ')
                goto bad;
    }
    if (!error)
        goto need_error;
    {
        size_t need = 7 + (rn ? rn + 9 : 0) + (en ? en + 11 : 0) + (dn ? dn + 22 : 0)
            + (un ? un + 14 : 0) + (sn ? sn + 10 : 0);
        if (need > cap) {
            cap = need;
        }
    }
    out = (char*)js_malloc(ctx, cap);
    if (!out)
        goto oom;
    off = dyn_snappend(out, cap, off, "Bearer");
    if (realm) {
        off = dyn_snappend(out, cap, off, " realm=\"%.*s\"", (int)rn, realm);
    }
    if (error) {
        off = dyn_snappend(out, cap, off, "%s error=\"%.*s\"", realm ? "," : "", (int)en, error);
    }
    if (desc) {
        off = dyn_snappend(out, cap, off, ", error_description=\"%.*s\"", (int)dn, desc);
    }
    if (uri) {
        off = dyn_snappend(out, cap, off, ", error_uri=\"%.*s\"", (int)un, uri);
    }
    if (scope) {
        off = dyn_snappend(out, cap, off, ", scope=\"%.*s\"", (int)sn, scope);
    }
    r = JS_NewStringLen(ctx, out, off);
    js_free(ctx, out);
    if (realm)
        JS_FreeCString(ctx, realm);
    if (error)
        JS_FreeCString(ctx, error);
    if (desc)
        JS_FreeCString(ctx, desc);
    if (uri)
        JS_FreeCString(ctx, uri);
    if (scope)
        JS_FreeCString(ctx, scope);
    return r;
bad:
    if (realm)
        JS_FreeCString(ctx, realm);
    if (error)
        JS_FreeCString(ctx, error);
    if (desc)
        JS_FreeCString(ctx, desc);
    if (uri)
        JS_FreeCString(ctx, uri);
    if (scope)
        JS_FreeCString(ctx, scope);
    return JS_ThrowTypeError(ctx, "buildWWWAuthenticate: invalid chars in realm/error/description/uri/scope");
need_error:
    if (realm)
        JS_FreeCString(ctx, realm);
    if (error)
        JS_FreeCString(ctx, error);
    if (desc)
        JS_FreeCString(ctx, desc);
    if (uri)
        JS_FreeCString(ctx, uri);
    if (scope)
        JS_FreeCString(ctx, scope);
    return JS_ThrowTypeError(ctx, "buildWWWAuthenticate: error is required (RFC 6750 §3)");
oom:
    if (realm)
        JS_FreeCString(ctx, realm);
    if (error)
        JS_FreeCString(ctx, error);
    if (desc)
        JS_FreeCString(ctx, desc);
    if (uri)
        JS_FreeCString(ctx, uri);
    if (scope)
        JS_FreeCString(ctx, scope);
    return JS_ThrowOutOfMemory(ctx);
fail:
    if (realm)
        JS_FreeCString(ctx, realm);
    if (error)
        JS_FreeCString(ctx, error);
    if (desc)
        JS_FreeCString(ctx, desc);
    if (uri)
        JS_FreeCString(ctx, uri);
    if (scope)
        JS_FreeCString(ctx, scope);
    return JS_EXCEPTION;
}

JSValue oauth_build_auth_url(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue opts, v;
    const char *endpoint = NULL, *clientId = NULL, *redirectUri = NULL, *scope = NULL, *state = NULL, *cc = NULL, *ccm = NULL;
    size_t en = 0, cn = 0, rn = 0, sn = 0, scn = 0, ccn = 0, ccmn = 0;
    int has_code_challenge = 0;
    int ccm_is_malloc = 0;
    char* url = NULL;
    size_t url_cap = 4096, url_len = 0;
    JSPropertyEnum* ep_tab = NULL;
    uint32_t ep_len = 0;
    const char* ep_k = NULL;
    const char* ep_es = NULL;
    JSValue ep_ev = JS_UNDEFINED;
    int ep_live = 0;
    JSValue r = JS_EXCEPTION;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "buildAuthorizationUrl(opts: {authorizationEndpoint, clientId, redirectUri})");
    opts = argv[0];
    if (dyn_opts_strict(ctx, opts, oauth_auth_url_keys, countof(oauth_auth_url_keys)) < 0)
        return JS_EXCEPTION;
    if (www_field(ctx, opts, "authorizationEndpoint", NULL, &endpoint, &en)
        || www_field(ctx, opts, "clientId", NULL, &clientId, &cn)
        || www_field(ctx, opts, "redirectUri", NULL, &redirectUri, &rn))
        goto cleanup;
    if (!endpoint || !clientId || !redirectUri || en == 0 || cn == 0 || rn == 0) {
        goto fail;
    }
    if (has_ctl_or_crlf(endpoint, en) || has_ctl_or_crlf(clientId, cn) || has_ctl_or_crlf(redirectUri, rn))
        goto bad;
    {
        int allowInsecure = dyn_get_truthy(ctx, opts, "allowInsecure");
        if (allowInsecure < 0)
            goto cleanup;
        if (!allowInsecure) {
            int is_https = (en >= 8 && !memcmp(endpoint, "https://", 8));
            int is_loopback = (rn >= 22 && (!memcmp(redirectUri, "http://127.0.0.1", 16) || !memcmp(redirectUri, "http://[::1]", 12) || !memcmp(redirectUri, "http://localhost", 16)));
            if (!is_https && !is_loopback) {
                r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: authorizationEndpoint must be https (except loopback); pass allowInsecure:true to override");
                goto cleanup;
            }
        }
    }
    if (www_field(ctx, opts, "scope", NULL, &scope, &sn))
        goto cleanup;
    if (scope) {
        size_t i;
        int in_tok = 0;
        if (sn == 0 || scope[0] == ' ' || scope[sn - 1] == ' ')
            goto bad;
        for (i = 0; i < sn; i++) {
            unsigned char c = (unsigned char)scope[i];
            if (c == ' ') {
                if (!in_tok)
                    goto bad;
                in_tok = 0;
            } else {
                if (!is_nqchar(c))
                    goto bad;
                in_tok = 1;
            }
        }
    }
    int state_is_malloc = 0;
    if (dyn_get(ctx, opts, "state", &v))
        goto cleanup;
    if (JS_IsString(v)) {
        state = JS_ToCStringLen(ctx, &scn, v);
        if (!state) {
            JS_FreeValue(ctx, v);
            goto cleanup;
        }
        if (scn == 0) {
            JS_FreeCString(ctx, state);
            state = NULL;
            JS_FreeValue(ctx, v);
            r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: state must not be empty");
            goto cleanup;
        }
    } else if (JS_IsUndefined(v) || JS_IsNull(v)) {
        uint8_t sraw[32];
        if (dyn_os_entropy(sraw, sizeof sraw) != 0) {
            JS_FreeValue(ctx, v);
            r = JS_ThrowInternalError(ctx, "buildAuthorizationUrl: OS entropy unavailable");
            goto cleanup;
        }
        {
            char tmp[64];
            size_t tm = dyn_codec_base64url_encode(sraw, sizeof sraw, tmp);
            char* ms = (char*)js_malloc(ctx, tm + 1);
            if (!ms) {
                JS_FreeValue(ctx, v);
                r = JS_ThrowOutOfMemory(ctx);
                goto cleanup;
            }
            memcpy(ms, tmp, tm);
            ms[tm] = '\0';
            state = ms;
            scn = tm;
            state_is_malloc = 1;
        }
    } else {
        JS_FreeValue(ctx, v);
        r = JS_ThrowTypeError(ctx,
            "buildAuthorizationUrl: state must be a string");
        goto cleanup;
    }
    JS_FreeValue(ctx, v);
    if (dyn_get(ctx, opts, "codeChallenge", &v))
        goto cleanup;
    if (JS_IsString(v)) {
        cc = JS_ToCStringLen(ctx, &ccn, v);
        if (!cc) {
            JS_FreeValue(ctx, v);
            goto cleanup;
        }
        has_code_challenge = 1;
    } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        r = JS_ThrowTypeError(ctx,
            "buildAuthorizationUrl: codeChallenge must be a string");
        goto cleanup;
    }
    JS_FreeValue(ctx, v);
    if (dyn_get(ctx, opts, "codeChallengeMethod", &v))
        goto cleanup;
    if (JS_IsString(v)) {
        ccm = JS_ToCStringLen(ctx, &ccmn, v);
        if (!ccm) {
            JS_FreeValue(ctx, v);
            goto cleanup;
        }
    } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        r = JS_ThrowTypeError(ctx,
            "buildAuthorizationUrl: codeChallengeMethod must be a string");
        goto cleanup;
    }
    JS_FreeValue(ctx, v);
    if (cc && ccm && strcmp(ccm, "S256") && strcmp(ccm, "plain"))
        goto bad;
    if (cc) {
        size_t k;
        int chal_is_s256 = (ccm && !strcmp(ccm, "plain")) ? 0 : 1;
        if (chal_is_s256 ? (ccn != 43) : (ccn < 43 || ccn > 128))
            goto bad;
        for (k = 0; k < ccn; k++)
            if (!is_unreserved((unsigned char)cc[k]))
                goto bad;
    }
    if (has_code_challenge && !ccm) {
        ccm = (const char*)js_malloc(ctx, 5);
        if (!ccm)
            goto oom;
        memcpy(DYN_UNCONST(ccm), "S256", 4);
        ((char*)DYN_UNCONST(ccm))[4] = '\0';
        ccmn = 4;
        ccm_is_malloc = 1;
    }
    if (cc && ccm && !strcmp(ccm, "plain")) {
        int allow = dyn_get_truthy(ctx, opts, "allowPlain");
        if (allow < 0)
            goto cleanup;
        if (!allow)
            goto bad;
    }
    if (dyn_get(ctx, opts, "responseType", &v))
        goto cleanup;
    if (JS_IsString(v)) {
        const char* rt;
        size_t rtn = 0;
        rt = JS_ToCStringLen(ctx, &rtn, v);
        if (!rt) {
            JS_FreeValue(ctx, v);
            goto cleanup;
        }
        int ok = (rtn == 4 && !memcmp(rt, "code", 4));
        JS_FreeCString(ctx, rt);
        JS_FreeValue(ctx, v);
        if (!ok) {
            r = JS_ThrowTypeError(ctx, "unsupported_response_type: only 'code' is supported");
            goto cleanup;
        }
    } else
        JS_FreeValue(ctx, v);
    url = (char*)js_malloc(ctx, en + 1);
    if (!url)
        goto oom;
    memcpy(url, endpoint, en);
    url[en] = '\0';
    url_len = en;
    url_cap = en + 1;
    {
        int has_q = (memchr(endpoint, '?', en) != NULL);
        char* tmp;
        size_t tmpl;
#define APPEND_PARAM(k, vn, v)                                   \
    do {                                                         \
        char *kenc, *venc;                                       \
        kenc = form_encode_component(ctx, k, strlen(k), &tmpl);  \
        if (!kenc)                                               \
            goto oom;                                            \
        tmp = form_encode_component(ctx, v, vn, &tmpl);          \
        if (!tmp) {                                              \
            js_free(ctx, kenc);                                  \
            goto oom;                                            \
        }                                                        \
        venc = tmp;                                              \
        size_t need = url_len + 1 + strlen(kenc) + 1 + tmpl + 1; \
        if (need > url_cap) {                                    \
            char* nb = (char*)js_realloc(ctx, url, need + 256);  \
            if (!nb) {                                           \
                js_free(ctx, kenc);                              \
                js_free(ctx, venc);                              \
                goto oom;                                        \
            }                                                    \
            url = nb;                                            \
            url_cap = need + 256;                                \
        }                                                        \
        url[url_len++] = has_q ? '&' : '?';                      \
        has_q = 1;                                               \
        memcpy(url + url_len, kenc, strlen(kenc));               \
        url_len += strlen(kenc);                                 \
        url[url_len++] = '=';                                    \
        memcpy(url + url_len, venc, tmpl);                       \
        url_len += tmpl;                                         \
        url[url_len] = '\0';                                     \
        js_free(ctx, kenc);                                      \
        js_free(ctx, venc);                                      \
    } while (0)
        APPEND_PARAM("response_type", 4, "code");
        APPEND_PARAM("client_id", cn, clientId);
        APPEND_PARAM("redirect_uri", rn, redirectUri);
        if (scope)
            APPEND_PARAM("scope", sn, scope);
        if (state)
            APPEND_PARAM("state", scn, state);
        if (has_code_challenge) {
            APPEND_PARAM("code_challenge", ccn, cc);
            if (ccm)
                APPEND_PARAM("code_challenge_method", ccmn, ccm);
        }
        if (dyn_get(ctx, opts, "extraParams", &v))
            goto cleanup;
        if (JS_IsObject(v) && !JS_IsArray(ctx, v)) {
            ep_live = 1;
            if (JS_GetOwnPropertyNames(ctx, &ep_tab, &ep_len, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, v);
                goto cleanup;
            } else {
                for (uint32_t i = 0; i < ep_len; i++) {
                    size_t kn = 0;
                    const char* k = ep_k = JS_AtomToCStringLen(ctx, &kn, ep_tab[i].atom);
                    JSValue ev = JS_GetProperty(ctx, v, ep_tab[i].atom);
                    const char* es = ep_es = NULL;
                    size_t esn = 0;
                    if (!k || JS_IsException(ev)) {
                        JS_FreePropertyEnum(ctx, ep_tab, ep_len);
                        if (k)
                            JS_FreeCString(ctx, k);
                        JS_FreeValue(ctx, ev);
                        JS_FreeValue(ctx, v);
                        goto cleanup;
                    }
                    ep_ev = ev;
                    if (k && strlen(k) == kn) {
                        if (!strcmp(k, "response_type") || !strcmp(k, "client_id") || !strcmp(k, "redirect_uri") || !strcmp(k, "scope") || !strcmp(k, "state") || !strcmp(k, "code_challenge") || !strcmp(k, "code_challenge_method")) {
                            JS_FreeCString(ctx, k);
                            JS_FreeValue(ctx, ev);
                            continue;
                        }
                    }
                    if (k && !JS_IsString(ev)) {
                        r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: extraParams values must be strings");
                        JS_FreePropertyEnum(ctx, ep_tab, ep_len);
                        JS_FreeCString(ctx, k);
                        JS_FreeValue(ctx, ev);
                        JS_FreeValue(ctx, v);
                        goto cleanup;
                    }
                    if (k && JS_IsString(ev)) {
                        es = ep_es = JS_ToCStringLen(ctx, &esn, ev);
                        if (!es) {
                            r = JS_EXCEPTION;
                            JS_FreePropertyEnum(ctx, ep_tab, ep_len);
                            JS_FreeCString(ctx, k);
                            JS_FreeValue(ctx, ev);
                            JS_FreeValue(ctx, v);
                            goto cleanup;
                        }
                        if (has_ctl_or_crlf(k, kn) || has_ctl_or_crlf(es, esn)) {
                            r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: extraParams key/value must not contain CTL/CRLF");
                            JS_FreePropertyEnum(ctx, ep_tab, ep_len);
                            JS_FreeCString(ctx, k);
                            JS_FreeCString(ctx, es);
                            JS_FreeValue(ctx, ev);
                            JS_FreeValue(ctx, v);
                            goto cleanup;
                        }
                        APPEND_PARAM(k, esn, es);
                        JS_FreeCString(ctx, es);
                        ep_es = NULL;
                    }
                    if (k) {
                        JS_FreeCString(ctx, k);
                        ep_k = NULL;
                    }
                    JS_FreeValue(ctx, ev);
                    ep_ev = JS_UNDEFINED;
                }
                JS_FreePropertyEnum(ctx, ep_tab, ep_len);
                ep_tab = NULL;
            }
        }
        JS_FreeValue(ctx, v);
        ep_live = 0;
#undef APPEND_PARAM
    }
    {
        JSValue obj = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, obj, "url", JS_NewStringLen(ctx, url, url_len), JS_PROP_C_W_E);
        if (state)
            JS_DefinePropertyValueStr(ctx, obj, "state", JS_NewStringLen(ctx, state, scn), JS_PROP_C_W_E);
        r = obj;
    }
    goto cleanup;
bad:
    r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: invalid chars or scope");
    goto cleanup;
oom:
    if (ep_live) {
        if (ep_k)
            JS_FreeCString(ctx, ep_k);
        if (ep_es)
            JS_FreeCString(ctx, ep_es);
        JS_FreeValue(ctx, ep_ev);
        if (ep_tab)
            JS_FreePropertyEnum(ctx, ep_tab, ep_len);
        JS_FreeValue(ctx, v);
    }
    r = JS_ThrowOutOfMemory(ctx);
    goto cleanup;
fail:
    r = JS_ThrowTypeError(ctx, "buildAuthorizationUrl: authorizationEndpoint, clientId, redirectUri required strings");
cleanup:
    if (endpoint)
        JS_FreeCString(ctx, endpoint);
    if (clientId)
        JS_FreeCString(ctx, clientId);
    if (redirectUri)
        JS_FreeCString(ctx, redirectUri);
    if (scope)
        JS_FreeCString(ctx, scope);
    if (state) {
        if (state_is_malloc)
            js_free(ctx, DYN_UNCONST(state));
        else
            JS_FreeCString(ctx, state);
    }
    if (cc)
        JS_FreeCString(ctx, cc);
    if (ccm) {
        if (ccm_is_malloc)
            js_free(ctx, DYN_UNCONST(ccm));
        else
            JS_FreeCString(ctx, ccm);
    }
    if (url)
        js_free(ctx, url);
    return r;
}

JSValue oauth_build_token_body(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue obj;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0;
    char* out = NULL;
    size_t cap = 256, off = 0;
    JSValue r = JS_EXCEPTION;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "buildTokenRequestBody(params: Record<string,string>)");
    obj = argv[0];
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    out = (char*)js_malloc(ctx, cap);
    if (!out) {
        JS_FreePropertyEnum(ctx, tab, len);
        return JS_ThrowOutOfMemory(ctx);
    }
    out[0] = '\0';
    for (uint32_t i = 0; i < len; i++) {
        size_t kn = 0;
        const char* k = JS_AtomToCStringLen(ctx, &kn, tab[i].atom);
        JSValue ev = JS_GetProperty(ctx, obj, tab[i].atom);
        const char* es = NULL;
        size_t esn = 0;
        char *ek = NULL, *evv = NULL;
        if (!k || JS_IsException(ev)) {
            if (k)
                JS_FreeCString(ctx, k);
            JS_FreeValue(ctx, ev);
            goto done;
        }
        if (!JS_IsString(ev)) {
            r = JS_ThrowTypeError(ctx, "buildTokenRequestBody: value of '%s' must be a string", k);
            JS_FreeCString(ctx, k);
            JS_FreeValue(ctx, ev);
            goto done;
        }
        es = JS_ToCStringLen(ctx, &esn, ev);
        JS_FreeValue(ctx, ev);
        if (!es) {
            JS_FreeCString(ctx, k);
            r = JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        ek = form_encode_component(ctx, k, kn, NULL);
        evv = form_encode_component(ctx, es, esn, NULL);
        JS_FreeCString(ctx, k);
        JS_FreeCString(ctx, es);
        if (!ek || !evv) {
            if (ek)
                js_free(ctx, ek);
            if (evv)
                js_free(ctx, evv);
            r = JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        {
            size_t need = off + (off ? 1 : 0) + strlen(ek) + 1 + strlen(evv) + 1;
            if (need > cap) {
                size_t nc = need * 2;
                char* nb = (char*)js_realloc(ctx, out, nc);
                if (!nb) {
                    js_free(ctx, ek);
                    js_free(ctx, evv);
                    r = JS_ThrowOutOfMemory(ctx);
                    goto done;
                }
                out = nb;
                cap = nc;
            }
            if (off)
                out[off++] = '&';
            memcpy(out + off, ek, strlen(ek));
            off += strlen(ek);
            out[off++] = '=';
            memcpy(out + off, evv, strlen(evv));
            off += strlen(evv);
            out[off] = '\0';
        }
        js_free(ctx, ek);
        js_free(ctx, evv);
    }
    r = JS_NewStringLen(ctx, out, off);
done:
    if (out)
        js_free(ctx, out);
    JS_FreePropertyEnum(ctx, tab, len);
    return r;
}

JSValue oauth_build_client_auth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char *id = NULL, *sec = NULL;
    size_t idn = 0, secn = 0;
    char *ei = NULL, *es = NULL;
    size_t ein = 0, esn = 0;
    char *joined = NULL, *b64 = NULL;
    size_t blen;
    JSValue r = JS_EXCEPTION;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "buildClientAuthHeader(clientId, clientSecret?)");
    id = JS_ToCStringLen(ctx, &idn, argv[0]);
    if (!id)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsString(argv[1]))
        sec = JS_ToCStringLen(ctx, &secn, argv[1]);
    else
        secn = 0;
    if (has_ctl_or_crlf(id, idn) || (sec && has_ctl_or_crlf(sec, secn)))
        goto bad;
    ei = form_encode_component(ctx, id, idn, &ein);
    es = sec ? form_encode_component(ctx, sec, secn, &esn) : (char*)js_malloc(ctx, 1);
    if (!ei || !es)
        goto oom;
    if (!sec) {
        es[0] = '\0';
        esn = 0;
    }
    joined = (char*)js_malloc(ctx, ein + 1 + esn + 1);
    if (!joined)
        goto oom;
    memcpy(joined, ei, ein);
    joined[ein] = ':';
    memcpy(joined + ein + 1, es, esn);
    joined[ein + 1 + esn] = '\0';
    b64 = (char*)js_malloc(ctx, 4 * ((ein + 1 + esn + 2) / 3) + 1);
    if (!b64)
        goto oom;
    blen = dyn_codec_base64_encode((const uint8_t*)joined, ein + 1 + esn, b64);
    {
        char* out = (char*)js_malloc(ctx, 6 + blen + 1);
        if (!out)
            goto oom;
        memcpy(out, "Basic ", 6);
        memcpy(out + 6, b64, blen);
        out[6 + blen] = '\0';
        r = JS_NewStringLen(ctx, out, 6 + blen);
        oauth_wipe(out, 6 + blen);
        js_free(ctx, out);
    }
    goto cleanup;
bad:
    r = JS_ThrowTypeError(ctx, "buildClientAuthHeader: clientId/secret must not contain CTL/CRLF");
    goto cleanup;
oom:
    r = JS_ThrowOutOfMemory(ctx);
cleanup:
    if (id)
        JS_FreeCString(ctx, id);
    if (sec)
        JS_FreeCString(ctx, sec);
    if (ei)
        js_free(ctx, ei);
    if (es)
        js_free(ctx, es);
    if (joined) {
        oauth_wipe(joined, ein + 1 + esn);
        js_free(ctx, joined);
    }
    if (b64) {
        size_t k;
        for (k = 0; k < blen; k++)
            ((char*)b64)[k] = 0;
        js_free(ctx, b64);
    }
    return r;
}

#endif
