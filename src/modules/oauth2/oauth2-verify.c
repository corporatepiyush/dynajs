#include "oauth2-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

#define OAUTH_JWT_HOLDER "__oauth_jwt_verify"

JSValue oauth_verify_challenge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char *ver = NULL, *chal = NULL, *meth = NULL;
    size_t vn = 0, cn = 0;
    int is_s256 = 1, ok = 0;
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "verifyCodeChallenge(verifier, challenge, method)");
    ver = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!ver)
        return JS_EXCEPTION;
    chal = JS_ToCStringLen(ctx, &cn, argv[1]);
    if (!chal) {
        JS_FreeCString(ctx, ver);
        return JS_EXCEPTION;
    }
    meth = JS_ToCString(ctx, argv[2]);
    if (!meth) {
        JS_FreeCString(ctx, ver);
        JS_FreeCString(ctx, chal);
        return JS_EXCEPTION;
    }
    if (!strcmp(meth, "plain"))
        is_s256 = 0;
    else if (!strcmp(meth, "S256"))
        is_s256 = 1;
    else {
        JS_FreeCString(ctx, ver);
        JS_FreeCString(ctx, chal);
        JS_FreeCString(ctx, meth);
        return JS_ThrowTypeError(ctx, "method must be S256 or plain");
    }
    if (is_s256) {
        uint8_t digest[32];
        JSValue enc;
        const char* es;
        size_t el;
        dyn_sha256((const uint8_t*)ver, vn, digest);
        enc = b64url_new(ctx, digest, 32);
        if (JS_IsException(enc)) {
            JS_FreeCString(ctx, ver);
            JS_FreeCString(ctx, chal);
            JS_FreeCString(ctx, meth);
            return JS_EXCEPTION;
        }
        es = JS_ToCStringLen(ctx, &el, enc);
        JS_FreeValue(ctx, enc);
        if (!es) {
            JS_FreeCString(ctx, ver);
            JS_FreeCString(ctx, chal);
            JS_FreeCString(ctx, meth);
            return JS_EXCEPTION;
        }
        ok = (el == cn && dyn_ct_equal((const uint8_t*)es, (const uint8_t*)chal, el));
        JS_FreeCString(ctx, es);
    } else {
        ok = (vn == cn && dyn_ct_equal((const uint8_t*)ver, (const uint8_t*)chal, vn));
    }
    JS_FreeCString(ctx, ver);
    JS_FreeCString(ctx, chal);
    JS_FreeCString(ctx, meth);
    return JS_NewBool(ctx, ok);
}

static JSValue oauth_get_jwt_verify(JSContext* ctx)
{
    JSValue g, holder, v, r;
    const char* src = "import * as m from \"dyna:crypto\";\n"
                      "Object.defineProperty(globalThis, \"" OAUTH_JWT_HOLDER "\",\n"
                      "  { value: m.JWTVerify, writable: false, enumerable: false, configurable: false });";
    {
        JSPropertyDescriptor d;
        JSAtom a = JS_NewAtom(ctx, OAUTH_JWT_HOLDER);
        int has;
        if (a == JS_ATOM_NULL)
            return JS_EXCEPTION;
        g = JS_GetGlobalObject(ctx);
        has = JS_GetOwnProperty(ctx, &d, g, a);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, g);
        if (has < 0)
            return JS_EXCEPTION;
        if (has) {
            JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter);
            if (!(d.flags & (JS_PROP_GETSET | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) && JS_IsFunction(ctx, d.value))
                return d.value;
            JS_FreeValue(ctx, d.value);
        }
    }
    r = JS_Eval(ctx, src, strlen(src), "<oauth-jwt-bridge>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        return r;
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v))
        return v;
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_DupValue(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, v);
    g = JS_GetGlobalObject(ctx);
    holder = JS_GetPropertyStr(ctx, g, OAUTH_JWT_HOLDER);
    JS_FreeValue(ctx, g);
    return holder;
}

static const char* const oauth_verify_keys[] = { "algorithms", "aud", "iss", "requiredScope", "clockSkewSec", "requireExp" };

