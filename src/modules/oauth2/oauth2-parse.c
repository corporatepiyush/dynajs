#include "oauth2-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

static void form_pct_decode(char* out, size_t* out_len, const char* s, size_t n)
{
    dyn_pct_buf_t b = { out, 0 };
    dyn_pct_decode_core(&b, dyn_pct_buf_sink, s, n, 1);
    out[b.n] = '\0';
    if (out_len)
        *out_len = b.n;
}

JSValue oauth_parse_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i, start = 0;
    JSValue arr;
    uint32_t idx = 0;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "parseScope(scope): string required");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > 4096) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "scope exceeds 4096 bytes");
    }
    if (n > 0 && (s[0] == ' ' || s[n - 1] == ' ')) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "scope: leading/trailing space");
    }
    for (i = 0; i <= n; i++) {
        int is_end = (i == n);
        int is_sp = (!is_end && s[i] == ' ');
        if (is_end || is_sp) {
            size_t len = i - start;
            if (len == 0 && !is_end) {
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx, "scope: empty token");
            }
            if (len) {
                size_t k;
                for (k = start; k < i; k++)
                    if (!is_nqchar((unsigned char)s[k])) {
                        JS_FreeCString(ctx, s);
                        return JS_ThrowTypeError(ctx, "scope token contains invalid char");
                    }
            }
            start = i + 1;
        }
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeCString(ctx, s);
        return arr;
    }
    start = 0;
    for (i = 0; i <= n; i++) {
        int is_end = (i == n);
        int is_sp = (!is_end && s[i] == ' ');
        if (is_end || is_sp) {
            size_t len = i - start;
            if (len) {
                JS_DefinePropertyValueUint32(ctx, arr, idx++, JS_NewStringLen(ctx, s + start, len), JS_PROP_C_W_E);
            }
            start = i + 1;
        }
    }
    JS_FreeCString(ctx, s);
    return arr;
}

JSValue oauth_parse_bearer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* h;
    size_t n, i = 0;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NULL;
    h = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    while (i < n && (h[i] == ' ' || h[i] == '\t'))
        i++;
    if (n - i < 6) {
        JS_FreeCString(ctx, h);
        return JS_NULL;
    }
    if ((h[i] != 'B' && h[i] != 'b') || (h[i + 1] != 'e' && h[i + 1] != 'E') || (h[i + 2] != 'a' && h[i + 2] != 'A') || (h[i + 3] != 'r' && h[i + 3] != 'R') || (h[i + 4] != 'e' && h[i + 4] != 'E') || (h[i + 5] != 'r' && h[i + 5] != 'R')) {
        JS_FreeCString(ctx, h);
        return JS_NULL;
    }
    i += 6;
    if (i >= n || (h[i] != ' ' && h[i] != '\t')) {
        JS_FreeCString(ctx, h);
        return JS_NULL;
    }
    while (i < n && (h[i] == ' ' || h[i] == '\t'))
        i++;
    if (i >= n) {
        JS_FreeCString(ctx, h);
        return JS_NULL;
    }
    {
        size_t tok = n - i;
        while (tok && (h[i + tok - 1] == ' ' || h[i + tok - 1] == '\t'))
            tok--;
        if (!is_valid_b64token(h + i, tok, 0)) {
            JS_FreeCString(ctx, h);
            return JS_NULL;
        }
        r = JS_NewStringLen(ctx, h + i, tok);
    }
    JS_FreeCString(ctx, h);
    return r;
}

