#include "oauth2-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OAUTH2)

JSValue oauth_gen_verifier(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint8_t raw[32];
    JSValue r;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (dyn_os_entropy(raw, sizeof raw) < 0)
        return JS_ThrowInternalError(ctx, "generateCodeVerifier: OS entropy unavailable");
    r = b64url_new(ctx, raw, sizeof raw);
    oauth_wipe(raw, sizeof raw);
    return r;
}

JSValue oauth_gen_challenge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char *ver = NULL, *meth = NULL;
    size_t vn = 0;
    int is_s256 = 1;
    JSValue r = JS_EXCEPTION;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "generateCodeChallenge(verifier[, method])");
    ver = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!ver)
        return JS_EXCEPTION;
    if (vn < 43 || vn > 128) {
        JS_FreeCString(ctx, ver);
        return JS_ThrowRangeError(ctx, "code_verifier must be 43..128 unreserved chars");
    }
    {
        size_t i;
        for (i = 0; i < vn; i++)
            if (!is_unreserved((unsigned char)ver[i])) {
                JS_FreeCString(ctx, ver);
                return JS_ThrowTypeError(ctx, "code_verifier charset must be unreserved");
            }
    }
    if (argc > 1 && JS_IsString(argv[1])) {
        meth = JS_ToCString(ctx, argv[1]);
        if (!meth) {
            JS_FreeCString(ctx, ver);
            return JS_EXCEPTION;
        }
        if (!strcmp(meth, "plain"))
            is_s256 = 0;
        else if (!strcmp(meth, "S256"))
            is_s256 = 1;
        else {
            JS_FreeCString(ctx, ver);
            JS_FreeCString(ctx, meth);
            return JS_ThrowTypeError(ctx, "code_challenge_method must be S256 or plain");
        }
    }
    if (is_s256) {
        uint8_t digest[32];
        dyn_sha256((const uint8_t*)ver, vn, digest);
        r = b64url_new(ctx, digest, 32);
    } else {
        r = JS_NewStringLen(ctx, ver, vn);
    }
    JS_FreeCString(ctx, ver);
    if (meth)
        JS_FreeCString(ctx, meth);
    return r;
}

JSValue oauth_gen_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t n = 32;
    uint8_t* buf;
    JSValue r;
    (void)this_val;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &n, argv[0]))
            return JS_EXCEPTION;
    }
    if (n < 1 || n > 256)
        return JS_ThrowRangeError(ctx, "generateState: bytes must be 1..256");
    buf = (uint8_t*)js_malloc(ctx, (size_t)n);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_os_entropy(buf, (size_t)n) < 0) {
        js_free(ctx, buf);
        return JS_ThrowInternalError(ctx, "OS entropy unavailable");
    }
    r = b64url_new(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return r;
}

#endif