JSValue oauth_verify_jwt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue jwtVerify = JS_UNDEFINED, payload = JS_UNDEFINED, opts = JS_UNDEFINED, v = JS_UNDEFINED;
    int64_t skew = 0;
    time_t now = time(NULL);
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "verifyJWT(token, key, {algorithms, aud, iss, requiredScope, clockSkewSec})");
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "verifyJWT: token must be string");
    if (argc < 3 || !JS_IsObject(argv[2]))
        return JS_ThrowTypeError(ctx, "verifyJWT: opts with algorithms required");
    opts = argv[2];
    if (dyn_opts_strict(ctx, opts, oauth_verify_keys, countof(oauth_verify_keys)) < 0)
        return JS_EXCEPTION;
    if (dyn_get(ctx, opts, "algorithms", &v))
        return JS_EXCEPTION;
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "verifyJWT: algorithms must be array");
    }
    {
        JSValue lenv = JS_GetPropertyStr(ctx, v, "length");
        uint32_t n = 0;
        if (JS_IsException(lenv) || JS_ToUint32(ctx, &n, lenv)) {
            JS_FreeValue(ctx, lenv);
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            const char* es;
            int none;
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!JS_IsString(e)) {
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "verifyJWT: algorithms[%u] must be a string", (unsigned)i);
            }
            es = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (!es) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            none = !strcasecmp(es, "none");
            JS_FreeCString(ctx, es);
            if (none) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "verifyJWT: alg:none not allowed");
            }
        }
    }
    JS_FreeValue(ctx, v);
    if (dyn_get(ctx, opts, "clockSkewSec", &v))
        return JS_EXCEPTION;
    if (JS_IsNumber(v)) {
        if (JS_ToInt64(ctx, &skew, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, v);
    if (skew < 0 || skew > 300)
        return JS_ThrowRangeError(ctx, "verifyJWT: clockSkewSec must be 0..300");
    jwtVerify = oauth_get_jwt_verify(ctx);
    if (JS_IsException(jwtVerify))
        return jwtVerify;
    if (!JS_IsFunction(ctx, jwtVerify)) {
        JS_FreeValue(ctx, jwtVerify);
        return JS_ThrowTypeError(ctx, "verifyJWT: JWT not available: it needs the optional \"crypto\" module (built with OpenSSL)");
    }
    {
        JSValue jopts = JS_NewObject(ctx);
        JSValue algs;
        if (JS_IsException(jopts)) {
            JS_FreeValue(ctx, jwtVerify);
            return JS_EXCEPTION;
        }
        algs = JS_GetPropertyStr(ctx, opts, "algorithms");
        if (JS_IsException(algs) || JS_SetPropertyStr(ctx, jopts, "algorithms", algs) < 0) {
            JS_FreeValue(ctx, jopts);
            JS_FreeValue(ctx, jwtVerify);
            return JS_EXCEPTION;
        }
        JSValueConst args[3] = { argv[0], argv[1], jopts };
        payload = JS_Call(ctx, jwtVerify, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, jopts);
        JS_FreeValue(ctx, jwtVerify);
        if (JS_IsException(payload))
            return payload;
    }
    if (!JS_IsObject(payload) || JS_IsArray(ctx, payload)) {
        JS_FreeValue(ctx, payload);
        return JS_ThrowTypeError(ctx, "verifyJWT: payload must be a JSON object (RFC 7519 §4)");
    }
    {
        int req = dyn_get_truthy(ctx, opts, "requireExp");
        if (req < 0) {
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, payload, "exp");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        if (req && !JS_IsNumber(v)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx,
                "verifyJWT: requireExp is set and the token carries no numeric exp");
        }
    }
    if (JS_IsNumber(v)) {
        double expd;
        if (JS_ToFloat64(ctx, &expd, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        if (!isfinite(expd)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: exp must be finite");
        }
        if ((double)now >= expd + skew) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: token expired");
        }
    } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, payload);
        return JS_ThrowTypeError(ctx, "verifyJWT: exp must be number");
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, payload, "nbf");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, payload);
        return JS_EXCEPTION;
    }
    if (JS_IsNumber(v)) {
        double nbfd;
        if (JS_ToFloat64(ctx, &nbfd, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        if (!isfinite(nbfd)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: nbf must be finite");
        }
        if (nbfd > (double)now + skew) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: token not yet valid (nbf)");
        }
    } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, payload);
        return JS_ThrowTypeError(ctx, "verifyJWT: nbf must be number");
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "aud");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, payload);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        char **want = NULL, **have = NULL;
        uint32_t nw = 0, nh = 0, i, j;
        int ok = 0;
        JSValue tokAud;
        if (oauth_str_list(ctx, v, "verifyJWT: aud", &want, &nw)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        tokAud = JS_GetPropertyStr(ctx, payload, "aud");
        if (JS_IsException(tokAud)
            || (!JS_IsUndefined(tokAud) && !JS_IsNull(tokAud)
                && oauth_str_list(ctx, tokAud, "verifyJWT: the token's aud", &have, &nh))) {
            JS_FreeValue(ctx, tokAud);
            oauth_str_list_free(want, nw);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, tokAud);
        for (i = 0; i < nh && !ok; i++)
            for (j = 0; j < nw && !ok; j++)
                ok = !strcmp(have[i], want[j]);
        oauth_str_list_free(want, nw);
        oauth_str_list_free(have, nh);
        if (!ok) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: aud mismatch");
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "iss");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, payload);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, payload);
        return JS_ThrowTypeError(ctx,
            "verifyJWT: iss must be a string when set");
    }
    if (JS_IsString(v)) {
        JSValue tokIss = JS_GetPropertyStr(ctx, payload, "iss");
        int ok = 0;
        if (JS_IsException(tokIss)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        if (JS_IsString(tokIss)) {
            const char* a = JS_ToCString(ctx, tokIss);
            const char* b = JS_ToCString(ctx, v);
            ok = a && b && !strcmp(a, b);
            if (a)
                JS_FreeCString(ctx, a);
            if (b)
                JS_FreeCString(ctx, b);
        }
        JS_FreeValue(ctx, tokIss);
        if (!ok) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_ThrowTypeError(ctx, "verifyJWT: iss mismatch");
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "requiredScope");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, payload);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, payload);
        return JS_ThrowTypeError(ctx, "verifyJWT: requiredScope must be an array of strings");
    }
    if (JS_IsArray(ctx, v)) {
        JSValue tokScope = JS_GetPropertyStr(ctx, payload, "scope");
        const char* scopeStr = NULL;
        JSValue lenv;
        uint32_t n = 0;
        if (JS_IsException(tokScope)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        if (JS_IsString(tokScope))
            scopeStr = JS_ToCString(ctx, tokScope);
        lenv = JS_IsString(tokScope) && !scopeStr ? JS_EXCEPTION : JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lenv) || JS_ToUint32(ctx, &n, lenv)) {
            JS_FreeValue(ctx, lenv);
            if (scopeStr)
                JS_FreeCString(ctx, scopeStr);
            JS_FreeValue(ctx, tokScope);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, payload);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            const char* need = NULL;
            int found = 0;
            if (!JS_IsException(e) && JS_IsString(e))
                need = JS_ToCString(ctx, e);
            if (!need) {
                int was_exc = JS_IsException(e) || JS_IsString(e);
                JS_FreeValue(ctx, e);
                if (scopeStr)
                    JS_FreeCString(ctx, scopeStr);
                JS_FreeValue(ctx, tokScope);
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, payload);
                return was_exc ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "verifyJWT: requiredScope[%u] must be a string", (unsigned)i);
            }
            if (scopeStr) {
                size_t sl = strlen(scopeStr), off = 0;
                while (off < sl) {
                    while (off < sl && scopeStr[off] == ' ')
                        off++;
                    size_t b = off;
                    while (off < sl && scopeStr[off] != ' ')
                        off++;
                    size_t l = off - b;
                    if (l && need && strlen(need) == l && !memcmp(need, scopeStr + b, l))
                        found = 1;
                }
            }
            if (need)
                JS_FreeCString(ctx, need);
            JS_FreeValue(ctx, e);
            if (!found) {
                if (scopeStr)
                    JS_FreeCString(ctx, scopeStr);
                JS_FreeValue(ctx, tokScope);
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, payload);
                return JS_ThrowTypeError(ctx, "verifyJWT: required scope missing");
            }
        }
        if (scopeStr)
            JS_FreeCString(ctx, scopeStr);
        JS_FreeValue(ctx, tokScope);
    }
    JS_FreeValue(ctx, v);
    return payload;
}

#endif