JSValue oauth_parse_bearer_from_req(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue v;
    const char* s;
    size_t n;
    int allowQuery = 0, allowBody = 0;
    int found = 0;
    JSValue foundVal = JS_NULL;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "parseBearerFromRequest(req: {headers, query, body})");
    allowQuery = dyn_get_truthy(ctx, argv[0], "allowQuery");
    if (allowQuery < 0)
        return JS_EXCEPTION;
    allowBody = dyn_get_truthy(ctx, argv[0], "allowBody");
    if (allowBody < 0)
        return JS_EXCEPTION;
    if (dyn_get(ctx, argv[0], "headers", &v))
        return JS_EXCEPTION;
    if (JS_IsObject(v)) {
        JSPropertyEnum* htab = NULL;
        uint32_t hlen = 0, hk_i;
        JSValue hv = JS_UNDEFINED;
        if (JS_GetOwnPropertyNames(ctx, &htab, &hlen, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        } else {
            for (hk_i = 0; hk_i < hlen; hk_i++) {
                size_t hkn = 0;
                const char* hk = JS_AtomToCStringLen(ctx, &hkn, htab[hk_i].atom);
                int is_auth = 0;
                if (hk && hkn == sizeof "authorization" - 1) {
                    static const char want[] = "authorization";
                    size_t k;
                    is_auth = 1;
                    for (k = 0; k < sizeof want - 1; k++) {
                        unsigned char c = (unsigned char)hk[k];
                        if (c == '\0' || (c | 0x20) != (unsigned char)want[k]) {
                            is_auth = 0;
                            break;
                        }
                    }
                    if (is_auth && hk[sizeof want - 1] != '\0')
                        is_auth = 0;
                }
                if (is_auth) {
                    JSValue cand = JS_GetProperty(ctx, v, htab[hk_i].atom);
                    if (JS_IsException(cand)) {
                        if (hk)
                            JS_FreeCString(ctx, hk);
                        JS_FreeValue(ctx, hv);
                        JS_FreePropertyEnum(ctx, htab, hlen);
                        JS_FreeValue(ctx, v);
                        return JS_EXCEPTION;
                    }
                    if (JS_IsString(cand)) {
                        JS_FreeValue(ctx, hv);
                        hv = cand;
                    } else
                        JS_FreeValue(ctx, cand);
                }
                if (hk)
                    JS_FreeCString(ctx, hk);
            }
            if (JS_IsString(hv)) {
                JSValue p = oauth_parse_bearer(ctx, JS_UNDEFINED, 1, &hv);
                if (JS_IsException(p)) {
                    JS_FreeValue(ctx, hv);
                    JS_FreePropertyEnum(ctx, htab, hlen);
                    JS_FreeValue(ctx, v);
                    return JS_EXCEPTION;
                }
                if (!JS_IsNull(p)) {
                    found++;
                    foundVal = p;
                } else
                    JS_FreeValue(ctx, p);
            }
            JS_FreeValue(ctx, hv);
            JS_FreePropertyEnum(ctx, htab, hlen);
        }
    }
    JS_FreeValue(ctx, v);
    if (allowBody) {
        if (dyn_get(ctx, argv[0], "body", &v)) {
            JS_FreeValue(ctx, foundVal);
            return JS_EXCEPTION;
        }
        if (JS_IsString(v)) {
            s = JS_ToCStringLen(ctx, &n, v);
            if (!s) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, foundVal);
                return JS_EXCEPTION;
            } else {
                size_t i = 0;
                while (i < n) {
                    size_t b = i, eq = (size_t)-1;
                    while (i < n && s[i] != '&') {
                        if (s[i] == '=' && eq == (size_t)-1)
                            eq = i;
                        i++;
                    }
                    if (eq != (size_t)-1 && i - b > 13 && !memcmp(s + b, "access_token", 12) && s[b + 12] == '=') {
                        size_t vl = i - eq - 1;
                        char* dec = (char*)js_malloc(ctx, vl + 1);
                        size_t dl;
                        if (dec) {
                            form_pct_decode(dec, &dl, s + eq + 1, vl);
                            if (is_valid_b64token(dec, dl, 0)) {
                                if (found) {
                                    js_free(ctx, dec);
                                    JS_FreeCString(ctx, s);
                                    JS_FreeValue(ctx, v);
                                    JS_FreeValue(ctx, foundVal);
                                    return JS_ThrowTypeError(ctx, "parseBearerFromRequest: must not use more than one method (header + body)");
                                }
                                found++;
                                foundVal = JS_NewStringLen(ctx, dec, dl);
                            }
                            js_free(ctx, dec);
                        }
                    }
                    if (i < n && s[i] == '&')
                        i++;
                }
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (allowQuery) {
        if (dyn_get(ctx, argv[0], "query", &v)) {
            JS_FreeValue(ctx, foundVal);
            return JS_EXCEPTION;
        }
        if (JS_IsString(v)) {
            s = JS_ToCStringLen(ctx, &n, v);
            if (!s) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, foundVal);
                return JS_EXCEPTION;
            } else {
                size_t i = 0;
                const char* qs = s;
                size_t qn = n;
                if (qn && qs[0] == '?') {
                    qs++;
                    qn--;
                }
                while (i < qn) {
                    size_t b = i, eq = (size_t)-1;
                    while (i < qn && qs[i] != '&') {
                        if (qs[i] == '=' && eq == (size_t)-1)
                            eq = i;
                        i++;
                    }
                    if (eq != (size_t)-1 && i - b > 13 && !memcmp(qs + b, "access_token", 12) && qs[b + 12] == '=') {
                        size_t vl = i - eq - 1;
                        char* dec = (char*)js_malloc(ctx, vl + 1);
                        size_t dl;
                        if (dec) {
                            form_pct_decode(dec, &dl, qs + eq + 1, vl);
                            if (is_valid_b64token(dec, dl, 0)) {
                                if (found) {
                                    js_free(ctx, dec);
                                    JS_FreeCString(ctx, s);
                                    JS_FreeValue(ctx, v);
                                    JS_FreeValue(ctx, foundVal);
                                    return JS_ThrowTypeError(ctx, "parseBearerFromRequest: must not use more than one method (header/query/body)");
                                }
                                found++;
                                foundVal = JS_NewStringLen(ctx, dec, dl);
                            }
                            js_free(ctx, dec);
                        }
                    }
                    if (i < qn && qs[i] == '&')
                        i++;
                }
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (found)
        return foundVal;
    return JS_NULL;
}

JSValue oauth_parse_auth_resp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* url;
    size_t n;
    JSValue obj;
    int saw_code = 0, saw_error = 0, saw_state = 0, saw_desc = 0, saw_uri = 0;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse(url: string)");
    url = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    const char* q = strchr(url, '?');
    const char* f = strchr(url, '#');
    const char* search = q ? q + 1 : NULL;
    size_t slen = 0;
    if (search) {
        const char* end = f && f > search ? f : url + n;
        slen = end - search;
    } else if (f) {
        search = "";
        slen = 0;
    }
    if (f) {
        const char* fs = f + 1;
        size_t flen = (size_t)(url + n - fs);
        size_t i = 0;
        while (i < flen) {
            size_t b = i, eq = (size_t)-1;
            while (i < flen && fs[i] != '&') {
                if (fs[i] == '=' && eq == (size_t)-1)
                    eq = i;
                i++;
            }
            size_t kl = (eq == (size_t)-1 ? i - b : eq - b);
            if ((kl == 12 && !memcmp(fs + b, "access_token", 12)) || (kl == 10 && !memcmp(fs + b, "token_type", 10)) || (kl == 10 && !memcmp(fs + b, "expires_in", 10)) || (kl == 5 && !memcmp(fs + b, "scope", 5)) || (kl == 8 && !memcmp(fs + b, "id_token", 8))) {
                JS_FreeCString(ctx, url);
                return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: implicit-flow token in fragment not supported (code flow only)");
            }
            if (i < flen && fs[i] == '&')
                i++;
        }
    }
    obj = JS_NewObject(ctx);
    if (search && slen) {
        size_t i = 0;
        while (i < slen) {
            size_t b = i, eq = (size_t)-1;
            while (i < slen && search[i] != '&') {
                if (search[i] == '=' && eq == (size_t)-1)
                    eq = i;
                i++;
            }
            size_t kl = (eq == (size_t)-1 ? i - b : eq - b);
            size_t vl = (eq == (size_t)-1 ? 0 : i - eq - 1);
            char* k = (char*)js_malloc(ctx, kl + 1);
            char* v = (char*)js_malloc(ctx, vl + 1);
            size_t kl2, vl2;
            if (k && v) {
                form_pct_decode(k, &kl2, search + b, kl);
                if (eq != (size_t)-1)
                    form_pct_decode(v, &vl2, search + eq + 1, vl);
                else {
                    v[0] = '\0';
                    vl2 = 0;
                }
                if (!strcmp(k, "code")) {
                    if (saw_code) {
                        js_free(ctx, k);
                        js_free(ctx, v);
                        JS_FreeValue(ctx, obj);
                        JS_FreeCString(ctx, url);
                        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: duplicate code param");
                    }
                    saw_code = 1;
                    JS_DefinePropertyValueStr(ctx, obj, "code", JS_NewStringLen(ctx, v, vl2), JS_PROP_C_W_E);
                } else if (!strcmp(k, "state")) {
                    if (saw_state) {
                        js_free(ctx, k);
                        js_free(ctx, v);
                        JS_FreeValue(ctx, obj);
                        JS_FreeCString(ctx, url);
                        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: duplicate state param");
                    }
                    saw_state = 1;
                    JS_DefinePropertyValueStr(ctx, obj, "state", JS_NewStringLen(ctx, v, vl2), JS_PROP_C_W_E);
                } else if (!strcmp(k, "error")) {
                    if (saw_error) {
                        js_free(ctx, k);
                        js_free(ctx, v);
                        JS_FreeValue(ctx, obj);
                        JS_FreeCString(ctx, url);
                        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: duplicate error param");
                    }
                    saw_error = 1;
                    JS_DefinePropertyValueStr(ctx, obj, "error", JS_NewStringLen(ctx, v, vl2), JS_PROP_C_W_E);
                } else if (!strcmp(k, "error_description")) {
                    if (saw_desc) {
                        js_free(ctx, k);
                        js_free(ctx, v);
                        JS_FreeValue(ctx, obj);
                        JS_FreeCString(ctx, url);
                        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: duplicate error_description param");
                    }
                    saw_desc = 1;
                    JS_DefinePropertyValueStr(ctx, obj, "errorDescription", JS_NewStringLen(ctx, v, vl2), JS_PROP_C_W_E);
                } else if (!strcmp(k, "error_uri")) {
                    if (saw_uri) {
                        js_free(ctx, k);
                        js_free(ctx, v);
                        JS_FreeValue(ctx, obj);
                        JS_FreeCString(ctx, url);
                        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: duplicate error_uri param");
                    }
                    saw_uri = 1;
                    JS_DefinePropertyValueStr(ctx, obj, "errorUri", JS_NewStringLen(ctx, v, vl2), JS_PROP_C_W_E);
                }
            }
            if (k)
                js_free(ctx, k);
            if (v)
                js_free(ctx, v);
            if (i < slen && search[i] == '&')
                i++;
        }
    }
    JS_FreeCString(ctx, url);
    if (saw_code && saw_error) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "parseAuthorizationResponse: code and error must not both be present (6749 §4.1.2.1)");
    }
    return obj;
}

