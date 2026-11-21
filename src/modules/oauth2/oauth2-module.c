#include "oauth2-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

static int oauth_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pdata, size_t* plen, const char** powned, JSValue* pkeep)
{
    *powned = NULL;
    *pkeep = JS_UNDEFINED;
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
    {
        size_t off = 0, len = 0, bpe = 0, ab = 0;
        JSValue abv = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
        if (!JS_IsException(abv)) {
            uint8_t* base;
            if (bpe != 1) {
                JS_FreeValue(ctx, abv);
                JS_ThrowTypeError(ctx, "secureCompare: ByteView must be 1-byte (Uint8Array/DataView)");
                return -1;
            }
            base = JS_GetArrayBuffer(ctx, &ab, abv);
            JS_FreeValue(ctx, abv);
            if (!base)
                return -1;
            if (off > ab || len > ab - off) {
                JS_ThrowRangeError(ctx, "view out of bounds");
                return -1;
            }
            *pkeep = JS_DupValue(ctx, v);
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        {
            size_t n;
            uint8_t* p = JS_GetArrayBuffer(ctx, &n, v);
            if (p) {
                *pkeep = JS_DupValue(ctx, v);
                *pdata = p;
                *plen = n;
                return 0;
            }
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
}

static JSValue oauth_secure_compare(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const uint8_t *a = NULL, *b = NULL;
    size_t al = 0, bl = 0;
    const char *ao = NULL, *bo = NULL;
    JSValue ak = JS_UNDEFINED, bk = JS_UNDEFINED;
    uint8_t* acopy = NULL;
    int eq = 0;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "secureCompare(a, b): two BytesInput required");
    if (oauth_bytes(ctx, argv[0], &a, &al, &ao, &ak) < 0)
        return JS_EXCEPTION;
    acopy = (uint8_t*)js_malloc(ctx, al ? al : 1);
    if (!acopy) {
        if (ao)
            JS_FreeCString(ctx, ao);
        JS_FreeValue(ctx, ak);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (al)
        memcpy(acopy, a, al);
    if (ao)
        JS_FreeCString(ctx, ao);
    JS_FreeValue(ctx, ak);
    ak = JS_UNDEFINED;
    if (oauth_bytes(ctx, argv[1], &b, &bl, &bo, &bk) < 0) {
        oauth_wipe(acopy, al ? al : 1);
        js_free(ctx, acopy);
        return JS_EXCEPTION;
    }
    eq = (al == bl) && dyn_ct_equal(acopy, b, al);
    oauth_wipe(acopy, al ? al : 1);
    js_free(ctx, acopy);
    if (bo)
        JS_FreeCString(ctx, bo);
    JS_FreeValue(ctx, bk);
    return JS_NewBool(ctx, eq);
}

static JSValue oauth_format_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue lenv;
    uint32_t n = 0, i;
    char* out;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "formatScope(scopes: string[])");
    lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lenv)) {
        JS_FreeValue(ctx, lenv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (n == 0)
        return JS_NewString(ctx, "");
    if (n > (1u << 20)) {
        return JS_ThrowRangeError(ctx,
            "formatScope: at most 1048576 scope elements");
    }
    out = (char*)js_malloc(ctx, (size_t)n * 65 + 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    {
        size_t off = 0;
        for (i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            const char* s;
            size_t sl;
            if (JS_IsException(v)) {
                js_free(ctx, out);
                return JS_EXCEPTION;
            }
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                js_free(ctx, out);
                return JS_ThrowTypeError(ctx, "scope element must be string");
            }
            s = JS_ToCStringLen(ctx, &sl, v);
            JS_FreeValue(ctx, v);
            if (!s) {
                js_free(ctx, out);
                return JS_EXCEPTION;
            }
            if (sl == 0 || sl > 64) {
                JS_FreeCString(ctx, s);
                js_free(ctx, out);
                return JS_ThrowRangeError(ctx, "scope token length 1..64");
            }
            {
                size_t k;
                for (k = 0; k < sl; k++)
                    if (!is_nqchar((unsigned char)s[k])) {
                        JS_FreeCString(ctx, s);
                        js_free(ctx, out);
                        return JS_ThrowTypeError(ctx, "scope token invalid char");
                    }
            }
            if (off)
                out[off++] = ' ';
            memcpy(out + off, s, sl);
            off += sl;
            JS_FreeCString(ctx, s);
        }
        out[off] = '\0';
        r = JS_NewStringLen(ctx, out, off);
    }
    js_free(ctx, out);
    return r;
}

int oauth_str_list(JSContext* ctx, JSValueConst v, const char* what,
    char*** out, uint32_t* pn)
{
    uint32_t n = 1, i;
    char** l;
    int arr = JS_IsArray(ctx, v);
    if (!arr && !JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "%s must be a string or an array of strings", what);
        return -1;
    }
    if (arr) {
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        int bad = JS_IsException(lv) || JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        if (bad)
            return -1;
    }
    l = (char**)calloc(n ? (n < 64 ? n : 64) : 1, sizeof(char*));
    if (!l) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue e = arr ? JS_GetPropertyUint32(ctx, v, i) : JS_DupValue(ctx, v);
        const char* str;
        if (JS_IsException(e)) {
            oauth_str_list_free(l, i);
            return -1;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            oauth_str_list_free(l, i);
            JS_ThrowTypeError(ctx, "%s[%u] must be a string", what, (unsigned)i);
            return -1;
        }
        if (i >= 64 && (i & (i - 1)) == 0) {
            char** grown = (char**)realloc(l, (size_t)i * 2 * sizeof(char*));
            if (!grown) {
                JS_FreeValue(ctx, e);
                oauth_str_list_free(l, i);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            l = grown;
        }
        str = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!str) {
            oauth_str_list_free(l, i);
            return -1;
        }
        l[i] = strdup(str);
        JS_FreeCString(ctx, str);
        if (!l[i]) {
            oauth_str_list_free(l, i);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    *out = l;
    *pn = n;
    return 0;
}

static const JSCFunctionListEntry oauth_funcs[] = {
    JS_CFUNC_DEF("generateCodeVerifier", 0, oauth_gen_verifier),
    JS_CFUNC_DEF("isValidCodeVerifier", 1, oauth_is_valid_verifier),
    JS_CFUNC_DEF("generateCodeChallenge", 1, oauth_gen_challenge),
    JS_CFUNC_DEF("verifyCodeChallenge", 3, oauth_verify_challenge),
    JS_CFUNC_DEF("generateState", 0, oauth_gen_state),
    JS_CFUNC_DEF("secureCompare", 2, oauth_secure_compare),
    JS_CFUNC_DEF("parseScope", 1, oauth_parse_scope),
    JS_CFUNC_DEF("formatScope", 1, oauth_format_scope),
    JS_CFUNC_DEF("buildBearerHeader", 1, oauth_build_bearer),
    JS_CFUNC_DEF("parseBearerHeader", 1, oauth_parse_bearer),
    JS_CFUNC_DEF("parseBearerFromRequest", 1, oauth_parse_bearer_from_req),
    JS_CFUNC_DEF("isValidBearerToken", 1, oauth_is_valid_bearer),
    JS_CFUNC_DEF("buildWWWAuthenticate", 1, oauth_build_www),
    JS_CFUNC_DEF("isValidRedirectUri", 2, oauth_is_valid_redirect),
    JS_CFUNC_DEF("buildAuthorizationUrl", 1, oauth_build_auth_url),
    JS_CFUNC_DEF("parseAuthorizationResponse", 1, oauth_parse_auth_resp),
    JS_CFUNC_DEF("buildTokenRequestBody", 1, oauth_build_token_body),
    JS_CFUNC_DEF("parseTokenResponse", 1, oauth_parse_token_resp),
    JS_CFUNC_DEF("buildClientAuthHeader", 1, oauth_build_client_auth),
    JS_CFUNC_DEF("verifyJWT", 3, oauth_verify_jwt),
};

static int oauth_init(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, oauth_funcs, countof(oauth_funcs));
}

int js_nat_init_oauth2(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:oauth2", oauth_init);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, oauth_funcs, countof(oauth_funcs));
}

#else
int js_nat_init_oauth2(JSContext* ctx)
{
    (void)ctx;
    return 0;
}

#endif
