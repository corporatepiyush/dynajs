#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

JSValue dyn_bcrypt_hash_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* pw;
    uint8_t pwc[72];
    size_t pwlen;
    const char* powned;
    int64_t rounds = 10;
    uint8_t salt[16];
    char out[61];
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Bcrypt.hash(password[, rounds])");
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &rounds, argv[1]))
            return JS_EXCEPTION;
    }
    if (rounds < DYN_BCRYPT_MIN_COST || rounds > DYN_BCRYPT_MAX_COST)
        return JS_ThrowRangeError(ctx, "Bcrypt.hash: rounds must be 4..31");
    if (dyn_crypto_data(ctx, argv[0], &pw, &pwlen, &powned))
        return JS_EXCEPTION;
    if (pwlen > 72) {
        if (powned)
            JS_FreeCString(ctx, powned);
        return JS_ThrowRangeError(ctx,
            "Bcrypt.hash: password exceeds 72 bytes; bcrypt's limit is 72 "
            "and truncating silently would hash a different password");
    }
    if (dyn_os_entropy(salt, 16) < 0) {
        if (powned)
            JS_FreeCString(ctx, powned);
        return JS_ThrowInternalError(ctx, "Bcrypt.hash: OS entropy unavailable");
    }
    if (pwlen)
        memcpy(pwc, pw, pwlen);
    (void)dyn_kdf_begin(ctx, 0, 0);
    (void)DYN_OFFLOAD_CPU(dyn_bcrypt_core(pwc, pwlen, salt, (uint32_t)rounds, out));
    memset(pwc, 0, sizeof pwc);
    memset(salt, 0, sizeof salt);
    if (powned)
        JS_FreeCString(ctx, powned);
    return JS_NewString(ctx, out);
}

JSValue dyn_bcrypt_verify_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* pw;
    uint8_t pwc[72];
    size_t pwlen;
    const char* powned;
    const char* hash = NULL;
    size_t hlen = 0;
    uint8_t salt[16];
    char out[61];
    uint32_t logr;
    int r = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Bcrypt.verify(password, hash)");
    hash = JS_ToCStringLen(ctx, &hlen, argv[1]);
    if (!hash)
        return JS_EXCEPTION;
    if (dyn_crypto_data(ctx, argv[0], &pw, &pwlen, &powned)) {
        JS_FreeCString(ctx, hash);
        return JS_EXCEPTION;
    }
    if (pwlen <= 72 && dyn_bcrypt_parse(hash, hlen, &logr, salt) == 0) {
        if (pwlen)
            memcpy(pwc, pw, pwlen);
        (void)dyn_kdf_begin(ctx, 0, 0);
        if (DYN_OFFLOAD_CPU(dyn_bcrypt_core(pwc, pwlen, salt, logr, out)) == 0)
            r = dyn_ct_equal((const uint8_t*)out + 7, (const uint8_t*)hash + 7, 53);
        memset(pwc, 0, sizeof pwc);
    }
    memset(salt, 0, sizeof salt);
    JS_FreeCString(ctx, hash);
    if (powned)
        JS_FreeCString(ctx, powned);
    return JS_NewBool(ctx, r);
}

#endif