JSValue oauth_parse_token_resp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* body;
    size_t n;
    JSValue j;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "parseTokenResponse(body: string)");
    body = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!body)
        return JS_EXCEPTION;
    j = JS_ParseJSON(ctx, body, n, "<token>");
    JS_FreeCString(ctx, body);
    if (JS_IsException(j))
        return j;
    if (!JS_IsObject(j)) {
        JS_FreeValue(ctx, j);
        return JS_ThrowTypeError(ctx, "parseTokenResponse: body must be a JSON object");
    }
    {
        JSValue tv = JS_GetPropertyStr(ctx, j, "token_type");
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, j);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(tv)) {
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: token_type (string) required");
        }
        {
            const char* ts = JS_ToCString(ctx, tv);
            int ok;
            if (!ts) {
                JS_FreeValue(ctx, tv);
                JS_FreeValue(ctx, j);
                return JS_EXCEPTION;
            }
            ok = !strcasecmp(ts, "Bearer") || !strcasecmp(ts, "bearer");
            JS_FreeCString(ctx, ts);
            if (!ok) {
                JS_FreeValue(ctx, tv);
                JS_FreeValue(ctx, j);
                return JS_ThrowTypeError(ctx, "parseTokenResponse: token_type must be Bearer");
            }
        }
        JS_FreeValue(ctx, tv);
    }
    {
        JSValue tv = JS_GetPropertyStr(ctx, j, "access_token");
        size_t an = 0;
        int ok = 0;
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, j);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(tv)) {
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: access_token (string) required");
        }
        {
            const char* ts = JS_ToCStringLen(ctx, &an, tv);
            if (!ts) {
                JS_FreeValue(ctx, tv);
                JS_FreeValue(ctx, j);
                return JS_EXCEPTION;
            }
            ok = is_valid_b64token(ts, an, 0);
            JS_FreeCString(ctx, ts);
        }
        JS_FreeValue(ctx, tv);
        if (!ok) {
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: access_token must be a b64token");
        }
    }
    {
        JSValue tv = JS_GetPropertyStr(ctx, j, "expires_in");
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, j);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsNumber(tv)) {
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: expires_in must be a number");
        }
        JS_FreeValue(ctx, tv);
        tv = JS_GetPropertyStr(ctx, j, "scope");
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, j);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsString(tv)) {
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: scope must be a string");
        }
        JS_FreeValue(ctx, tv);
        tv = JS_GetPropertyStr(ctx, j, "refresh_token");
        if (JS_IsException(tv)) {
            JS_FreeValue(ctx, j);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(tv) && !JS_IsNull(tv) && !JS_IsString(tv)) {
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, j);
            return JS_ThrowTypeError(ctx, "parseTokenResponse: refresh_token must be a string");
        }
        JS_FreeValue(ctx, tv);
    }
    return j;
}

#endif
